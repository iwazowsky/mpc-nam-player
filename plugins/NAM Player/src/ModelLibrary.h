// ModelLibrary.h - finds .nam files and arranges them as folders of models.
//
// Two places are scanned when the plugin is created, and again whenever the
// live link (LinkServer.h) adds or removes a profile. A scan fills a fresh
// ModelLibrary; the plugin then swaps it in, so readers always see a complete one.
//
//   <drive>/NAM Profiles/            your own profiles. This folder sits at the top
//       Marshall/crunch.nam          level of the card, beside "VST2 Plugins", so
//       Marshall/lead.nam            replacing the plugin folder never touches it.
//       clean.nam                    Each sub-folder that holds .nam files becomes a
//                                    "folder" on the Folder knob (up to 3 levels deep);
//                                    files directly in "NAM Profiles" appear as "Profiles".
//
//   <plugin folder>/Content/Models/  profiles shipped inside the plugin ("Factory").
//
// Not realtime safe: scan() touches the file system.

#ifndef NAMPLAYER_MODEL_LIBRARY_H
#define NAMPLAYER_MODEL_LIBRARY_H

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace namplayer {

struct Model
{
    std::string name;   // shown on the knob: the file name without ".nam"
    std::string file;   // file name, used to find the model again in a saved project
    std::string path;   // full path
    std::string kind;   // "A2", "A1 standard", "LSTM", ...: read from the start of the file, "" if unknown
};

struct Folder
{
    std::string name;   // shown on the Folder knob
    std::string key;    // stable id for saved projects: "user:Marshall", "factory:"
    std::vector<Model> models;
};

class ModelLibrary
{
public:
    static constexpr int kMaxDepth   = 3;
    static constexpr int kMaxFolders = 256;
    static constexpr int kMaxModels  = 512;   // per folder

    void scan(const std::string& userRoot, const std::string& factoryRoot)
    {
        folders_.clear();
        if (!userRoot.empty()) scanDir(userRoot, "", "user", "Profiles", 0);
        if (!factoryRoot.empty()) scanDir(factoryRoot, "", "factory", "Factory", 0);
    }

    const std::vector<Folder>& folders() const { return folders_; }
    bool empty() const { return folders_.empty(); }

    // A knob position (0..1) to one of `count` equal slots. -1 when there is nothing to choose.
    static int indexFor(float norm, int count)
    {
        if (count <= 0) return -1;
        const int i = (int)(norm * (float)count);
        return i < 0 ? 0 : (i >= count ? count - 1 : i);
    }

    int folderIndex(const std::string& folderKey) const
    {
        for (size_t f = 0; f < folders_.size(); ++f)
            if (folders_[f].key == folderKey) return (int)f;
        return -1;
    }

    int modelIndex(int folder, const std::string& file) const
    {
        if (folder < 0 || folder >= (int)folders_.size()) return -1;
        const std::vector<Model>& models = folders_[(size_t)folder].models;
        for (size_t m = 0; m < models.size(); ++m)
            if (models[m].file == file) return (int)m;
        return -1;
    }

    bool find(const std::string& folderKey, const std::string& file, int& folder, int& model) const
    {
        folder = folderIndex(folderKey);
        model  = modelIndex(folder, file);
        return model >= 0;
    }

private:
    static std::string lower(std::string s)
    {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // What kind of model a file holds, from its first few kilobytes (a .nam file names its
    // architecture before the long list of weights). Cheap enough to do for every file.
    static std::string sniffKind(const std::string& path)
    {
        char buf[4097] = {0};
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) return std::string();
        const size_t n = std::fread(buf, 1, 4096, f);
        std::fclose(f);
        const std::string head(buf, n);
        const auto valueAfter = [&head](const char* key) {
            size_t i = head.find(key);
            if (i == std::string::npos) return std::string();
            i = head.find(':', i);
            if (i == std::string::npos) return std::string();
            size_t a = head.find_first_not_of(" \t\r\n\"", i + 1);
            if (a == std::string::npos) return std::string();
            const size_t b = head.find_first_of(",\"}\r\n ]", a);
            return head.substr(a, b == std::string::npos ? std::string::npos : b - a);
        };
        const std::string arch = valueAfter("\"architecture\"");
        if (arch == "SlimmableContainer") return "A2";
        if (arch == "LSTM") return "LSTM";
        if (arch == "WaveNet")
        {
            const std::string channels = valueAfter("\"channels\"");
            if (head.find("\"kernel_sizes\"") != std::string::npos)
                return channels == "3" ? "A2 Lite" : channels == "8" ? "A2 Full" : "A2 custom";
            return channels == "16" ? "A1 standard" : channels == "12" ? "A1 lite" : channels == "8" ? "A1 feather"
                 : channels == "4" ? "A1 nano" : "A1 custom";
        }
        return arch;
    }

    static bool isNamFile(const std::string& name)
    {
        return name.size() > 4 && lower(name.substr(name.size() - 4)) == ".nam";
    }

    void scanDir(const std::filesystem::path& dir, const std::string& relative, const char* tag,
                 const char* rootLabel, int depth)
    {
        namespace fs = std::filesystem;
        std::vector<fs::path> subdirs;
        Folder folder;

        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            // Hidden files, and the "._name" companions macOS writes on FAT/exFAT cards.
            if (name.empty() || name[0] == '.') continue;
            std::error_code typeError;
            if (it->is_directory(typeError))
                subdirs.push_back(it->path());
            else if (it->is_regular_file(typeError) && isNamFile(name) && (int)folder.models.size() < kMaxModels)
                folder.models.push_back({name.substr(0, name.size() - 4), name, it->path().string(), sniffKind(it->path().string())});
        }

        if (!folder.models.empty() && (int)folders_.size() < kMaxFolders)
        {
            std::sort(folder.models.begin(), folder.models.end(),
                      [](const Model& a, const Model& b) { return lower(a.name) < lower(b.name); });
            folder.name = relative.empty() ? std::string(rootLabel) : dir.filename().string();
            folder.key  = std::string(tag) + ":" + relative;
            folders_.push_back(std::move(folder));
        }

        if (depth >= kMaxDepth) return;
        std::sort(subdirs.begin(), subdirs.end(), [](const fs::path& a, const fs::path& b) {
            return lower(a.filename().string()) < lower(b.filename().string());
        });
        for (const fs::path& sub : subdirs)
        {
            const std::string name = sub.filename().string();
            scanDir(sub, relative.empty() ? name : relative + "/" + name, tag, rootLabel, depth + 1);
        }
    }

    std::vector<Folder> folders_;
};

} // namespace namplayer

#endif // NAMPLAYER_MODEL_LIBRARY_H
