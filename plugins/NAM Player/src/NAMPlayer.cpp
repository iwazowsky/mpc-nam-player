// NAM Player - a Neural Amp Modeler profile player for the MPC, by Vazgen
//
// Browse the .nam profiles on the card with two knobs (Folder, Model) and play
// the track through the selected amp / pedal capture. Runs NAM "A2" profiles
// (the 2026 architecture) and the older A1 WaveNet and LSTM profiles.
//
// Signal flow (per block, in steps of up to 64 frames):
//
//   in L/R --+--------------------------------> delay (64 smp) --+ dry
//            |                                                   |
//            +-> mono sum -> Drive -> 44.1k->model rate -> MODEL  |
//                 -> model rate->44.1k -> DC block -> loudness    v
//                 normalise -> Tone (low-pass) ----------------> Mix -> Level -> out L/R
//
// The model is mono, so the wet signal is the same on both sides; the dry
// signal keeps its stereo image. The dry path is delayed by exactly the wet
// path's latency (64 samples, 1.5 ms: the two sample-rate converters), so Mix
// blends without comb filtering.
//
// Threads:
//   * audio thread: process(). Never allocates, locks, logs or touches files.
//   * worker thread (one per instance): watches the Folder / Model / Quality
//     knobs, loads the selected .nam file, and hands the finished model to the
//     audio thread through an atomic pointer. The audio thread fades the old
//     model out, swaps, fades the new one in, and hands the old one back to the
//     worker to be freed.
//   * UI thread: value text, state save / load.
//   * link thread (one per instance): the live link, a small web server on the
//     MPC's USB network link through which a computer or phone can send
//     profiles while the plugin runs. See LinkServer.h.
//
// CPU guard: a neural model that is too big for the CPU would stall the whole
// MPC. process() times itself; a model that keeps taking more than 90% of real
// time is switched to pass-through ("CPU!" on the Model knob). With Quality on
// Auto, an A2 profile is first replaced by its lighter A2-Lite version.

#include "mpc_plugin.h"

#include "dsp/biquad.h"
#include "dsp/smoother.h"

#include "LinkServer.h"
#include "ModelLibrary.h"
#include "NamEngine.h"
#include "Resampler.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#include <pthread.h>
#include <sched.h>

// Unloading. When the MPC removes the last instance it unloads this library.
// C++ registers the destructor of every static object (NAM Core's model
// registry, the SDK's own plugin-folder string, ...) with the C library, to be
// run when the library goes away. Calling __cxa_finalize at that moment is the
// job of a start-up file that GCC ships and the SDK's default zig toolchain
// does not, so without the function below those registrations would stay
// behind, pointing into unloaded memory, and crash the MPC when it shuts down.
// With a GCC toolchain this simply runs twice, which is harmless.
#if defined(__linux__)
extern "C" void  __cxa_finalize(void*);
extern "C" void* __dso_handle __attribute__((visibility("hidden")));
__attribute__((destructor)) static void namPlayerRunStaticDestructors() { __cxa_finalize(&__dso_handle); }
#endif

namespace {

using namespace namplayer;

// The order here is permanent: it is the parameter index the MPC, saved
// projects and the MPC Sample knob mapping in plugin.json all refer to.
enum ParamIndex
{
    kLevel = 0,   // dB, output level. Parameter 0 is nudged by the host on every load: harmless here.
    kDrive,       // dB, gain into the model: how hard the amp is pushed
    kMix,         // %, dry / wet
    kTone,        // Hz, low-pass on the wet signal (stands in for a speaker cabinet); maximum = off
    kQuality,     // Auto / Lite / Full: which version of an A2 profile to run
    kNormalize,   // on: bring every profile to the same loudness
    kModel,       // 0..1, position in the current folder's model list
    kFolder,      // 0..1, position in the folder list
    kNumParams
};

enum Quality { kQualityAuto = 0, kQualityLite, kQualityFull };

constexpr int    kChunk          = 64;                  // host frames per inner step
constexpr int    kLatency        = Resampler::kTaps;    // 64 samples through the wet path, whatever the model
constexpr double kMaxRateRatio   = 4.5;                 // model rate / host rate we will resample for
constexpr double kMinRateRatio   = 0.19;
constexpr int    kModelBufSize   = 320;                 // >= kChunk * kMaxRateRatio + 2
constexpr int    kDownBufSize    = 96;                  // >= kChunk + 2 / kMinRateRatio + 2
constexpr int    kFifoSize       = 256;                 // power of two
constexpr int    kFadeFrames     = 256;                 // 6 ms fade around a model swap
constexpr int    kHoldFrames     = kLatency + 32;       // silence between the fades: the new model's latency
constexpr float  kToneMaxHz      = 20000.0f;
constexpr float  kToneOffHz      = 19500.0f;            // at or above this the filter is bypassed
constexpr float  kNormalizeDb    = -18.0f;              // NAM's standard loudness target
constexpr float  kTripLoad       = 0.90f;               // share of real time one block may take ...
constexpr int    kTripBlocks     = 16;                  // ... this many blocks running, before pass-through
constexpr float  kAutoLiteLoad   = 0.50f;               // average load at which Auto drops to A2-Lite
constexpr int    kAutoLitePolls  = 10;                  // ... for this many worker polls (about 150 ms)

inline float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }

// Everything the audio thread needs to run one model. Built by the worker.
struct Slot
{
    std::unique_ptr<Engine>    engine;           // null: pass the audio through unchanged
    std::unique_ptr<Resampler> up, down;         // null: the model runs at the host rate
    int   prefill  = kLatency;                   // zeros that top the wet path's delay up to kLatency
    float normGain = 1.0f;                       // loudness normalisation for this model
    bool  lite     = false;                      // the light version of a slimmable model
};

} // namespace

class NAMPlayer : public mpc::Plugin, private LinkHost
{
public:
    NAMPlayer() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("level",     "Level",     -24.0f, 12.0f,    0.0f,       "dB", 1));
        addParam(mpc::floatParam ("drive",     "Drive",     -24.0f, 24.0f,    0.0f,       "dB", 1));
        addParam(mpc::floatParam ("mix",       "Mix",       0.0f,   100.0f,   100.0f,     "%",  0));
        addParam(mpc::logParam   ("tone",      "Tone",      500.0f, kToneMaxHz, kToneMaxHz, "Hz", 0));
        addParam(mpc::choiceParam("quality",   "Quality",   {"Auto", "Lite", "Full"}, kQualityAuto));
        addParam(mpc::boolParam  ("normalize", "Normalize", true));
        addParam(mpc::floatParam ("model",     "Model",     0.0f, 1.0f, 0.0f, "", 3));
        addParam(mpc::floatParam ("folder",    "Folder",    0.0f, 1.0f, 0.0f, "", 3));

        // Built-in presets set the sound controls only (the first six values);
        // they leave the Folder / Model selection where it is.
        //                         Level  Drive  Mix     Tone      Quality        Normalize
        addProgram("Default",     { 0.0f,  0.0f, 100.0f, 20000.0f, kQualityAuto,  1.0f});
        addProgram("Pushed",      {-3.0f,  9.0f, 100.0f, 8000.0f,  kQualityAuto,  1.0f});
        addProgram("Dark Crunch", {-2.0f, 12.0f, 100.0f, 4500.0f,  kQualityAuto,  1.0f});
        addProgram("Parallel",    { 0.0f,  6.0f,  50.0f, 6500.0f,  kQualityAuto,  1.0f});

        // build.py sets NAM_PLAYER_NO_SCAN while it dumps the parameter list, so the
        // generated UI never depends on which profiles happen to be on the build machine.
        fadeGain_.snap(1.0f);
        drive_.snap(1.0f);
        level_.snap(1.0f);
        mix_.snap(1.0f);
        norm_.snap(1.0f);

        if (std::getenv("NAM_PLAYER_NO_SCAN") == nullptr)
        {
            userRoot_    = userProfilesFolder();
            factoryRoot_ = mpc::contentPath("Models");
            auto library = std::make_shared<ModelLibrary>();
            library->scan(userRoot_, factoryRoot_);
            library_ = library;
        }

        // Test mode (NAM_PLAYER_SYNC=1): load profiles inside process() instead of on the
        // worker thread. An offline render finishes long before a background load would,
        // so tools such as host_sim would otherwise only ever hear the pass-through.
        // Never set this on an MPC: loading in process() is exactly what the worker avoids.
        syncLoad_ = std::getenv("NAM_PLAYER_SYNC") != nullptr;
        // Test mode (NAM_PLAYER_TEST_LOAD=0.7): pretend every full-size model uses that
        // share of real time, so the CPU guard can be exercised on a fast computer.
        if (const char* fake = std::getenv("NAM_PLAYER_TEST_LOAD")) fakeLoad_ = (float)std::atof(fake);
        if (!syncLoad_) startWorker();
        if (std::getenv("NAM_PLAYER_NO_SCAN") == nullptr) link_.start();
    }

    ~NAMPlayer() override
    {
        link_.stop();
        stop_.store(true, std::memory_order_release);
        if (workerRunning_) pthread_join(worker_, nullptr);
        if (cur_ != &nullSlot_) delete cur_;
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;   // audio is processed in fixed 64-frame steps, so no buffer depends on it
        hostRate_.store(sampleRate > 1000.0 ? sampleRate : 44100.0, std::memory_order_release);
        drive_.setTime(20.0f, sampleRate);
        level_.setTime(20.0f, sampleRate);
        mix_.setTime(20.0f, sampleRate);
        norm_.setTime(20.0f, sampleRate);
        dcCoeff_ = 1.0f - 2.0f * 3.14159265f * 10.0f / (float)sampleRate;   // 10 Hz DC blocker
    }

    void reset() override
    {
        resetStreams();
        std::fill(std::begin(dryDelayL_), std::end(dryDelayL_), 0.0f);
        std::fill(std::begin(dryDelayR_), std::end(dryDelayR_), 0.0f);
        dryPos_ = 0;
        tone_.reset();
        toneHz_ = get(kTone);
        dcX1_ = dcY1_ = 0.0f;
        drive_.snap(1.0f);
        level_.snap(dbToGain(get(kLevel)));
        mix_.snap(get(kMix) * 0.01f);
        norm_.snap(1.0f);
        fadeGain_.snap(1.0f);
        fade_ = Fade::Steady;
        overCount_ = 0;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        if (syncLoad_) workerStep(true);   // test mode only

        const auto started = std::chrono::steady_clock::now();
        const int  numFrames = ctx.numFrames;

        const float* inL  = (ctx.inputs != nullptr && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* inR  = (ctx.inputs != nullptr && ctx.numInputs > 1) ? ctx.inputs[1] : inL;
        float*       outL = ctx.numOutputs > 0 ? ctx.outputs[0] : nullptr;
        float*       outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        if (outL == nullptr) return;

        // Parameters, once per block.
        driveTarget_    = dbToGain(get(kDrive));
        normalizeOn_    = getBool(kNormalize);
        toneTargetHz_   = get(kTone);
        level_.setTarget(dbToGain(get(kLevel)));
        mix_.setTarget(get(kMix) * 0.01f);

        meterInSq_ = meterInPeak_ = meterOutPeak_ = 0.0f;
        for (int offset = 0; offset < numFrames; offset += kChunk)
        {
            const int n = numFrames - offset < kChunk ? numFrames - offset : kChunk;
            processChunk(inL ? inL + offset : nullptr, inR ? inR + offset : nullptr,
                         outL + offset, outR ? outR + offset : nullptr, n, (float)ctx.sampleRate);
        }

        // Meters for the live link's page: a 0.3 s average of the level going into the
        // model, and peaks that fall back slowly. Plain atomics, read by the link thread.
        const float blockSeconds = (float)numFrames / (float)ctx.sampleRate;
        const float a = mpc::clamp(blockSeconds / 0.3f, 0.0f, 1.0f), fall = std::exp(-blockSeconds / 0.5f);
        const float ms = inMeanSq_.load(std::memory_order_relaxed);
        inMeanSq_.store(ms + a * (meterInSq_ / (float)numFrames - ms), std::memory_order_relaxed);
        inPeak_.store(std::fmax(meterInPeak_, inPeak_.load(std::memory_order_relaxed) * fall), std::memory_order_relaxed);
        outPeak_.store(std::fmax(meterOutPeak_, outPeak_.load(std::memory_order_relaxed) * fall), std::memory_order_relaxed);
        blockFrames_.store(numFrames, std::memory_order_relaxed);

        // CPU guard: how much of this block's real-time budget did we use?
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        float        load    = (float)(elapsed * ctx.sampleRate / (double)numFrames);
        if (fakeLoad_ > 0.0f && cur_->engine != nullptr && !cur_->lite) load = fakeLoad_;   // test mode only
        const float  average = cpuLoad_.load(std::memory_order_relaxed);
        cpuLoad_.store(average + 0.02f * (load - average), std::memory_order_relaxed);
        cpuPeak_.store(std::fmax(load, cpuPeak_.load(std::memory_order_relaxed) * std::exp(-blockSeconds / 3.0f)), std::memory_order_relaxed);
        if (load > 1.0f) overruns_.fetch_add(1, std::memory_order_relaxed);   // this block took longer than it lasts
        if (cur_->engine != nullptr && !tripped_.load(std::memory_order_relaxed))
        {
            if (load > kTripLoad)
            {
                if (++overCount_ >= kTripBlocks) tripped_.store(true, std::memory_order_release);
            }
            else
                overCount_ = 0;
        }
    }

    // Value text. Folder and Model show names from the card instead of numbers.
    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index == kTone && plainValue >= kToneOffHz) { out = "Off"; return true; }
        if (index != kModel && index != kFolder) return false;

        const Choice choice = currentChoice();
        if (choice.folder < 0)
        {
            out = index == kFolder ? "No profiles" : "No models";
            return true;
        }
        if (index == kFolder) { out = choice.folderRef().name; return true; }

        const char* prefix = "";
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (choice.path() == errorPath_) prefix = "ERR ";
            else if (tripped_.load() && choice.path() == livePath_) prefix = "CPU! ";
        }
        out = prefix + std::to_string(choice.model + 1) + " " + choice.modelRef().name;
        return true;
    }

    // The project remembers the profile by folder and file name, not by knob
    // position, so adding profiles to the card later does not change what an
    // old project loads.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const Choice choice = currentChoice();
        if (choice.folder < 0) return;
        out.push_back(1);   // format version
        putString(out, choice.folderRef().key);
        putString(out, choice.modelRef().file);
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        size_t pos = 0;
        std::string key, file;
        if (size < 1 || data[pos++] != 1) return;
        if (!getString(data, size, pos, key) || !getString(data, size, pos, file)) return;
        // The parameters were restored just before this call: the profile is
        // remembered together with those knob positions (see currentChoice()).
        pinTo(key, file);   // does nothing if the profile is not on this card: the knobs decide
    }

private:
    // ----------------------------------------------------------------- audio

    void processChunk(const float* inL, const float* inR, float* outL, float* outR, int n, float sampleRate)
    {
        updateSwap();

        Slot&      slot   = *cur_;
        const bool active = slot.engine != nullptr && !tripped_.load(std::memory_order_relaxed);

        // 1. Read the input first: inputs and outputs are the same buffers on the MPC.
        drive_.setTarget(active ? driveTarget_ : 1.0f);
        for (int i = 0; i < n; ++i)
        {
            const float l = inL ? inL[i] : 0.0f;
            const float r = inR ? inR[i] : l;
            dryL_[i] = l;
            dryR_[i] = r;
            in_[i]   = 0.5f * (l + r) * drive_.process();
            meterInSq_ += in_[i] * in_[i];
            meterInPeak_ = std::fmax(meterInPeak_, std::fabs(in_[i]));
        }

        // 2. To the model's sample rate.
        const float* x = in_;
        int          m = n;
        if (slot.up) { m = slot.up->process(in_, n, model_); x = model_; }

        // 3. The model, in pieces the engines accept.
        const float* y = x;
        if (active)
        {
            for (int o = 0; o < m; o += kEngineMaxBlock)
                slot.engine->process(x + o, modelOut_ + o, m - o < kEngineMaxBlock ? m - o : kEngineMaxBlock);
            y = modelOut_;
        }

        // 4. Back to the host rate. The converters hand back n frames on average
        //    but n or n + 1 in any one step; the FIFO absorbs that.
        if (slot.down) fifoPush(down_, slot.down->process(y, m, down_));
        else           fifoPush(y, m);
        fifoPop(wet_, n);

        // 5. Tone filter coefficients, once per step.
        toneHz_ += 0.2f * (toneTargetHz_ - toneHz_);
        const bool toneOn = toneHz_ < kToneOffHz;
        tone_.setCoeffs(mpc::dsp::BiquadCoeffs::lowpass(sampleRate, mpc::clamp(toneHz_, 200.0f, 0.45f * sampleRate)));

        // 6. Wet chain, mix, level.
        norm_.setTarget(active && normalizeOn_ ? slot.normGain : 1.0f);
        for (int i = 0; i < n; ++i)
        {
            float wet = wet_[i];
            if (!(std::fabs(wet) < 64.0f)) wet = 0.0f;           // a model gone wrong (NaN, runaway) must not reach the output

            const float dc = wet - dcX1_ + dcCoeff_ * dcY1_;      // models can idle at a DC offset
            dcX1_ = wet;
            dcY1_ = dc;
            wet   = dc * norm_.process() * fadeGain_.next();

            const float filtered = tone_.process(wet);
            if (toneOn) wet = filtered;

            const float dl = dryDelayL_[dryPos_];
            const float dr = dryDelayR_[dryPos_];
            dryDelayL_[dryPos_] = dryL_[i];
            dryDelayR_[dryPos_] = dryR_[i];
            if (++dryPos_ >= kLatency) dryPos_ = 0;

            const float mix   = mix_.process();
            const float level = level_.process();
            outL[i] = (dl * (1.0f - mix) + wet * mix) * level;
            meterOutPeak_ = std::fmax(meterOutPeak_, std::fabs(outL[i]));
            if (outR) outR[i] = (dr * (1.0f - mix) + wet * mix) * level;
        }

        // Diagnosis (live link): while a capture is armed, keep what came in, what went
        // into the model, what came out of it, and what left the plugin.
        if (capture_.armed.load(std::memory_order_acquire))
        {
            const int pos = capture_.pos.load(std::memory_order_relaxed);
            const int k   = n < capture_.frames - pos ? n : capture_.frames - pos;
            float*    d   = capture_.data.data() + (size_t)pos * 4;
            for (int i = 0; i < k; ++i, d += 4) { d[0] = dryL_[i]; d[1] = in_[i]; d[2] = wet_[i]; d[3] = outL[i]; }
            capture_.pos.store(pos + k, std::memory_order_release);
            if (pos + k >= capture_.frames) capture_.armed.store(false, std::memory_order_release);
        }
    }

    // Model swap: fade the wet signal out, exchange the slot, wait, fade back in.
    // Called once per step; steps are kChunk frames except at the end of a block.
    void updateSwap()
    {
        if (fade_ == Fade::Steady)
        {
            // Wait until the worker has collected the previous model, so there
            // is always room to hand it the one we are about to replace.
            if (pending_.load(std::memory_order_acquire) != nullptr &&
                retired_.load(std::memory_order_acquire) == nullptr)
            {
                fade_ = Fade::Out;
                fadeGain_.setTarget(0.0f, kFadeFrames);
            }
        }
        else if (fade_ == Fade::Out && !fadeGain_.isRamping())
        {
            if (Slot* next = pending_.exchange(nullptr, std::memory_order_acq_rel))
            {
                Slot* old = cur_;
                cur_ = next;
                if (old != &nullSlot_) retired_.store(old, std::memory_order_release);
                resetStreams();
                overCount_ = 0;
                cpuLoad_.store(0.0f, std::memory_order_relaxed);
                tripped_.store(false, std::memory_order_release);
            }
            // The new model's first samples are still on their way through the wet
            // path; fading in before they arrive would let them start with a step.
            fade_ = Fade::Hold;
            holdFrames_ = kHoldFrames;
        }
        else if (fade_ == Fade::Hold)
        {
            holdFrames_ -= kChunk;
            if (holdFrames_ <= 0)
            {
                fade_ = Fade::In;
                fadeGain_.setTarget(1.0f, kFadeFrames);
            }
        }
        else if (fade_ == Fade::In && !fadeGain_.isRamping())
            fade_ = Fade::Steady;
    }

    // Clears the sample-rate converters and the wet FIFO of the current slot.
    void resetStreams()
    {
        Slot& slot = *cur_;
        if (slot.up) slot.up->reset();
        if (slot.down) slot.down->reset();
        fifoRead_  = 0;
        fifoCount_ = slot.prefill;
        std::fill(std::begin(fifo_), std::end(fifo_), 0.0f);
    }

    void fifoPush(const float* src, int count)
    {
        for (int i = 0; i < count && fifoCount_ < kFifoSize; ++i)
            fifo_[(fifoRead_ + fifoCount_++) & (kFifoSize - 1)] = src[i];
    }

    void fifoPop(float* dst, int count)
    {
        int i = 0;
        for (; i < count && fifoCount_ > 0; ++i, --fifoCount_)
        {
            dst[i] = fifo_[fifoRead_];
            fifoRead_ = (fifoRead_ + 1) & (kFifoSize - 1);
        }
        for (; i < count; ++i) dst[i] = 0.0f;   // cannot happen (see step 4), but never read stale memory
    }

    // ---------------------------------------------------------- selection

    // What is selected, read from one snapshot of the library so that the
    // indices, names and path always belong together.
    struct Choice
    {
        std::shared_ptr<const ModelLibrary> library;
        int folder = -1, model = -1;
        const Folder& folderRef() const { return library->folders()[(size_t)folder]; }
        const Model&  modelRef() const { return folderRef().models[(size_t)model]; }
        std::string   path() const { return folder < 0 ? std::string() : modelRef().path; }
    };

    // Which profile the knobs point at. Never called on the audio thread.
    //
    // A profile can also be "pinned": restored from a project, or chosen over
    // the live link. A pinned profile stays selected until a knob moves. Moving
    // Model browses inside the pinned folder; moving Folder lets go of both.
    Choice currentChoice()
    {
        const float folderNorm = getNormalised(kFolder);
        const float modelNorm  = getNormalised(kModel);
        Choice c;
        std::lock_guard<std::mutex> lock(stateMutex_);
        c.library = library_;
        const std::vector<Folder>& folders = c.library->folders();

        if (pin_.folderActive && folderNorm != pin_.folderNorm) pin_.folderActive = pin_.modelActive = false;
        if (pin_.modelActive && modelNorm != pin_.modelNorm) pin_.modelActive = false;
        if (pin_.folderActive)
        {
            c.folder = c.library->folderIndex(pin_.key);
            if (c.folder < 0) pin_.folderActive = pin_.modelActive = false;      // the folder has gone
            else if (pin_.modelActive)
            {
                c.model = c.library->modelIndex(c.folder, pin_.file);
                if (c.model >= 0) return c;
                pin_.modelActive = false;                                       // the file has gone
            }
        }
        if (c.folder < 0) c.folder = ModelLibrary::indexFor(folderNorm, (int)folders.size());
        if (c.folder < 0) return c;
        c.model = ModelLibrary::indexFor(modelNorm, (int)folders[(size_t)c.folder].models.size());
        return c;
    }

    // Pins a profile at the current knob positions. False if it is not in the library.
    bool pinTo(const std::string& key, const std::string& file)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        int folder = -1, model = -1;
        if (!library_->find(key, file, folder, model)) return false;
        pin_.folderActive = pin_.modelActive = true;
        pin_.folderNorm = getNormalised(kFolder);
        pin_.modelNorm  = getNormalised(kModel);
        pin_.key  = key;
        pin_.file = file;
        return true;
    }

    // ------------------------------------------------------------ live link

    std::string linkProfilesRoot() override { return userRoot_; }
    std::string linkContentPath(const std::string& relative) override { return mpc::contentPath(relative); }
    bool linkSelect(const std::string& key, const std::string& file) override { return pinTo(key, file); }

    // Recording for diagnosis. The buffer is allocated here, on the link thread, and the
    // audio thread only fills it while "armed" is set.
    bool linkCaptureStart(double seconds) override
    {
        capture_.armed.store(false, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));   // let a step in progress finish
        const double rate = hostRate_.load();
        const int frames = (int)(mpc::clamp(seconds > 0.0 ? seconds : 4.0, 0.5, 8.0) * rate);
        capture_.data.assign((size_t)frames * 4, 0.0f);
        capture_.frames = frames;
        capture_.rate   = rate;
        capture_.pos.store(0, std::memory_order_release);
        capture_.armed.store(true, std::memory_order_release);
        return true;
    }

    // Channels: 1 input (left), 2 model input (mono, after Drive), 3 model output (64 samples
    // later, before normalising and Tone), 4 plugin output (left).
    int linkCaptureGet(std::string& wav) override
    {
        if (capture_.frames == 0) return -1;
        if (capture_.armed.load(std::memory_order_acquire)) return 0;
        const uint32_t bytes = (uint32_t)capture_.data.size() * 4, rate = (uint32_t)capture_.rate;
        const auto u32 = [&wav](uint32_t v) { for (int i = 0; i < 4; ++i) wav += (char)(v >> (8 * i)); };
        const auto u16 = [&wav](uint32_t v) { wav += (char)v; wav += (char)(v >> 8); };
        wav.reserve(bytes + 44);
        wav += "RIFF"; u32(36 + bytes); wav += "WAVEfmt "; u32(16); u16(3); u16(4); u32(rate); u32(rate * 16); u16(16); u16(32);
        wav += "data"; u32(bytes);
        wav.append(reinterpret_cast<const char*>(capture_.data.data()), bytes);
        return 1;
    }

    // The page's controls. The value is stored exactly as a knob turn would store it;
    // the MPC reads it back the next time it shows or saves the parameter.
    bool linkSetParam(const std::string& id, double value) override
    {
        for (int i : {kLevel, kDrive, kMix, kTone, kQuality, kNormalize})
            if (param(i).id == id) { set(i, (float)value); return true; }
        return false;
    }

    // The card changed: read it again. The profile that is playing stays
    // selected even if the new list puts it at a different knob position.
    void linkProfilesChanged(const std::string& folder, const std::string& file) override
    {
        auto fresh = std::make_shared<ModelLibrary>();
        fresh->scan(userRoot_, factoryRoot_);

        std::string key, keepFile;
        const Choice before = currentChoice();
        if (before.folder >= 0) { key = before.folderRef().key; keepFile = before.modelRef().file; }
        if (!file.empty()) { key = "user:" + folder; keepFile = file; }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            library_ = fresh;
        }
        if (!keepFile.empty()) pinTo(key, keepFile);
    }

    std::string linkStatusJson() override
    {
        const Choice choice = currentChoice();
        std::string error;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            error = errorPath_;
        }
        std::string liveKind, livePath;
        bool        liveLite = false;
        double      liveRate = 0.0;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            liveKind = liveKind_; livePath = livePath_; liveLite = liveLite_; liveRate = liveRate_;
        }
        const auto num = [](double v, const char* fmt = "%.3f") { char b[40]; std::snprintf(b, sizeof(b), fmt, v); return std::string(b); };
        const auto db  = [&num](float linear) { return num(linear > 1e-6f ? 20.0 * std::log10((double)linear) : -120.0, "%.1f"); };
        std::string json = "{\"plugin\":\"NAM Player\",\"version\":\"1.0.0\",\"uploads\":";
        json += userRoot_.empty() ? "false" : "true";
        json += ",\"cpu\":" + num(cpuLoad_.load()) + ",\"cpuPeak\":" + num(cpuPeak_.load()) +
                ",\"overruns\":" + std::to_string(overruns_.load()) + ",\"bypassed\":" + (tripped_.load() ? "true" : "false");
        json += ",\"block\":" + std::to_string(blockFrames_.load()) + ",\"rate\":" + num(hostRate_.load(), "%.0f");
        // Levels: what goes into the model (after Drive) and what leaves the plugin, in dBFS.
        json += ",\"inRms\":" + db(std::sqrt(inMeanSq_.load())) + ",\"inPeak\":" + db(inPeak_.load()) + ",\"outPeak\":" + db(outPeak_.load());
        // The model that is actually running (it can differ from the selection while one loads).
        json += ",\"engine\":{\"kind\":" + jsonString(liveKind) + ",\"lite\":" + (liveLite ? "true" : "false") +
                ",\"rate\":" + num(liveRate, "%.0f") + ",\"current\":" + (livePath == choice.path() ? "true" : "false") + "}";
        json += ",\"params\":{";
        bool firstParam = true;
        for (int i : {kLevel, kDrive, kMix, kTone, kQuality, kNormalize})
        {
            json += std::string(firstParam ? "" : ",") + jsonString(param(i).id) + ":{\"value\":" + num(get(i), "%.2f") +
                    ",\"norm\":" + num(getNormalised(i), "%.4f") + ",\"min\":" + num(param(i).min, "%.2f") + ",\"max\":" + num(param(i).max, "%.2f") + ",\"text\":" + jsonString(displayText(i)) + "}";
            firstParam = false;
        }
        json += "}";
        json += ",\"current\":";
        if (choice.folder < 0) json += "null";
        else
            json += "{\"key\":" + jsonString(choice.folderRef().key) + ",\"folder\":" + jsonString(choice.folderRef().name) +
                    ",\"file\":" + jsonString(choice.modelRef().file) + ",\"name\":" + jsonString(choice.modelRef().name) +
                    ",\"error\":" + (choice.path() == error ? "true" : "false") + "}";
        json += ",\"folders\":[";
        const std::vector<Folder>& folders = choice.library->folders();
        for (size_t f = 0; f < folders.size(); ++f)
        {
            const Folder& folder = folders[f];
            // "dir" is the folder relative to "NAM Profiles", for removing files; factory profiles have none.
            const bool user = folder.key.compare(0, 5, "user:") == 0;
            json += std::string(f ? "," : "") + "{\"key\":" + jsonString(folder.key) + ",\"name\":" + jsonString(folder.name) +
                    ",\"dir\":" + (user ? jsonString(folder.key.substr(5)) : std::string("null")) + ",\"models\":[";
            for (size_t m = 0; m < folder.models.size(); ++m)
                json += std::string(m ? "," : "") + "{\"name\":" + jsonString(folder.models[m].name) +
                        ",\"file\":" + jsonString(folder.models[m].file) + ",\"kind\":" + jsonString(folder.models[m].kind) + "}";
            json += "]}";
        }
        return json + "]}";
    }

    // "<drive>/NAM Profiles", worked out from where this plugin is installed
    // ("<drive>/VST2 Plugins/<plugin folder>"). Empty when not installed that way.
    static std::string userProfilesFolder()
    {
        if (const char* env = std::getenv("NAM_PLAYER_PROFILES"))
            if (env[0] != '\0') return env;
        namespace fs = std::filesystem;
        const fs::path plugin(mpc::pluginFolder());
        if (plugin.empty() || plugin.parent_path().filename() != "VST2 Plugins") return std::string();
        return (plugin.parent_path().parent_path() / "NAM Profiles").string();
    }

    static void putString(std::vector<uint8_t>& out, const std::string& s)
    {
        const size_t n = s.size() > 65535 ? 65535 : s.size();
        out.push_back((uint8_t)(n & 0xFF));
        out.push_back((uint8_t)(n >> 8));
        out.insert(out.end(), s.begin(), s.begin() + (std::ptrdiff_t)n);
    }

    static bool getString(const uint8_t* data, size_t size, size_t& pos, std::string& s)
    {
        if (pos + 2 > size) return false;
        const size_t n = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
        pos += 2;
        if (pos + n > size) return false;
        s.assign(reinterpret_cast<const char*>(data + pos), n);
        pos += n;
        return true;
    }

    // -------------------------------------------------------------- worker

    struct LoadKey
    {
        std::string path;
        bool        lite = false;
        double      hostRate = 0.0;
        bool operator==(const LoadKey& o) const { return path == o.path && lite == o.lite && hostRate == o.hostRate; }
    };

    // A plain pthread rather than std::thread, for two reasons. The MPC locks all
    // of its memory, so a default 8 MB thread stack would cost 8 MB of RAM per
    // instance: 1 MB is plenty for loading a model. And std::thread makes the
    // statically linked C++ runtime create a thread-local key that would be left
    // behind each time the library is unloaded.
    void startWorker()
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1024 * 1024);
        // Never compete with the audio thread, whatever priority the creating thread has.
        sched_param param{};
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
        pthread_attr_setschedparam(&attr, &param);
        workerRunning_ = pthread_create(&worker_, &attr, &workerEntry, this) == 0;
        pthread_attr_destroy(&attr);
        // If no thread could be created the plugin still works as a pass-through.
    }

    static void* workerEntry(void* self)
    {
        static_cast<NAMPlayer*>(self)->workerMain();
        return nullptr;
    }

    void workerMain()
    {
        while (!stop_.load(std::memory_order_acquire))
        {
            workerStep(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    // One pass of the worker: free the model the audio thread has finished with,
    // work out which profile is wanted, and load it if that has changed.
    // immediate: skip the "knob has come to rest" wait (first load, test mode).
    void workerStep(bool immediate)
    {
        using clock = std::chrono::steady_clock;
        WorkerState& w = work_;

        delete retired_.exchange(nullptr, std::memory_order_acq_rel);

        LoadKey want;
        want.path = currentChoice().path();
        want.hostRate = hostRate_.load(std::memory_order_acquire);

        const int quality = getInt(kQuality);
        if (quality == kQualityAuto && w.loaded.path == want.path && w.loadedSlimmable && !w.loaded.lite)
        {
            w.overPolls = cpuLoad_.load(std::memory_order_relaxed) > kAutoLiteLoad ? w.overPolls + 1 : 0;
            if (w.overPolls >= kAutoLitePolls || tripped_.load(std::memory_order_acquire)) w.autoLitePath = want.path;
        }
        want.lite = quality == kQualityLite ||
                    (quality == kQualityAuto && !want.path.empty() && want.path == w.autoLitePath);

        if (want == w.loaded) return;
        if (!(want == w.candidate)) { w.candidate = want; w.candidateSince = clock::now(); }
        // Wait until the knob has rested for a moment, so sweeping through a
        // long list does not load every profile on the way.
        if (immediate || w.first || clock::now() - w.candidateSince >= std::chrono::milliseconds(120))
        {
            w.loadedSlimmable = load(want);
            w.loaded    = want;
            w.first     = false;
            w.overPolls = 0;
        }
    }

    // Builds a slot for `key` and offers it to the audio thread. Returns whether the model is slimmable.
    bool load(const LoadKey& key)
    {
        auto slot      = std::make_unique<Slot>();
        bool slimmable = false, failed = false, lite = false;
        std::string kind;
        double      rate = 0.0;

        if (!key.path.empty())
        {
            LoadResult result = loadEngine(key.path, key.lite);
            if (result.engine)
            {
                const double modelRate = result.engine->sampleRate;
                const double ratio     = modelRate / key.hostRate;
                if (std::llround(modelRate) != std::llround(key.hostRate) && ratio <= kMaxRateRatio &&
                    ratio >= kMinRateRatio && Resampler::supported(key.hostRate, modelRate) &&
                    Resampler::supported(modelRate, key.hostRate))
                {
                    // Up: half the filter length, in host samples. Down: the nearest whole
                    // number of host samples to its natural delay. The pair is then an exact
                    // number of host samples late, so the dry path can match it exactly.
                    const int upDelay   = Resampler::kTaps / 2;
                    const int downDelay = (int)std::lround(0.5 * Resampler::kTaps / ratio);   // host samples
                    slot->up   = std::make_unique<Resampler>(key.hostRate, modelRate, kChunk, (double)upDelay);
                    slot->down = std::make_unique<Resampler>(modelRate, key.hostRate, slot->up->maxOutput(kChunk),
                                                             (double)downDelay * ratio);
                    slot->prefill = mpc::clamp(kLatency - upDelay - downDelay, 0, kLatency);
                }
                // (A rate we cannot convert for runs at the host rate: detuned, but it plays.)
                if (result.engine->hasLoudness)
                    slot->normGain = mpc::clamp(dbToGain(kNormalizeDb - (float)result.engine->loudnessDb), 0.05f, 8.0f);
                slimmable    = result.engine->slimmable;
                slot->lite   = result.engine->isLite;
                lite = result.engine->isLite;
                kind = result.engine->kind;
                rate = modelRate;
                slot->engine = std::move(result.engine);
            }
            else
            {
                failed = true;
                mpc::log("NAM Player: cannot load %s: %s", key.path.c_str(), result.error.c_str());
            }
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            errorPath_ = failed ? key.path : std::string();
            livePath_  = key.path;
            liveKind_  = kind;
            liveLite_  = lite;
            liveRate_  = rate;
        }
        // If the audio thread has not taken the previous offer yet, replace it.
        delete pending_.exchange(slot.release(), std::memory_order_acq_rel);
        return slimmable;
    }

    // ----------------------------------------------------------------- data

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name           = "NAM Player";
        info.vendor         = "Vazgen";       // must match plugin.json "manufacturer"
        info.uniqueId       = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version        = 1000;           // 1.0.0.0
        info.isInstrument   = false;
        info.numInputs      = 2;
        info.numOutputs     = 2;
        info.latencySamples = kLatency;
        info.tailSamples    = 8192;           // the longest model memory (A2: 6347 samples at 48 kHz) plus latency
        return info;
    }

    static constexpr char kUid[5] = "V10S";

    enum class Fade { Steady, Out, Hold, In };

    struct WorkerState
    {
        LoadKey     loaded, candidate;
        bool        loadedSlimmable = false, first = true;
        std::string autoLitePath;     // the profile Auto has dropped to its Lite version
        int         overPolls = 0;
        std::chrono::steady_clock::time_point candidateSince = std::chrono::steady_clock::now();
    };

    struct Capture
    {
        std::vector<float> data;              // 4 channels, interleaved
        int                frames = 0;
        double             rate = 44100.0;
        std::atomic<int>   pos{0};
        std::atomic<bool>  armed{false};
    };

    struct Pin
    {
        bool        folderActive = false, modelActive = false;
        float       folderNorm = 0.0f, modelNorm = 0.0f;
        std::string key, file;                // the library folder key and the file name
    };

    std::string userRoot_, factoryRoot_;      // where profiles are scanned from; set in the constructor

    // Shared between threads.
    std::atomic<Slot*>  pending_{nullptr};    // worker -> audio: the next model
    std::atomic<Slot*>  retired_{nullptr};    // audio -> worker: the previous model, to be freed
    std::atomic<double> hostRate_{44100.0};
    std::atomic<float>  cpuLoad_{0.0f};       // slow average of process() time / real time
    std::atomic<bool>   tripped_{false};      // CPU guard has put the current model in pass-through
    // Meters for the live link's page (written by the audio thread).
    std::atomic<float>    cpuPeak_{0.0f}, inMeanSq_{0.0f}, inPeak_{0.0f}, outPeak_{0.0f};
    std::atomic<uint32_t> overruns_{0};       // blocks that took longer to process than they last
    std::atomic<int>      blockFrames_{0};
    std::atomic<bool>   stop_{false};
    // Guarded by stateMutex_, which the audio thread never takes.
    std::mutex          stateMutex_;
    std::shared_ptr<const ModelLibrary> library_ = std::make_shared<ModelLibrary>();   // replaced whole on a rescan
    Pin                 pin_;
    std::string         errorPath_;           // the profile that failed to load, if any
    std::string         livePath_;            // the profile last handed to the audio thread
    std::string         liveKind_;            // ... which engine runs it
    bool                liveLite_ = false;    // ... whether it is the light version
    double              liveRate_ = 0.0;      // ... and its sample rate
    Capture             capture_;
    LinkServer          link_{*this};
    pthread_t           worker_{};
    bool                workerRunning_ = false;
    WorkerState         work_;                // worker thread only (audio thread in test mode)
    bool                syncLoad_ = false;    // test mode, see the constructor
    float               fakeLoad_ = 0.0f;     // test mode, see the constructor

    // Audio thread only.
    Slot   nullSlot_;                         // pass-through, used until the first model arrives
    Slot*  cur_ = &nullSlot_;
    Fade   fade_ = Fade::Steady;
    int    overCount_ = 0, holdFrames_ = 0;
    float  meterInSq_ = 0.0f, meterInPeak_ = 0.0f, meterOutPeak_ = 0.0f;   // gathered over one block
    float  driveTarget_ = 1.0f, toneTargetHz_ = kToneMaxHz, toneHz_ = kToneMaxHz;
    bool   normalizeOn_ = true;
    float  dcCoeff_ = 0.9986f, dcX1_ = 0.0f, dcY1_ = 0.0f;
    int    fifoRead_ = 0, fifoCount_ = kLatency, dryPos_ = 0;

    mpc::dsp::Smoother   drive_, level_, mix_, norm_;
    mpc::dsp::LinearRamp fadeGain_;
    mpc::dsp::Biquad     tone_;

    float in_[kChunk] = {}, dryL_[kChunk] = {}, dryR_[kChunk] = {}, wet_[kChunk] = {};
    float model_[kModelBufSize] = {}, modelOut_[kModelBufSize] = {}, down_[kDownBufSize] = {};
    float fifo_[kFifoSize] = {};
    float dryDelayL_[kLatency] = {}, dryDelayR_[kLatency] = {};
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(NAMPlayer)
