// NamEngine.h - loads a .nam file and runs it.
//
// Two inference back ends sit behind one interface:
//
//   * A2 fast path: TONE3000's generated C/NEON engines for the two standard
//     "NAM A2" shapes (A2-Lite, 3 channels, and A2-Full, 8 channels). Roughly
//     3x faster than NAM Core on ARM, which is what makes A2-Full practical on
//     an MPC Sample.
//   * NAM Core: Steven Atkinson's reference library. Runs everything else:
//     A1 WaveNets (standard / lite / feather / nano), LSTMs, custom A2 shapes.
//
// This header deliberately includes nothing from either library, so the plugin
// file compiles quickly and stays readable.

#ifndef NAMPLAYER_NAM_ENGINE_H
#define NAMPLAYER_NAM_ENGINE_H

#include <memory>
#include <string>

#ifndef NAM_PLAYER_ENGINE_MAX_BLOCK
#define NAM_PLAYER_ENGINE_MAX_BLOCK 96   // build.py passes the same value to the engines
#endif

namespace namplayer {

constexpr int kEngineMaxBlock = NAM_PLAYER_ENGINE_MAX_BLOCK;

class Engine
{
public:
    virtual ~Engine() = default;

    // Mono in, mono out, numFrames <= kEngineMaxBlock. `in` and `out` must not
    // overlap. Realtime safe.
    virtual void process(const float* in, float* out, int numFrames) = 0;

    double      sampleRate  = 48000.0;  // the rate the model was trained at
    bool        hasLoudness = false;    // the file says how loud the model is
    double      loudnessDb  = -18.0;    // ... in dB, for a standard input
    bool        slimmable   = false;    // the file also holds a lighter version
    bool        isLite      = false;    // the lighter version is the one loaded
    const char* kind        = "";       // for logs and tools: which back end runs it
};

struct LoadResult
{
    std::unique_ptr<Engine> engine;     // null when loading failed
    std::string             error;      // why
};

// Reads and builds a model, prewarmed and ready to process. Allocates and does
// file I/O: call from a worker thread, never from process().
// wantLite: for a slimmable model (A2), load its lightest version.
LoadResult loadEngine(const std::string& path, bool wantLite);

} // namespace namplayer

#endif // NAMPLAYER_NAM_ENGINE_H
