// LinkServer.h - the "live link": a very small web server inside the plugin.
//
// An MPC in Developer Mode puts a network link on its USB cable. While NAM
// Player is loaded, a computer or phone at the other end of that cable can
// open  http://<the MPC's address>:9626/  and
//
//   * see the profiles on the card and which one is playing,
//   * send .nam files, which are saved to "NAM Profiles" on the card and can
//     start playing at once (no SD Card Access mode, no eject, audio keeps running),
//   * audition or remove profiles,
//   * work the plugin's controls and watch its levels and CPU load: the page is
//     the plugin's screen on devices that give it none.
//
// The page itself runs in the visitor's browser, which is the one with the
// internet connection: it downloads from the open platforms and hands the files
// to the plugin. The plugin never connects out to anything.
//
// What it will and will not do:
//   * It listens only on USB network interfaces (names starting usb / rndis /
//     ncm / ecm), never on Wi-Fi or Ethernet, so on an MPC that has those the
//     link is not reachable from the wider network. NAM_PLAYER_LINK_BIND=<address>
//     overrides this for testing.
//   * It writes only .nam files (and ABOUT.txt notes) inside "NAM Profiles".
//   * Requests that change anything must carry the header "X-NAM-Player: 1",
//     which a web page from another site cannot send.
//   * One connection at a time, 16 MB per file, 3 s timeouts.
//
// TONE3000: the page signs in on tone3000.com in the visitor's browser (OAuth with
// PKCE, "select tone" flow). TONE3000 sends the result to /t3k/callback here, where
// the page that started the sign-in collects it. The plugin stores only the
// publishable key (a file in "NAM Profiles"); tokens stay in the browser.
//
// Runs on its own low-priority thread; nothing here touches the audio thread.

#ifndef NAMPLAYER_LINK_SERVER_H
#define NAMPLAYER_LINK_SERVER_H

#include <atomic>
#include <string>
#include <vector>

#include <pthread.h>

namespace namplayer {

constexpr int kLinkPort = 9626;

// A string as a JSON string literal, quotes included.
std::string jsonString(const std::string& s);

// What the server needs from the plugin. Called on the link thread.
class LinkHost
{
public:
    virtual ~LinkHost() = default;
    // The state shown on the page, as JSON.
    virtual std::string linkStatusJson() = 0;
    // "<card>/NAM Profiles", or "" when the plugin is not installed on a card.
    virtual std::string linkProfilesRoot() = 0;
    // Files under the root changed: rescan. If `file` is not empty, select
    // that profile (folder is relative to the root, "" for the root itself).
    virtual void linkProfilesChanged(const std::string& folder, const std::string& file) = 0;
    // Select a profile by library folder key ("user:Marshall", "factory:") and file name.
    virtual bool linkSelect(const std::string& folderKey, const std::string& file) = 0;
    // Set a sound control ("drive", "mix", "tone", "level", "quality", "normalize") to a plain value.
    virtual bool linkSetParam(const std::string& id, double value) = 0;
    // Diagnosis: record a few seconds of what the plugin receives and what it puts out.
    virtual bool linkCaptureStart(double seconds) = 0;
    // 1 = finished (wav holds a 4-channel float WAV), 0 = still recording, -1 = nothing recorded.
    virtual int linkCaptureGet(std::string& wav) = 0;
    // A file shipped with the plugin (Content/...), for the page's catalogue.
    virtual std::string linkContentPath(const std::string& relative) = 0;
};

class LinkServer
{
public:
    explicit LinkServer(LinkHost& host) : host_(host) {}
    ~LinkServer() { stop(); }

    void start();
    void stop();

    // A file or folder name that is safe on FAT / exFAT and short enough for
    // the MPC's screen. Returns "" if nothing usable is left.
    static std::string safeName(const std::string& text, size_t limit);

private:
    struct Listener
    {
        int         fd = -1;
        std::string address;
    };
    struct Request;

    static void* entry(void* self);
    void run();
    void refreshListeners();
    void closeListeners();
    void serve(int client);
    void handle(int client, Request& request);
    void putProfile(int client, Request& request);
    void deleteProfile(int client, Request& request);

    // TONE3000 sign-in: the result TONE3000 sends back to /t3k/callback, kept until the page collects it.
    std::string           parkedState_, parkedJson_;

    LinkHost&             host_;
    std::atomic<bool>     stop_{false};
    bool                  running_ = false;
    pthread_t             thread_{};
    std::vector<Listener> listeners_;
};

} // namespace namplayer

#endif // NAMPLAYER_LINK_SERVER_H
