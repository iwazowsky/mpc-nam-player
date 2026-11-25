// lifecycle.cpp - loads the plugin the way the MPC does: in real time, with its
// worker thread, and unloading the library afterwards.
//
//   lifecycle <plugin library> [quick cycles] [a .nam file, for the live-link check]
//
// 1. Real-time run (no test mode): feed a sine in 128-frame blocks at real-time
//    pace and check that the background loader delivers a model, that switching
//    profiles works, and that the model is still running at the end.
// 2. Live link: by default nothing listens (the link binds to USB network
//    interfaces only). Told to use 127.0.0.1, the plugin's web server answers,
//    takes a profile, and the plugin starts playing it.
// 3. Load / instantiate / close / unload the library many times. Anything the
//    plugin leaves behind in the process (thread-local keys, exit handlers
//    pointing into unloaded code) shows up here or when this program exits.

#include "mpc_vst2_abi.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static VstIntPtr hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float)
{
    return opcode == audioMasterVersion ? 2400 : 0;
}

typedef AEffect* (*Entry)(audioMasterCallback);

static AEffect* create(const char* path, void*& lib)
{
    lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return nullptr; }
    Entry entry = (Entry)dlsym(lib, "VSTPluginMain");
    return entry ? entry(hostCallback) : nullptr;
}

static void destroy(AEffect* fx, void* lib)
{
    fx->dispatcher(fx, effMainsChanged, 0, 0, nullptr, 0.0f);
    fx->dispatcher(fx, effClose, 0, 0, nullptr, 0.0f);
    dlclose(lib);
}

// Processes `seconds` of a 220 Hz sine in real time. Returns how different the
// last 0.1 s of output is from the input (0 = the plugin is passing audio through).
static double runRealTime(AEffect* fx, double seconds, long& frame)
{
    const int block = 128;
    const double rate = 44100.0;
    std::vector<float> l(block), r(block), ref;
    std::vector<float> out;
    const long total = (long)(seconds * rate);
    auto next = std::chrono::steady_clock::now();
    for (long done = 0; done < total; done += block)
    {
        for (int i = 0; i < block; ++i)
        {
            const float v = 0.25f * (float)std::sin(2.0 * 3.14159265358979 * 220.0 * (double)(frame + i) / rate);
            l[(size_t)i] = r[(size_t)i] = v;
            ref.push_back(0.25f * (float)std::sin(2.0 * 3.14159265358979 * 220.0 * (double)(frame + i - 64) / rate));
        }
        float* bufs[2] = {l.data(), r.data()};
        fx->processReplacing(fx, bufs, bufs, block);
        out.insert(out.end(), l.begin(), l.end());
        frame += block;
        next += std::chrono::microseconds((long)(1e6 * block / rate));
        std::this_thread::sleep_until(next);
    }
    double diff = 0.0, sig = 0.0;
    const size_t tail = (size_t)(0.1 * rate);
    for (size_t i = out.size() - tail; i < out.size(); ++i)
    {
        diff += (double)(out[i] - ref[i]) * (out[i] - ref[i]);
        sig += (double)ref[i] * ref[i];
    }
    return std::sqrt(diff / sig);
}

// One HTTP request to 127.0.0.1. Returns the whole response, or "" if nothing is listening.
static std::string http(int port, const std::string& head, const std::string& body = std::string())
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    std::string out;
    if (connect(fd, (const sockaddr*)&addr, sizeof(addr)) == 0)
    {
        const std::string request = head + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < request.size())
        {
            const ssize_t n = send(fd, request.data() + sent, request.size() - sent, 0);
            if (n <= 0) break;
            sent += (size_t)n;
        }
        char buf[4096];
        ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) out.append(buf, (size_t)n);
    }
    close(fd);
    return out;
}

static AEffect* createPrepared(const char* path, void*& lib)
{
    AEffect* fx = create(path, lib);
    if (!fx) return nullptr;
    fx->dispatcher(fx, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    fx->dispatcher(fx, effSetBlockSize, 0, 128, nullptr, 0.0f);
    fx->dispatcher(fx, effOpen, 0, 0, nullptr, 0.0f);
    fx->setParameter(fx, 5, 0.0f);
    fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0.0f);
    return fx;
}

static int linkCheck(const char* path, const char* modelPath)
{
    const int port = 19627;
    int failures = 0;
    long frame = 0;

    void* lib = nullptr;
    AEffect* fx = createPrepared(path, lib);
    if (!fx) return 1;
    runRealTime(fx, 0.6, frame);
    const bool silent = http(9626, "GET /api/status HTTP/1.1").empty() && http(port, "GET /api/status HTTP/1.1").empty();
    std::printf("%s  live link: nothing listens unless there is a USB network interface\n", silent ? "PASS" : "FAIL");
    failures += silent ? 0 : 1;
    destroy(fx, lib);

    std::string model;
    if (FILE* f = std::fopen(modelPath, "rb"))
    {
        char buf[8192]; size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) model.append(buf, n);
        std::fclose(f);
    }
    setenv("NAM_PLAYER_LINK_BIND", "127.0.0.1", 1);
    setenv("NAM_PLAYER_LINK_PORT", std::to_string(port).c_str(), 1);
    fx = createPrepared(path, lib);
    if (!fx) return 1;
    const double before = runRealTime(fx, 0.8, frame);
    const std::string status = http(port, "GET /api/status HTTP/1.1");
    const std::string denied = http(port, "PUT /api/profile?folder=Link%20test&name=sent.nam&select=1 HTTP/1.1", model);
    const std::string put = http(port, "PUT /api/profile?folder=Link%20test&name=sent.nam&select=1 HTTP/1.1\r\nX-NAM-Player: 1", model);
    const double after = runRealTime(fx, 1.0, frame);
    const std::string now = http(port, "GET /api/status HTTP/1.1");
    const std::string del = http(port, "DELETE /api/profile?folder=Link%20test&file=sent.nam HTTP/1.1\r\nX-NAM-Player: 1");
    const bool ok = status.find("\"plugin\":\"NAM Player\"") != std::string::npos && status.find("\"uploads\":true") != std::string::npos
                    && denied.find(" 403 ") != std::string::npos && put.find(" 200 ") != std::string::npos
                    && now.find("\"file\":\"sent.nam\",\"name\":\"sent\"") != std::string::npos
                    && del.find(" 200 ") != std::string::npos && std::fabs(before - after) > 1e-4;
    std::printf("%s  live link: a profile sent over HTTP is saved to the card and starts playing (output changed %.3f -> %.3f)\n",
                ok ? "PASS" : "FAIL", before, after);
    if (!ok) std::printf("      status: %.120s\n      put: %.160s\n", status.c_str(), put.c_str());
    failures += ok ? 0 : 1;
    destroy(fx, lib);
    unsetenv("NAM_PLAYER_LINK_BIND");
    unsetenv("NAM_PLAYER_LINK_PORT");
    return failures;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: lifecycle <lib> [quick cycles]\n"); return 2; }
    const int quick = argc > 2 ? std::atoi(argv[2]) : 200;
    int failures = 0;

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        void* lib = nullptr;
        AEffect* fx = create(argv[1], lib);
        if (!fx) return 2;
        fx->dispatcher(fx, effSetSampleRate, 0, 0, nullptr, 44100.0f);
        fx->dispatcher(fx, effSetBlockSize, 0, 128, nullptr, 0.0f);
        fx->dispatcher(fx, effOpen, 0, 0, nullptr, 0.0f);
        fx->setParameter(fx, 5, 0.0f);    // Normalize off
        fx->setParameter(fx, 6, 0.0f);    // first model of the first folder
        fx->setParameter(fx, 7, 0.0f);
        fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0.0f);

        long frame = 0;
        char text[64] = {0};
        const double first = runRealTime(fx, 1.0, frame);
        fx->dispatcher(fx, effGetParamDisplay, 6, 0, text, 0.0f);
        std::printf("cycle %d: after 1.0 s   model \"%s\"  output differs from input by %.3f\n", cycle, text, first);
        fx->setParameter(fx, 6, 0.99f);   // last model of the folder, while audio runs
        const double second = runRealTime(fx, 1.0, frame);
        fx->dispatcher(fx, effGetParamDisplay, 6, 0, text, 0.0f);
        std::printf("cycle %d: after switch  model \"%s\"  output differs from input by %.3f\n", cycle, text, second);
        if (!(first > 0.02) || !(second > 0.02) || std::fabs(first - second) < 1e-4)
        {
            std::printf("FAIL  cycle %d: the background loader did not deliver both models\n", cycle);
            ++failures;
        }
        destroy(fx, lib);
    }
    if (failures == 0) std::printf("PASS  real time: the worker thread loads and swaps models while audio runs\n");

    if (argc > 3) failures += linkCheck(argv[1], argv[3]);

    for (int i = 0; i < quick; ++i)
    {
        void* lib = nullptr;
        AEffect* fx = create(argv[1], lib);
        if (!fx) { std::printf("FAIL  load cycle %d: the plugin could not be created\n", i); return 1; }
        fx->dispatcher(fx, effOpen, 0, 0, nullptr, 0.0f);
        fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0.0f);
        destroy(fx, lib);
    }
    std::printf("PASS  %d load / unload cycles\n", quick);
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;   // a crash after this line is an exit handler left behind by the plugin
}
