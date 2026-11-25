// host.cpp - a minimal scripted VST2 host for NAM Player's regression tests.
//
//   host <plugin library> <in.f32> <out.f32> <command>...
//
// in/out are raw float32, stereo interleaved, at 44.1 kHz. Commands run in order:
//   p<index>=<0..1>   set a parameter (normalised)
//   block=<n>         frames per process call from now on (0 = random 1..512)
//   run=<frames>      process that many frames of input (in place, like the MPC)
//   live=<frames>     the same, paced at real time (the plugin's own threads get to work)
//   save / load       effGetChunk into a buffer / effSetChunk it back
//   text=<index>      print the parameter's value text
//   rate=<hz>         sample rate (before the first run)
// The plugin is prepared (sample rate, block size, resume) before the first run.

#include "mpc_vst2_abi.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <string>
#include <vector>

static VstIntPtr hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float)
{
    return opcode == audioMasterVersion ? 2400 : 0;
}

int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: host <lib> <in.f32> <out.f32> commands...\n"); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    typedef AEffect* (*Entry)(audioMasterCallback);
    Entry entry = (Entry)dlsym(lib, "VSTPluginMain");
    AEffect* fx = entry ? entry(hostCallback) : nullptr;
    if (!fx) { std::fprintf(stderr, "no plugin\n"); return 2; }

    std::vector<float> in;
    if (FILE* f = std::fopen(argv[2], "rb"))
    {
        float buf[4096]; size_t n;
        while ((n = std::fread(buf, sizeof(float), 4096, f)) > 0) in.insert(in.end(), buf, buf + n);
        std::fclose(f);
    }
    const size_t totalFrames = in.size() / 2;
    std::vector<float> out;
    out.reserve(in.size());

    float rate = 44100.0f;
    int block = 128;
    bool prepared = false;
    size_t pos = 0;
    unsigned rng = 12345;
    std::vector<unsigned char> chunk;
    std::vector<float> l(4096), r(4096);

    for (int a = 4; a < argc; ++a)
    {
        const std::string cmd = argv[a];
        if (cmd[0] == 'p' && cmd.find('=') != std::string::npos)
        {
            const int index = std::atoi(cmd.c_str() + 1);
            fx->setParameter(fx, index, (float)std::atof(cmd.c_str() + cmd.find('=') + 1));
        }
        else if (cmd.rfind("block=", 0) == 0) block = std::atoi(cmd.c_str() + 6);
        else if (cmd.rfind("rate=", 0) == 0) rate = (float)std::atof(cmd.c_str() + 5);
        else if (cmd == "save")
        {
            void* data = nullptr;
            const VstIntPtr n = fx->dispatcher(fx, effGetChunk, 0, 0, &data, 0.0f);
            chunk.assign((unsigned char*)data, (unsigned char*)data + n);
        }
        else if (cmd == "load") fx->dispatcher(fx, effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data(), 0.0f);
        else if (cmd.rfind("save=", 0) == 0)      // save the state to a file
        {
            void* data = nullptr;
            const VstIntPtr n = fx->dispatcher(fx, effGetChunk, 0, 0, &data, 0.0f);
            if (FILE* f = std::fopen(cmd.c_str() + 5, "wb")) { std::fwrite(data, 1, (size_t)n, f); std::fclose(f); }
        }
        else if (cmd.rfind("load=", 0) == 0)      // restore a state saved by another run
        {
            std::vector<unsigned char> bytes;
            if (FILE* f = std::fopen(cmd.c_str() + 5, "rb"))
            {
                unsigned char buf[4096]; size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
                std::fclose(f);
            }
            fx->dispatcher(fx, effSetChunk, 0, (VstIntPtr)bytes.size(), bytes.data(), 0.0f);
        }
        else if (cmd.rfind("text=", 0) == 0)
        {
            char text[64] = {0};
            fx->dispatcher(fx, effGetParamDisplay, std::atoi(cmd.c_str() + 5), 0, text, 0.0f);
            std::printf("text %s: %s\n", cmd.c_str() + 5, text);
        }
        else if (cmd.rfind("run=", 0) == 0 || cmd.rfind("live=", 0) == 0)
        {
            // live=<frames>: like run, but paced at real time, so the plugin's own threads
            // (background loading, the live link) get to work as they do on an MPC.
            const bool live = cmd[0] == 'l';
            auto next = std::chrono::steady_clock::now();
            if (!prepared)
            {
                fx->dispatcher(fx, effSetSampleRate, 0, 0, nullptr, rate);
                fx->dispatcher(fx, effSetBlockSize, 0, 512, nullptr, 0.0f);
                fx->dispatcher(fx, effOpen, 0, 0, nullptr, 0.0f);
                fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0.0f);
                prepared = true;
            }
            size_t frames = (size_t)std::atoll(cmd.c_str() + (live ? 5 : 4));
            while (frames > 0)
            {
                int n = block;
                if (n <= 0) { rng = rng * 1664525u + 1013904223u; n = 1 + (int)((rng >> 8) % 512); }
                if ((size_t)n > frames) n = (int)frames;
                for (int i = 0; i < n; ++i)
                {
                    const bool have = pos + (size_t)i < totalFrames;
                    l[(size_t)i] = have ? in[2 * (pos + (size_t)i)] : 0.0f;
                    r[(size_t)i] = have ? in[2 * (pos + (size_t)i) + 1] : 0.0f;
                }
                float* bufs[2] = {l.data(), r.data()};
                fx->processReplacing(fx, bufs, bufs, n);   // in place
                for (int i = 0; i < n; ++i) { out.push_back(l[(size_t)i]); out.push_back(r[(size_t)i]); }
                pos += (size_t)n;
                frames -= (size_t)n;
                if (live)
                {
                    next += std::chrono::microseconds((long long)(1e6 * n / rate));
                    std::this_thread::sleep_until(next);
                }
            }
        }
        else { std::fprintf(stderr, "unknown command %s\n", cmd.c_str()); return 2; }
    }

    if (FILE* f = std::fopen(argv[3], "wb")) { std::fwrite(out.data(), sizeof(float), out.size(), f); std::fclose(f); }
    fx->dispatcher(fx, effMainsChanged, 0, 0, nullptr, 0.0f);
    fx->dispatcher(fx, effClose, 0, 0, nullptr, 0.0f);
    return 0;
}
