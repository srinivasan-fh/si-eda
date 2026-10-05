// SiEDA Core — statistical-free eye diagram of a linear channel: an NRZ PRBS bit stream through the channel's
// frequency response, by superposition of its pulse response (exact for a linear, time-invariant channel).
//
//   Pulse response  p(t) = IFFT[H(f)·P(f)], P the spectrum of one unit interval with linear edges (the driver's rise
//                   time); H sampled up to where P has decayed (≥ 4 / t_edge, ≥ 3 × the bit rate), a raised-cosine
//                   taper above. Time window ≥ 128 UI and ≥ 80 × the channel delay, so reflections settle.
//   Waveform        y(t) = v_mid + A·Σ s_k·p_eff(t − kT), s_k = ±1 from PRBS 7 / 9 / 15 / 23 / 31 (x⁷+x⁶+1,
//                   x⁹+x⁵+1, x¹⁵+x¹⁴+1, x²³+x¹⁸+1, x³¹+x²⁸+1), circular over a full period when it fits.
//   Equalisation    Optional transmit FFE (taps on the symbols, Σ|c| = 1; "auto" = zero-forcing on the pre- / post-
//                   cursors) and a receive CTLE H(s) = A·(1 + s/ωz) / ((1 + s/ωp1)(1 + s/ωp2)), ωz = A·ωp1, ωp2 = 3ωp1
//                   (DC gain A, unity high-frequency gain, then a gain stage restoring the unequalised main cursor;
//                   "auto" sweeps A from 0 to −20 dB for the largest worst-case eye).
//   Measurements    Eye height at the best sampling phase (inner contour), eye width at the decision threshold,
//                   deterministic jitter (data-dependent, from every threshold crossing), total jitter at the target BER
//                   with an optional random jitter (dual-Dirac: TJ = DJ + 2·Q(BER)·RJ), the worst-case eye of peak
//                   distortion analysis, and a hexagonal mask (width in UI, height in V) with its margin.
// No noise, crosstalk aggressors, DFE or receiver non-linearity are modelled.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/LossyLine.hpp"

namespace sieda {

struct EyeOptions {
    double bitRate = 5e9;   // b/s
    int prbs = 7;           // 7, 9, 15, 23, 31
    int samplesPerUi = 32;  // 16 … 128
    double riseTime = 0;    // 10–90 % edge of the driver (s); 0 = 0.25 UI
    bool ctle = false, ctleAuto = false;
    double ctleDcGainDb = -6;  // ≤ 0
    double ctlePeakHz = 0;     // first pole; 0 = Nyquist (bit rate / 2)
    bool ffe = false, ffeAuto = false;
    std::vector<double> ffeTaps;  // [pre…, main, post…], ffePre taps before the main one
    int ffePre = 1, ffePost = 2;
    double rjRms = 0;      // random jitter (s RMS)
    double ber = 1e-12;
    double maskWidthUi = 0, maskHeight = 0;  // hexagon; 0 = no mask
    size_t maxBits = 32767;
};

struct EyeResult {
    std::string error;
    double bitRate = 0, ui = 0, dt = 0;
    int prbs = 7;
    size_t bits = 0;
    int samplesPerUi = 32;
    double vMid = 0, amplitude = 0;
    double eyeHeight = 0;      // V, inner opening at the best phase
    double eyeWidth = 0;       // s, at the threshold (UI − DJ)
    double eyeWidthBer = 0;    // s, UI − TJ(BER)
    double bestPhase = 0;      // s from the start of the UI window (eye centre)
    double pdaHeight = 0;      // V, worst-case (peak distortion) eye height
    double djPeakToPeak = 0, jitterRms = 0, totalJitter = 0;
    bool open = false;
    double maskMargin = 0;  // V (mask set): ≥ 0 passes
    bool maskPass = true;
    double ctleDcGainDb = 0;  // applied (0 = none)
    std::vector<double> ffeTaps;  // applied
    std::vector<double> cursors;  // main cursor and ±ISI cursors at the best phase (V, ×amplitude)
    int mainCursor = 0;           // index of the main cursor in `cursors`
    // Plot: 2 UI wide, centred on the eye.
    int cols = 0, rows = 0;
    double vMin = 0, vMax = 0;
    std::vector<double> density;  // rows × cols, row 0 = vMax, normalised 0…1 (log-scaled hits)
    std::vector<double> upper, lower;  // inner contour per column (V), NaN where undefined
    std::vector<double> pulseTime, pulse;  // pulse response (decimated)
    std::vector<std::string> notes;
};

/// PRBS bits (0 / 1) of `order` (7, 9, 15, 23, 31), `count` of them from the all-ones seed.
std::vector<int> prbsSequence(int order, size_t count);
/// CTLE response at f.
cplx ctleResponse(double f, double dcGainDb, double peakHz);
/// Eye of the channel with frequency response `transfer` (source unit amplitude → receiver), symbol amplitude `amplitude`
/// (V per ±1 symbol: half the swing) around `vMid`. `channelDelay` (s) sizes the time window.
/// Frequency response at a batch of frequencies (Hz).
using TransferFn = std::function<std::vector<cplx>(const std::vector<double>& freq)>;
EyeResult simulateEye(const TransferFn& transfer, double amplitude, double vMid, double channelDelay,
                      const EyeOptions& opt);
/// Step response (V per volt of source step) of `transfer` for an edge of `riseTime` (10–90 %), sampled at `dt` up to
/// `tStop`.
std::vector<double> stepResponse(const TransferFn& transfer, double riseTime, double dt, double tStop);
Json eyeJson(const EyeResult& e);
EyeOptions eyeOptionsFromJson(const Json& j);

}  // namespace sieda
