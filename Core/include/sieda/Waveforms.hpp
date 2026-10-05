// SiEDA Core — waveform measurements (SPICE ".meas"-like): extremes, average, RMS, rise / fall time, overshoot,
// settling, frequency, period and duty cycle of a sampled waveform over a time window. Guide: docs/SIMULATION.md.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "sieda/Json.hpp"

namespace sieda {

struct WaveformMeasurements {
    bool ok = false;
    std::string error;
    double from = 0, to = 0;  // the window measured (s)
    int samples = 0;
    double min = 0, max = 0, tMin = 0, tMax = 0, peakToPeak = 0;
    double average = 0, rms = 0, acRms = 0;  // time-weighted (trapezoidal); acRms: RMS about the average
    double initial = 0, final = 0;           // first and last value in the window
    bool stepLike = false;  // the window holds a step (|final − initial| ≥ half the peak-to-peak): levels from it
    double riseTime = NAN, fallTime = NAN;   // 10 % → 90 % (90 % → 10 %) of the step, or of min…max otherwise
    double overshootPercent = NAN;           // beyond the final value, % of the step (step-like windows)
    double settlingTime = NAN;               // from the window start until it stays within ±2 % of the step
    double period = NAN, frequency = NAN, dutyCycle = NAN;  // from mid-level crossings with 10 % hysteresis
    int cycles = 0;
};

/// Measures `values` at `time` (ascending) between `from` and `to` (NaN: the whole waveform).
WaveformMeasurements measureWaveform(const std::vector<double>& time, const std::vector<double>& values,
                                     double from = NAN, double to = NAN);

/// C API form: request {"time":[…],"values":[…],"from"?,"to"?}; result documented at sieda_measure_waveform.
Json measureWaveformJson(const Json& request);

}  // namespace sieda
