// core_render.cpp - the reference: renders mono float32 audio through NeuralAmpModelerCore,
// at the model's own sample rate, with nothing else in the path.
//
//   core_render <model.nam> <slim 0..1> <in.f32> <out.f32>

#include <cstdio>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"

int main(int argc, char** argv)
{
    if (argc < 5) return 2;
    nam::activations::Activation::enable_fast_tanh();
    auto dsp = nam::get_dsp(std::filesystem::path(argv[1]));
    if (auto* slim = dynamic_cast<nam::SlimmableModel*>(dsp.get())) slim->SetSlimmableSize(std::atof(argv[2]));
    const int block = 64;
    dsp->Reset(dsp->GetExpectedSampleRate() > 0 ? dsp->GetExpectedSampleRate() : 48000.0, block);

    std::vector<float> in;
    if (FILE* f = std::fopen(argv[3], "rb"))
    {
        float buf[4096]; size_t n;
        while ((n = std::fread(buf, sizeof(float), 4096, f)) > 0) in.insert(in.end(), buf, buf + n);
        std::fclose(f);
    }
    std::vector<float> out(in.size());
    for (size_t i = 0; i < in.size(); i += block)
    {
        const int n = (int)(in.size() - i < (size_t)block ? in.size() - i : block);
        float* ip = in.data() + i; float* op = out.data() + i;
        dsp->process(&ip, &op, n);
    }
    if (FILE* f = std::fopen(argv[4], "wb")) { std::fwrite(out.data(), sizeof(float), out.size(), f); std::fclose(f); }
    std::printf("loudness %s %f\n", dsp->HasLoudness() ? "yes" : "no", dsp->HasLoudness() ? dsp->GetLoudness() : 0.0);
    return 0;
}
