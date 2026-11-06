#pragma once

#include <algorithm>
#include <cmath>
#include <complex>

/** RBJ biquad coefficients (normalised, a0 = 1). */
struct BiquadCoeffs
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;

    static BiquadCoeffs lowPass (double sr, double freq, double q)
    {
        const double w0 = 6.283185307179586 * freq / sr;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        return make ((1.0 - cw) * 0.5, 1.0 - cw, (1.0 - cw) * 0.5, a0, -2.0 * cw, 1.0 - alpha);
    }

    static BiquadCoeffs highPass (double sr, double freq, double q)
    {
        const double w0 = 6.283185307179586 * freq / sr;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        return make ((1.0 + cw) * 0.5, -(1.0 + cw), (1.0 + cw) * 0.5, a0, -2.0 * cw, 1.0 - alpha);
    }

    /** Complex frequency response at normalised angular frequency w (radians/sample). */
    std::complex<double> responseAt (double w) const
    {
        const std::complex<double> z1 = std::polar (1.0, -w);
        const std::complex<double> z2 = std::polar (1.0, -2.0 * w);
        const std::complex<double> num = (double) b0 + (double) b1 * z1 + (double) b2 * z2;
        const std::complex<double> den = 1.0 + (double) a1 * z1 + (double) a2 * z2;
        return num / den;
    }

private:
    static BiquadCoeffs make (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        BiquadCoeffs c;
        c.b0 = (float) (b0 / a0);
        c.b1 = (float) (b1 / a0);
        c.b2 = (float) (b2 / a0);
        c.a1 = (float) (a1 / a0);
        c.a2 = (float) (a2 / a0);
        return c;
    }
};

struct BiquadState
{
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept { z1 = z2 = 0.0f; }

    float process (const BiquadCoeffs& c, float x) noexcept   // transposed direct form II
    {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

/** 4th-order Butterworth high-pass (low cut) followed by 4th-order low-pass (high cut). */
struct BandPassCoeffs
{
    BiquadCoeffs hp[2], lp[2];

    void set (double sr, double lowHz, double highHz)
    {
        constexpr double q1 = 0.5411961, q2 = 1.3065630;   // Butterworth 4th-order section Qs
        hp[0] = BiquadCoeffs::highPass (sr, lowHz, q1);
        hp[1] = BiquadCoeffs::highPass (sr, lowHz, q2);
        lp[0] = BiquadCoeffs::lowPass  (sr, highHz, q1);
        lp[1] = BiquadCoeffs::lowPass  (sr, highHz, q2);
    }

    /** Magnitude (0..1) of the whole band-pass at normalised angular frequency w. */
    float magnitudeAt (double w) const
    {
        const std::complex<double> h = hp[0].responseAt (w) * hp[1].responseAt (w)
                                     * lp[0].responseAt (w) * lp[1].responseAt (w);
        return (float) std::min (1.0, std::abs (h));
    }
};

struct BandPassChannel
{
    BiquadState s[4];

    void reset() noexcept { for (auto& st : s) st.reset(); }

    float process (const BandPassCoeffs& c, float x) noexcept
    {
        x = s[0].process (c.hp[0], x);
        x = s[1].process (c.hp[1], x);
        x = s[2].process (c.lp[0], x);
        return s[3].process (c.lp[1], x);
    }
};

/** Feed-forward stereo-linked compressor with a 6 dB soft knee. Returns a linear gain. */
class Compressor
{
public:
    void reset() noexcept { grDb = 0.0f; }

    void setParams (double sr, float thrDb, float ratioIn, float attackMs, float releaseMs)
    {
        thresholdDb = thrDb;
        ratio = std::max (1.0f, ratioIn);
        atkCoef = std::exp (-1.0f / (std::max (0.05f, attackMs)  * 0.001f * (float) sr));
        relCoef = std::exp (-1.0f / (std::max (0.05f, releaseMs) * 0.001f * (float) sr));
    }

    float computeGain (float level) noexcept
    {
        const float levelDb = 20.0f * std::log10 (std::max (level, 1.0e-6f));
        const float over = levelDb - thresholdDb;

        float outDb;
        if (2.0f * over < -kneeDb)
            outDb = levelDb;
        else if (2.0f * std::abs (over) <= kneeDb)
        {
            const float x = over + kneeDb * 0.5f;
            outDb = levelDb + (1.0f / ratio - 1.0f) * x * x / (2.0f * kneeDb);
        }
        else
            outDb = thresholdDb + over / ratio;

        const float target = outDb - levelDb;                  // <= 0 dB
        const float c = target < grDb ? atkCoef : relCoef;     // more reduction -> attack
        grDb = target + c * (grDb - target);

        return std::exp (grDb * 0.11512925f);                  // dB -> linear
    }

private:
    static constexpr float kneeDb = 6.0f;
    float thresholdDb = -18.0f, ratio = 3.0f, atkCoef = 0.0f, relCoef = 0.0f, grDb = 0.0f;
};
