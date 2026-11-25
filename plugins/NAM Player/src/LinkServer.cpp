// LinkServer.cpp - see LinkServer.h.

#include "LinkServer.h"

#include "LinkPage.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <system_error>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace namplayer {

namespace fs = std::filesystem;

namespace {

constexpr size_t    kMaxHeader      = 8192;
constexpr long long kMaxBody        = 16LL * 1024 * 1024;
constexpr int       kTimeoutSeconds = 3;
constexpr size_t    kNameLimit      = 24;

// A write to a connection the other side has closed must come back as an
// error, never as SIGPIPE: that signal would end the whole MPC application.
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;   // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

bool sendAll(int fd, const char* data, size_t size)
{
    while (size > 0)
    {
        const ssize_t n = send(fd, data, size, kSendFlags);
        if (n <= 0) return false;
        data += n;
        size -= (size_t)n;
    }
    return true;
}

void respond(int fd, int status, const char* type, const std::string& body)
{
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 403 ? "Forbidden"
                       : status == 404 ? "Not Found" : status == 409 ? "Conflict" : status == 413 ? "Payload Too Large" : "Error";
    char head[256];
    const int n = std::snprintf(head, sizeof(head),
                                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                                "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                status, reason, type, body.size());
    if (sendAll(fd, head, (size_t)n)) sendAll(fd, body.data(), body.size());
}

void fail(int fd, int status, const std::string& message)
{
    respond(fd, status, "application/json", "{\"error\":" + namplayer::jsonString(message) + "}");
}

std::string urlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2]))
        {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        }
        else
            out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool endsWithNam(const std::string& name)
{
    return name.size() > 4 && lower(name.substr(name.size() - 4)) == ".nam";
}

// A relative folder such as "Marshall/Clean": no "..", no absolute paths, no backslashes.
bool safeRelative(const std::string& path)
{
    if (path.empty()) return true;
    if (path[0] == '/' || path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) return false;
    size_t start = 0;
    while (start <= path.size())
    {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part[0] == '.') return false;
        start = end + 1;
    }
    return true;
}

bool sameContent(const std::string& a, const std::string& b)
{
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::FILE* fa = std::fopen(a.c_str(), "rb");
    std::FILE* fb = std::fopen(b.c_str(), "rb");
    bool same = fa != nullptr && fb != nullptr;
    char ba[8192], bb[8192];
    while (same)
    {
        const size_t na = std::fread(ba, 1, sizeof(ba), fa), nb = std::fread(bb, 1, sizeof(bb), fb);
        if (na != nb || std::memcmp(ba, bb, na) != 0) same = false;
        if (na == 0) break;
    }
    if (fa) std::fclose(fa);
    if (fb) std::fclose(fb);
    return same;
}

bool usbInterfaceName(const char* name)
{
    for (const char* prefix : {"usb", "rndis", "ncm", "ecm"})
        if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) return true;
    return false;
}

int linkPort()
{
    if (const char* env = std::getenv("NAM_PLAYER_LINK_PORT"))
    {
        const int port = std::atoi(env);
        if (port > 0 && port < 65536) return port;
    }
    return kLinkPort;
}

struct Address
{
    sockaddr_storage storage{};
    socklen_t        length = 0;
    std::string      text;
};

void addAddress(std::vector<Address>& out, const sockaddr* sa, int port)
{
    Address a;
    char text[INET6_ADDRSTRLEN] = {0};
    if (sa->sa_family == AF_INET)
    {
        sockaddr_in in = *reinterpret_cast<const sockaddr_in*>(sa);
        in.sin_port = htons((uint16_t)port);
        std::memcpy(&a.storage, &in, sizeof(in));
        a.length = sizeof(in);
        inet_ntop(AF_INET, &in.sin_addr, text, sizeof(text));
        a.text = text;
    }
    else if (sa->sa_family == AF_INET6)
    {
        sockaddr_in6 in = *reinterpret_cast<const sockaddr_in6*>(sa);
        in.sin6_port = htons((uint16_t)port);
        std::memcpy(&a.storage, &in, sizeof(in));
        a.length = sizeof(in);
        inet_ntop(AF_INET6, &in.sin6_addr, text, sizeof(text));
        a.text = std::string(text) + "%" + std::to_string(in.sin6_scope_id);
    }
    else
        return;
    out.push_back(a);
}

// The addresses to listen on right now. Interfaces come and go (the cable is
// plugged in, Developer Mode is switched on), so this is asked again every 2 s.
std::vector<Address> wantedAddresses()
{
    std::vector<Address> out;
    const int   port = linkPort();
    const char* bind = std::getenv("NAM_PLAYER_LINK_BIND");
    if (bind != nullptr && bind[0] != '\0' && std::strcmp(bind, "usb") != 0)
    {
        sockaddr_in in{};
        in.sin_family = AF_INET;
        if (std::strcmp(bind, "off") != 0 && inet_pton(AF_INET, bind, &in.sin_addr) == 1)
            addAddress(out, reinterpret_cast<const sockaddr*>(&in), port);
        return out;
    }
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (const ifaddrs* i = list; i != nullptr; i = i->ifa_next)
    {
        if (i->ifa_addr == nullptr || i->ifa_name == nullptr) continue;
        if ((i->ifa_flags & IFF_UP) == 0 || (i->ifa_flags & IFF_LOOPBACK) != 0) continue;
        if (!usbInterfaceName(i->ifa_name)) continue;
        addAddress(out, i->ifa_addr, port);
    }
    freeifaddrs(list);
    return out;
}

} // namespace

std::string jsonString(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
        else out += (char)c;
    }
    return out + "\"";
}

struct LinkServer::Request
{
    std::string method, path, body;   // body: the part of it that arrived with the headers
    std::map<std::string, std::string> query, headers;
    long long contentLength = -1;
    long long received = 0;           // body bytes a handler has consumed so far

    std::string param(const char* key) const
    {
        const auto it = query.find(key);
        return it == query.end() ? std::string() : it->second;
    }
    bool trusted() const
    {
        const auto it = headers.find("x-nam-player");
        return it != headers.end() && it->second == "1";
    }
};

std::string LinkServer::safeName(const std::string& text, size_t limit)
{
    std::string out;
    for (unsigned char c : text)
    {
        const bool ok = std::isalnum(c) || std::strchr(" _+-().&'", c) != nullptr;
        const char ch = (ok && c != 0) ? (char)c : ' ';
        if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;   // collapse runs of spaces
        out += ch;
    }
    const auto trim = [&out] {
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
        size_t lead = 0;
        while (lead < out.size() && (out[lead] == ' ' || out[lead] == '.')) ++lead;
        out.erase(0, lead);
    };
    trim();
    if (out.size() > limit) { out.resize(limit); trim(); }
    return out;
}

void LinkServer::start()
{
    if (running_) return;
    const char* bind = std::getenv("NAM_PLAYER_LINK_BIND");
    if (bind != nullptr && std::strcmp(bind, "off") == 0) return;
    stop_.store(false);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    sched_param param{};
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &param);
    running_ = pthread_create(&thread_, &attr, &entry, this) == 0;
    pthread_attr_destroy(&attr);
}

void LinkServer::stop()
{
    if (!running_) return;
    stop_.store(true);
    pthread_join(thread_, nullptr);
    running_ = false;
}

void* LinkServer::entry(void* self)
{
    static_cast<LinkServer*>(self)->run();
    return nullptr;
}

void LinkServer::run()
{
    int ticks = 0;
    while (!stop_.load())
    {
        if (ticks++ % 8 == 0) refreshListeners();   // every 2 s

        std::vector<pollfd> fds;
        for (const Listener& l : listeners_) fds.push_back({l.fd, POLLIN, 0});
        if (poll(fds.empty() ? nullptr : fds.data(), (nfds_t)fds.size(), 250) <= 0) continue;

        for (const pollfd& p : fds)
        {
            if ((p.revents & POLLIN) == 0 || stop_.load()) continue;
            const int client = accept(p.fd, nullptr, nullptr);
            if (client < 0) continue;
            fcntl(client, F_SETFD, FD_CLOEXEC);
            fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) & ~O_NONBLOCK);
            timeval timeout{kTimeoutSeconds, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
            const int on = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
            serve(client);
            shutdown(client, SHUT_RDWR);
            close(client);
        }
    }
    closeListeners();
}

void LinkServer::closeListeners()
{
    for (Listener& l : listeners_) close(l.fd);
    listeners_.clear();
}

void LinkServer::refreshListeners()
{
    const std::vector<Address> wanted = wantedAddresses();

    // Drop listeners whose address has gone away.
    for (size_t i = 0; i < listeners_.size();)
    {
        const bool keep = std::any_of(wanted.begin(), wanted.end(),
                                      [&](const Address& a) { return a.text == listeners_[i].address; });
        if (keep) { ++i; continue; }
        close(listeners_[i].fd);
        listeners_.erase(listeners_.begin() + (std::ptrdiff_t)i);
    }

    for (const Address& a : wanted)
    {
        if (std::any_of(listeners_.begin(), listeners_.end(), [&](const Listener& l) { return l.address == a.text; }))
            continue;
        const int fd = socket(a.storage.ss_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        const int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (a.storage.ss_family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        // A second NAM Player instance finds the port taken and simply has no link.
        if (bind(fd, reinterpret_cast<const sockaddr*>(&a.storage), a.length) != 0 || listen(fd, 4) != 0)
        {
            close(fd);
            continue;
        }
        listeners_.push_back({fd, a.text});
    }
}

void LinkServer::serve(int client)
{
    // Headers.
    std::string data;
    size_t      headerEnd = std::string::npos;
    char        buf[4096];
    while (headerEnd == std::string::npos && data.size() < kMaxHeader)
    {
        const ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) return;
        data.append(buf, (size_t)n);
        headerEnd = data.find("\r\n\r\n");
    }
    if (headerEnd == std::string::npos) return fail(client, 400, "request too long");

    Request request;
    request.body = data.substr(headerEnd + 4);
    const std::string head = data.substr(0, headerEnd);

    size_t lineEnd = head.find("\r\n");
    const std::string first = head.substr(0, lineEnd);
    const size_t sp1 = first.find(' '), sp2 = first.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) return fail(client, 400, "bad request");
    request.method = first.substr(0, sp1);
    std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);

    const size_t qmark = target.find('?');
    request.path = urlDecode(target.substr(0, qmark));
    if (qmark != std::string::npos)
    {
        const std::string qs = target.substr(qmark + 1);
        size_t start = 0;
        while (start < qs.size())
        {
            size_t end = qs.find('&', start);
            if (end == std::string::npos) end = qs.size();
            const std::string pair = qs.substr(start, end - start);
            const size_t eq = pair.find('=');
            if (eq != std::string::npos) request.query[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            start = end + 1;
        }
    }
    while (lineEnd != std::string::npos)
    {
        const size_t start = lineEnd + 2;
        lineEnd = head.find("\r\n", start);
        const std::string line = head.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        size_t v = colon + 1;
        while (v < line.size() && line[v] == ' ') ++v;
        request.headers[lower(line.substr(0, colon))] = line.substr(v);
    }
    const auto length = request.headers.find("content-length");
    if (length != request.headers.end()) request.contentLength = std::atoll(length->second.c_str());

    handle(client, request);

    // If the request was turned down before its body was read (wrong header, file
    // too large, ...), read the rest of it anyway. Closing with unread data makes
    // the sender see a broken connection instead of the answer explaining why.
    long long remaining = request.contentLength - request.received - (long long)request.body.size();
    if (remaining > kMaxBody) remaining = kMaxBody;
    while (remaining > 0 && !stop_.load())
    {
        const ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        remaining -= n;
    }
}

void LinkServer::handle(int client, Request& request)
{
    const std::string& m = request.method;
    const std::string& p = request.path;

    if (m == "GET" && p == "/") return respond(client, 200, "text/html; charset=utf-8", kLinkPage);
    if (m == "GET" && p == "/api/status") return respond(client, 200, "application/json", host_.linkStatusJson());
    if (m == "GET" && p == "/catalog.txt")
    {
        std::string text;
        if (std::FILE* f = std::fopen(host_.linkContentPath("Link/community.txt").c_str(), "rb"))
        {
            char buf[8192];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 1024 * 1024) text.append(buf, n);
            std::fclose(f);
        }
        return respond(client, 200, "text/plain; charset=utf-8", text);
    }

    // --- TONE3000 sign-in --------------------------------------------------------
    if (m == "GET" && p == "/t3k/callback")
    {
        // TONE3000 sends the browser here when the user has signed in and picked a tone.
        // Only the page that started the flow holds the PKCE secret that makes the code usable.
        parkedState_ = request.param("state").substr(0, 128);
        parkedJson_  = "{\"code\":" + jsonString(request.param("code").substr(0, 2048)) +
                       ",\"tone_id\":" + jsonString(request.param("tone_id").substr(0, 32)) +
                       ",\"error\":" + jsonString(request.param("error").substr(0, 200)) +
                       ",\"canceled\":" + (request.param("canceled") == "true" ? "true" : "false") + "}";
        return respond(client, 200, "text/html; charset=utf-8",
                       "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                       "<body style='font:17px system-ui;margin:2em;background:#181613;color:#ece6d8'>"
                       "<p><b>NAM Player has what it needs.</b></p><p>Go back to the NAM Player page: it carries on by itself. "
                       "You can close this tab.</p><script>setTimeout(function(){window.close()},1200)</script>");
    }
    if (m == "GET" && p == "/api/t3k/poll")
    {
        if (parkedState_.empty() || parkedState_ != request.param("state"))
            return respond(client, 200, "application/json", "{\"pending\":true}");
        const std::string json = parkedJson_;
        parkedState_.clear();
        parkedJson_.clear();
        return respond(client, 200, "application/json", json);
    }
    if (m == "GET" && p == "/api/link")
    {
        // The IPv4 address this server listens on: TONE3000 must send the browser back to a
        // private network address, not to a name such as mpc-sample.local.
        std::string address;
        for (const Listener& l : listeners_)
            if (l.address.find(':') == std::string::npos) { address = l.address; break; }
        return respond(client, 200, "application/json",
                       "{\"address\":" + jsonString(address) + ",\"port\":" + std::to_string(linkPort()) + "}");
    }
    if (p == "/api/t3k/key" && (m == "GET" || m == "POST"))
    {
        const std::string root = host_.linkProfilesRoot();
        if (root.empty()) return fail(client, 409, "the plugin is not installed on a card");
        const std::string path = root + "/.tone3000-key";   // a dot file: the profile list ignores it
        if (m == "POST")
        {
            if (!request.trusted()) return fail(client, 403, "missing X-NAM-Player header");
            const std::string key = request.param("key");
            const bool ok = key.empty() || (key.compare(0, 8, "t3k_pub_") == 0 && key.size() <= 200 &&
                            std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }));
            if (!ok) return fail(client, 400, "that is not a publishable key: it starts with t3k_pub_ (never use the secret key)");
            std::error_code ec;
            fs::create_directories(root, ec);
            if (key.empty()) fs::remove(path, ec);
            else if (std::FILE* f = std::fopen(path.c_str(), "wb")) { std::fputs(key.c_str(), f); std::fclose(f); }
            else return fail(client, 409, "cannot write to the card");
            return respond(client, 200, "application/json", "{\"ok\":true}");
        }
        char buf[256] = {0};
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) { const size_t n = std::fread(buf, 1, 200, f); buf[n] = 0; std::fclose(f); }
        return respond(client, 200, "application/json", "{\"key\":" + jsonString(buf) + "}");
    }

    if (m == "GET" && p == "/api/capture.wav")
    {
        std::string wav;
        const int state = host_.linkCaptureGet(wav);
        if (state == 1) return respond(client, 200, "audio/wav", wav);
        return fail(client, state == 0 ? 409 : 404, state == 0 ? "still recording" : "nothing recorded yet");
    }

    // Everything below changes something, so it must come from the plugin's own
    // page or the companion tool. Browsers do not let other sites add this header.
    if (m == "PUT" || m == "POST" || m == "DELETE")
    {
        if (!request.trusted()) return fail(client, 403, "missing X-NAM-Player header");
        if (m == "PUT" && p == "/api/profile") return putProfile(client, request);
        if (m == "DELETE" && p == "/api/profile") return deleteProfile(client, request);
        if (m == "POST" && p == "/api/capture")
        {
            if (host_.linkCaptureStart(std::atof(request.param("seconds").c_str())))
                return respond(client, 200, "application/json", "{\"ok\":true}");
            return fail(client, 409, "cannot record right now");
        }
        if (m == "POST" && p == "/api/param")
        {
            const std::string value = request.param("value");
            char* end = nullptr;
            const double v = std::strtod(value.c_str(), &end);
            if (end != value.c_str() && v == v && host_.linkSetParam(request.param("id"), v))
                return respond(client, 200, "application/json", "{\"ok\":true}");
            return fail(client, 400, "no such control, or not a number");
        }
        if (m == "POST" && p == "/api/select")
        {
            if (host_.linkSelect(request.param("key"), request.param("file")))
                return respond(client, 200, "application/json", "{\"ok\":true}");
            return fail(client, 404, "no such profile");
        }
    }
    fail(client, 404, "not found");
}

void LinkServer::putProfile(int client, Request& request)
{
    const std::string root = host_.linkProfilesRoot();
    if (root.empty()) return fail(client, 409, "the plugin is not installed on a card, so there is nowhere to save profiles");
    if (request.contentLength <= 0) return fail(client, 400, "empty file");
    if (request.contentLength > kMaxBody) return fail(client, 413, "file too large (16 MB at most)");

    std::string folder = safeName(request.param("folder"), kNameLimit);
    std::string name   = request.param("name");
    if (endsWithNam(name)) name.resize(name.size() - 4);
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    name = safeName(name, kNameLimit);
    if (folder.empty()) folder = "My Profiles";
    if (name.empty()) name = "model";

    const auto expect = request.headers.find("expect");
    if (expect != request.headers.end() && lower(expect->second) == "100-continue")
        sendAll(client, "HTTP/1.1 100 Continue\r\n\r\n", 25);

    std::error_code ec;
    const fs::path dir = fs::path(root) / folder;
    fs::create_directories(dir, ec);
    const std::string part = (dir / ".upload.part").string();   // a dot file: the library ignores it
    std::FILE* out = std::fopen(part.c_str(), "wb");
    if (out == nullptr) return fail(client, 409, "cannot write to the card");

    // Stream the body to the card, checking on the way that it looks like a
    // NAM model: a JSON object with an "architecture" key.
    static const char kMarker[] = "\"architecture\"";
    const size_t markerSize = sizeof(kMarker) - 1;
    long long   received = 0;
    char        firstChar = 0;
    bool        hasMarker = false, ok = true;
    std::string tail;
    char        buf[16384];
    while (received < request.contentLength)
    {
        size_t n;
        if (!request.body.empty())
        {
            n = std::min(request.body.size(), sizeof(buf));
            std::memcpy(buf, request.body.data(), n);
            request.body.erase(0, n);
        }
        else
        {
            const ssize_t r = recv(client, buf, sizeof(buf), 0);
            if (r <= 0 || stop_.load()) { ok = false; break; }
            n = (size_t)r;
        }
        if ((long long)n > request.contentLength - received) n = (size_t)(request.contentLength - received);
        for (size_t i = 0; i < n && firstChar == 0; ++i)
            if (!std::isspace((unsigned char)buf[i])) firstChar = buf[i];
        if (!hasMarker)
        {
            tail.append(buf, n);
            hasMarker = tail.find(kMarker) != std::string::npos;
            if (tail.size() > markerSize) tail.erase(0, tail.size() - markerSize);
        }
        received += (long long)n;
        request.received = received;
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    ok = ok && std::fflush(out) == 0 && fsync(fileno(out)) == 0;
    std::fclose(out);

    // (fs::remove on the folder only succeeds if the failed upload was all it held.)
    if (!ok) { fs::remove(part, ec); fs::remove(dir, ec); return fail(client, 400, "the upload did not complete"); }
    if (firstChar != '{' || !hasMarker) { fs::remove(part, ec); fs::remove(dir, ec); return fail(client, 400, "not a .nam model file"); }

    // Never replace a different profile that happens to have the same name.
    std::string file;
    bool        alreadyThere = false;
    for (int n = 1; n < 100 && file.empty(); ++n)
    {
        const std::string candidate = (n == 1 ? name : name.substr(0, kNameLimit - 3) + " " + std::to_string(n)) + ".nam";
        const std::string path = (dir / candidate).string();
        if (!fs::exists(path, ec)) file = candidate;
        else if (sameContent(path, part)) { file = candidate; alreadyThere = true; }
    }
    if (file.empty()) { fs::remove(part, ec); return fail(client, 409, "too many profiles with this name"); }
    if (alreadyThere)
        fs::remove(part, ec);
    else
    {
        fs::rename(part, dir / file, ec);
        if (ec) { fs::remove(part, ec); return fail(client, 409, "cannot write to the card"); }
        // Keep the source, creator and licence beside the files.
        std::string about = request.param("about");
        if (!about.empty())
        {
            if (about.size() > 400) about.resize(400);
            for (char& c : about) if ((unsigned char)c < 0x20) c = ' ';
            if (std::FILE* f = std::fopen((dir / "ABOUT.txt").string().c_str(), "ab"))
            {
                std::fprintf(f, "%s\n    %s\n\n", file.c_str(), about.c_str());
                std::fclose(f);
            }
        }
    }

    host_.linkProfilesChanged(folder, request.param("select") == "1" ? file : std::string());
    respond(client, 200, "application/json",
            "{\"folder\":" + jsonString(folder) + ",\"file\":" + jsonString(file) +
            ",\"already_there\":" + (alreadyThere ? "true" : "false") + "}");
}

void LinkServer::deleteProfile(int client, Request& request)
{
    const std::string root = host_.linkProfilesRoot();
    const std::string folder = request.param("folder"), file = request.param("file");
    if (root.empty() || !safeRelative(folder) || file.empty() || file[0] == '.' ||
        file.find_first_of("/\\") != std::string::npos || !endsWithNam(file))
        return fail(client, 400, "not a profile on the card");

    std::error_code ec;
    const fs::path dir = folder.empty() ? fs::path(root) : fs::path(root) / folder;
    const fs::path path = dir / file;
    if (!fs::is_regular_file(path, ec)) return fail(client, 404, "no such profile");
    if (!fs::remove(path, ec)) return fail(client, 409, "cannot remove it from the card");

    // Tidy up a folder that has no profiles left.
    if (!folder.empty())
    {
        bool anyLeft = false;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (endsWithNam(it->path().filename().string())) anyLeft = true;
        if (!anyLeft)
        {
            fs::remove(dir / "ABOUT.txt", ec);
            fs::remove(dir, ec);   // only succeeds if it is empty
        }
    }
    host_.linkProfilesChanged(std::string(), std::string());
    respond(client, 200, "application/json", "{\"ok\":true}");
}

} // namespace namplayer
