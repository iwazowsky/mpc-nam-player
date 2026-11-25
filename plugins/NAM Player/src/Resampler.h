// Resampler.h - streaming rational sample-rate converter (polyphase windowed sinc).
//
// The MPC runs plugins at 44.1 kHz. NAM models are trained at 48 kHz, and a
// neural amp model played at the wrong rate is audibly detuned (its whole
// frequency response shifts by the rate ratio). NAM Player therefore converts
// 44.1 kHz -> model rate before the model and model rate -> 44.1 kHz after it.
//
// The ratio is the exact fraction outRate/inRate = L/M (160/147 for
// 44.1 -> 48 kHz), tracked with integer phase arithmetic, so the two converters
// can never drift against each other however long the plugin runs.
//
// Realtime: process() does no allocation. The constructor allocates and
// computes the filter table; build a Resampler on a worker thread.

#ifndef NAMPLAYER_RESAMPLER_H
#define NAMPLAYER_RESAMPLER_H

#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

#if defined(__ARM_NEON)
  #include <arm_neon.h>
#endif

namespace namplayer {

class Resampler
{
public:
    // Taps per phase (a multiple of 8). With the window below, 64 taps give a
    // passband flat to about 19 kHz and 100 dB of image / alias rejection for
    // the 44.1 <-> 48 kHz pair. The rejection matters more than usual here: a
    // high-gain amp model amplifies anything that leaks into its input by
    // 40 dB or more, so images at -70 dB would come out as audible grit.
    static constexpr int kTaps = 64;
    // Largest interpolation factor we build a table for (table = L * kTaps floats).
    static constexpr int kMaxPhases = 2048;

    // True when inRate -> outRate reduces to a fraction this class can handle.
    static bool supported(double inRate, double outRate)
    {
        int L = 0, M = 0;
        return reduce(inRate, outRate, L, M) && L <= kMaxPhases && M <= kMaxPhases * 8;
    }

    // maxInput: the largest numIn ever passed to process().
    // delayInputSamples: the delay the filter should have, in input samples. It
    // must be close to kTaps / 2 (the filter's natural delay); being able to
    // nudge it lets a pair of converters add up to a whole number of samples.
    Resampler(double inRate, double outRate, int maxInput, double delayInputSamples = 0.5 * kTaps)
    {
        reduce(inRate, outRate, L_, M_);
        maxInput_ = maxInput;
        delay_ = delayInputSamples;
        buf_.assign((size_t)(kTaps - 1 + maxInput), 0.0f);
        designTable(inRate, outRate);
    }

    // The most output frames process() can return for numIn input frames.
    int maxOutput(int numIn) const { return (int)(((long long)numIn * L_ + M_ - 1) / M_) + 1; }

    // Delay through the filter, in input samples.
    double delayInputSamples() const { return delay_; }

    void reset()
    {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        pos_ = 0;
        phase_ = 0;
    }

    // Consumes numIn frames (<= maxInput) and writes every output frame that is
    // now computable to `out` (capacity >= maxOutput(numIn)). Returns the count.
    int process(const float* in, int numIn, float* out)
    {
        float* buf = buf_.data();
        std::memcpy(buf + (kTaps - 1), in, sizeof(float) * (size_t)numIn);

        const float* table = table_.data();
        int idx = pos_, phase = phase_, n = 0;
        while (idx < numIn)
        {
            // The dot product of one filter phase with the input window: four
            // running sums, one per vector lane (the plain C version adds in the
            // same order, so both give the same result).
            const float* c = table + (size_t)phase * kTaps;
            const float* x = buf + idx;
#if defined(__ARM_NEON)
            float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f);
            for (int t = 0; t < kTaps; t += 8)
            {
                acc0 = vmlaq_f32(acc0, vld1q_f32(c + t), vld1q_f32(x + t));
                acc1 = vmlaq_f32(acc1, vld1q_f32(c + t + 4), vld1q_f32(x + t + 4));
            }
            const float32x4_t acc = vaddq_f32(acc0, acc1);
            const float32x2_t half = vadd_f32(vget_low_f32(acc), vget_high_f32(acc));
            out[n++] = vget_lane_f32(vpadd_f32(half, half), 0);
#else
            float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
            for (int t = 0; t < kTaps; t += 4)
            {
                a0 += c[t] * x[t];
                a1 += c[t + 1] * x[t + 1];
                a2 += c[t + 2] * x[t + 2];
                a3 += c[t + 3] * x[t + 3];
            }
            out[n++] = (a0 + a2) + (a1 + a3);
#endif
            // Advance by M/L input samples, without a division.
            phase += M_;
            while (phase >= L_) { phase -= L_; ++idx; }
        }
        pos_ = idx - numIn;
        phase_ = phase;
        std::memmove(buf, buf + numIn, sizeof(float) * (size_t)(kTaps - 1));
        return n;
    }

private:
    // outRate/inRate as a reduced fraction L/M. Rates are treated as whole Hz.
    static bool reduce(double inRate, double outRate, int& L, int& M)
    {
        const long long a = std::llround(outRate), b = std::llround(inRate);
        if (a <= 0 || b <= 0) return false;
        const long long g = std::gcd(a, b);
        L = (int)(a / g);
        M = (int)(b / g);
        return true;
    }

    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1e-12 * sum) break;
        }
        return sum;
    }

    // Kaiser-windowed sinc prototype at rate inRate * L, split into L phases.
    // table_[p * kTaps + t] multiplies the t-th oldest sample of the window.
    void designTable(double inRate, double outRate)
    {
        const int    n      = L_ * kTaps;
        const double centre = delay_ * L_;                            // in prototype samples
        const double half   = 0.5 * n;                                // window half-width
        const double beta   = 10.06;                                  // Kaiser window, 100 dB stopband
        const double cutoff = 0.5 * 0.98 * (inRate < outRate ? inRate : outRate);   // Hz
        const double fc     = cutoff / (inRate * L_);                 // cycles per prototype sample
        const double pi     = 3.14159265358979323846;
        const double i0b    = besselI0(beta);

        table_.assign((size_t)n, 0.0f);
        std::vector<double> phaseTaps((size_t)kTaps);
        for (int p = 0; p < L_; ++p)
        {
            double sum = 0.0;
            for (int t = 0; t < kTaps; ++t)
            {
                // The newest sample of the window (t = kTaps - 1) meets prototype tap p.
                const int    k = p + (kTaps - 1 - t) * L_;
                const double x = k - centre;
                const double s = (std::fabs(x) < 1e-9) ? 2.0 * fc : std::sin(2.0 * pi * fc * x) / (pi * x);
                const double r = x / half;
                const double w = besselI0(beta * std::sqrt(r * r < 1.0 ? 1.0 - r * r : 0.0)) / i0b;
                phaseTaps[(size_t)t] = s * w;
                sum += phaseTaps[(size_t)t];
            }
            // Each phase is normalised to unity gain at DC, so a steady input
            // gives a steady output with no ripple at the phase rate.
            const double g = (std::fabs(sum) > 1e-12) ? 1.0 / sum : 0.0;
            for (int t = 0; t < kTaps; ++t) table_[(size_t)p * kTaps + (size_t)t] = (float)(phaseTaps[(size_t)t] * g);
        }
    }

    int L_ = 1, M_ = 1;
    int maxInput_ = 0;
    double delay_ = 0.5 * kTaps;
    int pos_ = 0;      // index in buf_ of the oldest sample of the next window
    int phase_ = 0;    // 0..L_-1
    std::vector<float> table_;
    std::vector<float> buf_;   // kTaps-1 samples of history, then the incoming block
};

} // namespace namplayer

#endif // NAMPLAYER_RESAMPLER_H
