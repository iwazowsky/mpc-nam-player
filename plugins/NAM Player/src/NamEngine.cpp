// NamEngine.cpp - see NamEngine.h.

#include "NamEngine.h"

#include <cmath>
#include <fstream>
#include <mutex>
#include <vector>

#include "json.hpp"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"

extern "C" {
#include "nam_a2full.h"
#include "nam_a2lite.h"
}

namespace namplayer {
namespace {

using nlohmann::json;

// ---------------------------------------------------------------------------
// A2 fast path
// ---------------------------------------------------------------------------

// The layer-array configuration every standard A2 model has, without the three
// values that differ between files (channels, bottleneck, head_scale). A model
// takes the fast path only if its configuration equals this exactly; anything
// else (a future trainer option, a custom shape) goes to NAM Core, which is
// slower but always correct.
const char* const kA2CanonicalConfig = R"JSON(
{"layers": [{"input_size": 1, "condition_size": 1,
  "head": {"out_channels": 1, "kernel_size": 16, "bias": true},
  "kernel_sizes": [6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6],
  "dilations": [1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239],
  "head1x1": {"active": false, "out_channels": 1, "groups": 1},
  "layer1x1": {"active": true, "groups": 1},
  "groups_input": 1, "groups_input_mixin": 1,
  "conv_pre_film": {"active": false, "shift": true, "groups": 1},
  "conv_post_film": {"active": false, "shift": true, "groups": 1},
  "input_mixin_pre_film": {"active": false, "shift": true, "groups": 1},
  "input_mixin_post_film": {"active": false, "shift": true, "groups": 1},
  "activation_pre_film": {"active": false, "shift": true, "groups": 1},
  "activation_post_film": {"active": false, "shift": true, "groups": 1},
  "layer1x1_post_film": {"active": false, "shift": true, "groups": 1},
  "head1x1_post_film": {"active": false, "shift": true, "groups": 1},
  "slimmable": null}],
 "head": null}
)JSON";

constexpr int kA2Layers = 23;

enum class A2Shape { None, Lite, Full };

// Is `model` (a full NAM model object: architecture, config, weights) a standard A2 WaveNet?
A2Shape detectA2(const json& model)
{
    try
    {
        if (model.at("architecture") != "WaveNet") return A2Shape::None;
        json config = model.at("config");                    // a copy we can take apart (no weights in here)
        json& layer = config.at("layers").at(0);
        if (config.at("layers").size() != 1) return A2Shape::None;

        const int channels   = layer.at("channels").get<int>();
        const int bottleneck = layer.at("bottleneck").get<int>();
        if (channels != bottleneck || (channels != 3 && channels != 8)) return A2Shape::None;

        // Per-layer lists: 23 x LeakyReLU(0.01), no gating, no secondary activation.
        const json& act = layer.at("activation");
        const json& gate = layer.at("gating_mode");
        const json& sec = layer.at("secondary_activation");
        if (act.size() != kA2Layers || gate.size() != kA2Layers || sec.size() != kA2Layers) return A2Shape::None;
        for (int i = 0; i < kA2Layers; ++i)
        {
            if (act[(size_t)i].at("type") != "LeakyReLU") return A2Shape::None;
            if (std::fabs(act[(size_t)i].at("negative_slope").get<double>() - 0.01) > 1e-9) return A2Shape::None;
            if (act[(size_t)i].size() != 2) return A2Shape::None;
            if (gate[(size_t)i] != "none" || !sec[(size_t)i].is_null()) return A2Shape::None;
        }

        layer.erase("channels");
        layer.erase("bottleneck");
        layer.erase("activation");
        layer.erase("gating_mode");
        layer.erase("secondary_activation");
        config.erase("head_scale");
        static const json canonical = json::parse(kA2CanonicalConfig);
        if (config != canonical) return A2Shape::None;

        const size_t numWeights = model.at("weights").size();
        if (channels == 3 && numWeights == 1871) return A2Shape::Lite;
        if (channels == 8 && numWeights == 12146) return A2Shape::Full;
    }
    catch (const std::exception&)
    {
    }
    return A2Shape::None;
}

struct LiteApi
{
    using Weights = nam_a2lite_weights_t;
    using Inst    = nam_a2lite_t;
    using State   = nam_a2lite_state_t;
    static int  load(Weights* w, const float* p, int n) { return nam_a2lite_load_weights(w, p, n); }
    static void init(Inst* i, const Weights* w, State* s) { nam_a2lite_init(i, w, s); }
    static void prewarm(Inst* i) { nam_a2lite_prewarm(i); }
    static void process(Inst* i, const float* const* in, float* const* out, int n) { nam_a2lite_process(i, in, out, n); }
    static const char* name() { return "A2-Lite (fast engine)"; }
};

struct FullApi
{
    using Weights = nam_a2full_weights_t;
    using Inst    = nam_a2full_t;
    using State   = nam_a2full_state_t;
    static int  load(Weights* w, const float* p, int n) { return nam_a2full_load_weights(w, p, n); }
    static void init(Inst* i, const Weights* w, State* s) { nam_a2full_init(i, w, s); }
    static void prewarm(Inst* i) { nam_a2full_prewarm(i); }
    static void process(Inst* i, const float* const* in, float* const* out, int n) { nam_a2full_process(i, in, out, n); }
    static const char* name() { return "A2-Full (fast engine)"; }
};

// The engines keep no memory of their own; these three blocks are all of it
// (about 0.3 MB for A2-Full). The structs are over-aligned, which C++17's
// aligned operator new honours.
template <class Api>
class FastA2Engine final : public Engine
{
public:
    bool load(const std::vector<float>& weights)
    {
        weights_ = std::make_unique<typename Api::Weights>();
        inst_    = std::make_unique<typename Api::Inst>();
        state_   = std::make_unique<typename Api::State>();
        if (Api::load(weights_.get(), weights.data(), (int)weights.size()) != 0) return false;
        Api::init(inst_.get(), weights_.get(), state_.get());
        Api::prewarm(inst_.get());
        kind = Api::name();
        return true;
    }

    void process(const float* in, float* out, int numFrames) override
    {
        const float* const ins[1]  = {in};
        float* const       outs[1] = {out};
        Api::process(inst_.get(), ins, outs, numFrames);
    }

private:
    std::unique_ptr<typename Api::Weights> weights_;
    std::unique_ptr<typename Api::Inst>    inst_;
    std::unique_ptr<typename Api::State>   state_;
};

// ---------------------------------------------------------------------------
// NAM Core
// ---------------------------------------------------------------------------

class CoreEngine final : public Engine
{
public:
    explicit CoreEngine(std::unique_ptr<nam::DSP> dsp) : dsp_(std::move(dsp)) { kind = "NAM Core"; }

    void process(const float* in, float* out, int numFrames) override
    {
        NAM_SAMPLE* ins[1]  = {const_cast<NAM_SAMPLE*>(in)};
        NAM_SAMPLE* outs[1] = {out};
        dsp_->process(ins, outs, numFrames);
    }

private:
    std::unique_ptr<nam::DSP> dsp_;
};

// ---------------------------------------------------------------------------

double numberOr(const json& object, const char* key, double fallback)
{
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    return (it != object.end() && it->is_number()) ? it->get<double>() : fallback;
}

// A slimmable container holds several complete models, lightest first. Pick
// the one NAM Core would pick for a size of 0.0 (wantLite) or 1.0.
const json* pickSubmodel(const json& root, bool wantLite, bool& slimmable)
{
    slimmable = false;
    if (root.at("architecture") != "SlimmableContainer") return &root;
    const json& subs = root.at("config").at("submodels");
    if (!subs.is_array() || subs.empty()) throw std::runtime_error("container has no submodels");
    slimmable = subs.size() > 1;
    return &(wantLite ? subs.front() : subs.back()).at("model");
}

} // namespace

LoadResult loadEngine(const std::string& path, bool wantLite)
{
    LoadResult result;
    try
    {
        // NAM Core's default tanh is std::tanh; the fast approximation is what
        // every NAM host uses and is far cheaper.
        static std::once_flag once;
        std::call_once(once, [] { nam::activations::Activation::enable_fast_tanh(); });

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("cannot open file");
        json root;
        file >> root;
        if (!root.is_object()) throw std::runtime_error("not a NAM file");

        bool        slimmable = false;
        const json* model     = pickSubmodel(root, wantLite, slimmable);

        // Sample rate and loudness can sit on the submodel or on the file.
        double rate = numberOr(*model, "sample_rate", numberOr(root, "sample_rate", -1.0));
        if (!(rate >= 8000.0 && rate <= 384000.0)) rate = 48000.0;   // old files do not say: NAM's standard rate
        const json emptyObject = json::object();
        const json& meta     = model->contains("metadata") ? (*model)["metadata"] : emptyObject;
        const json& rootMeta = root.contains("metadata") ? root["metadata"] : emptyObject;
        const double loudness = numberOr(meta, "loudness", numberOr(rootMeta, "loudness", NAN));

        const A2Shape shape = detectA2(*model);
        if (shape != A2Shape::None)
        {
            const std::vector<float> weights = model->at("weights").get<std::vector<float>>();
            if (shape == A2Shape::Lite)
            {
                auto engine = std::make_unique<FastA2Engine<LiteApi>>();
                if (engine->load(weights)) result.engine = std::move(engine);
            }
            else
            {
                auto engine = std::make_unique<FastA2Engine<FullApi>>();
                if (engine->load(weights)) result.engine = std::move(engine);
            }
        }

        if (!result.engine)
        {
            std::unique_ptr<nam::DSP> dsp = nam::get_dsp(*model);
            if (!dsp) throw std::runtime_error("unsupported model");
            if (dsp->NumInputChannels() != 1 || dsp->NumOutputChannels() != 1)
                throw std::runtime_error("only mono models are supported");
            if (auto* slim = dynamic_cast<nam::SlimmableModel*>(dsp.get()))
            {
                slimmable = true;
                slim->SetSlimmableSize(wantLite ? 0.0 : 1.0);
            }
            if (dsp->GetExpectedSampleRate() > 0.0) rate = dsp->GetExpectedSampleRate();
            dsp->Reset(rate, kEngineMaxBlock);   // sizes the buffers and prewarms
            result.engine = std::make_unique<CoreEngine>(std::move(dsp));
        }

        result.engine->sampleRate  = rate;
        result.engine->hasLoudness = std::isfinite(loudness);
        result.engine->loudnessDb  = std::isfinite(loudness) ? loudness : -18.0;
        result.engine->slimmable   = slimmable;
        result.engine->isLite      = slimmable && wantLite;
    }
    catch (const std::exception& e)
    {
        result.engine.reset();
        result.error = e.what();
    }
    catch (...)
    {
        result.engine.reset();
        result.error = "unknown error";
    }
    return result;
}

} // namespace namplayer
