#include "sieda/Waveforms.hpp"

#include <algorithm>
#include <cmath>

namespace sieda {

WaveformMeasurements measureWaveform(const std::vector<double>& time, const std::vector<double>& values, double from,
                                     double to) {
    WaveformMeasurements m;
    const size_t n = std::min(time.size(), values.size());
    if (n < 2) {
        m.error = "The waveform needs at least two samples.";
        return m;
    }
    for (size_t i = 0; i < n; ++i)
        if (!std::isfinite(time[i]) || !std::isfinite(values[i]) || (i > 0 && time[i] < time[i - 1])) {
            m.error = "The waveform must have finite values at ascending times.";
            return m;
        }
    if (std::isfinite(from) && std::isfinite(to) && from > to) std::swap(from, to);
    const double lo = std::isfinite(from) ? from : time[0], hi = std::isfinite(to) ? to : time[n - 1];
    std::vector<double> t, v;
    for (size_t i = 0; i < n; ++i)
        if (time[i] >= lo && time[i] <= hi) {
            t.push_back(time[i]);
            v.push_back(values[i]);
        }
    if (t.size() < 2 || !(t.back() > t.front())) {
        m.error = "The window holds fewer than two samples: widen it.";
        return m;
    }
    m.from = t.front();
    m.to = t.back();
    m.samples = static_cast<int>(t.size());
    const size_t k = t.size();

    // Extremes, time averages.
    size_t imin = 0, imax = 0;
    double area = 0, area2 = 0;
    for (size_t i = 0; i < k; ++i) {
        if (v[i] < v[imin]) imin = i;
        if (v[i] > v[imax]) imax = i;
        if (i > 0) {
            const double dt = t[i] - t[i - 1];
            area += 0.5 * (v[i] + v[i - 1]) * dt;
            // Exact for a linear segment: ∫(a + (b − a)s)² ds = (a² + ab + b²) / 3.
            area2 += (v[i - 1] * v[i - 1] + v[i - 1] * v[i] + v[i] * v[i]) / 3.0 * dt;
        }
    }
    const double span = t.back() - t.front();
    m.min = v[imin];
    m.max = v[imax];
    m.tMin = t[imin];
    m.tMax = t[imax];
    m.peakToPeak = m.max - m.min;
    m.average = area / span;
    m.rms = std::sqrt(std::max(0.0, area2 / span));
    m.acRms = std::sqrt(std::max(0.0, area2 / span - m.average * m.average));
    m.initial = v.front();
    m.final = v.back();
    if (!(m.peakToPeak > 0)) {
        m.ok = true;  // a flat line: no edges, no period
        return m;
    }

    // Edge levels: of the step when the window holds one, else of min … max.
    const double step = m.final - m.initial;
    m.stepLike = std::fabs(step) >= 0.5 * m.peakToPeak;
    const double base = m.stepLike ? std::min(m.initial, m.final) : m.min;
    const double top = m.stepLike ? std::max(m.initial, m.final) : m.max;
    const double l10 = base + 0.1 * (top - base), l90 = base + 0.9 * (top - base);
    auto crossTime = [&](size_t i, double level) {  // between samples i−1 and i
        const double a = v[i - 1], b = v[i];
        return b == a ? t[i] : t[i - 1] + (t[i] - t[i - 1]) * (level - a) / (b - a);
    };
    // Rise: the first upward crossing of 10 % followed by an upward crossing of 90 % (no return below 10 % between).
    for (size_t i = 1; i < k && std::isnan(m.riseTime); ++i) {
        if (!(v[i - 1] < l10 && v[i] >= l10)) continue;
        const double t10 = crossTime(i, l10);
        for (size_t j = i; j < k; ++j) {
            if (j > i && v[j] < l10) break;
            if (j > 0 && v[j - 1] < l90 && v[j] >= l90) {
                m.riseTime = crossTime(j, l90) - t10;
                break;
            }
        }
    }
    for (size_t i = 1; i < k && std::isnan(m.fallTime); ++i) {
        if (!(v[i - 1] > l90 && v[i] <= l90)) continue;
        const double t90 = crossTime(i, l90);
        for (size_t j = i; j < k; ++j) {
            if (j > i && v[j] > l90) break;
            if (j > 0 && v[j - 1] > l10 && v[j] <= l10) {
                m.fallTime = crossTime(j, l10) - t90;
                break;
            }
        }
    }
    if (m.stepLike) {
        const double mag = std::fabs(step);
        m.overshootPercent = 100.0 * std::max(0.0, step > 0 ? (m.max - m.final) / mag : (m.final - m.min) / mag);
        const double band = 0.02 * mag;
        size_t last = k;  // last sample outside the ±2 % band
        for (size_t i = k; i-- > 0;)
            if (std::fabs(v[i] - m.final) > band) {
                last = i;
                break;
            }
        m.settlingTime = last == k ? 0.0 : (last + 1 < k ? t[last + 1] : t[last]) - t.front();
    }

    // Period from rising mid-level crossings with hysteresis.
    const double mid = 0.5 * (m.min + m.max), hyst = 0.1 * m.peakToPeak;
    std::vector<double> rising;
    int state = v[0] <= mid ? -1 : (v[0] > mid + hyst ? 1 : 0);  // starting at mid and rising counts as a crossing
    for (size_t i = 1; i < k; ++i) {
        if (state != 1 && v[i] > mid + hyst) {
            if (state == -1) {
                // The mid crossing on the way up (search back to where it passed mid).
                size_t j = i;
                while (j > 1 && v[j - 1] >= mid) --j;
                rising.push_back(v[j - 1] < mid && v[j] >= mid ? crossTime(j, mid) : t[j - 1]);
            }
            state = 1;
        } else if (state != -1 && v[i] < mid - hyst) {
            state = -1;
        }
    }
    if (rising.size() >= 2) {
        m.cycles = static_cast<int>(rising.size() - 1);
        m.period = (rising.back() - rising.front()) / m.cycles;
        m.frequency = 1.0 / m.period;
        // Duty: time above mid between the first and last rising crossing (segments split at mid).
        double high = 0;
        for (size_t i = 1; i < k; ++i) {
            double a = t[i - 1], b = t[i];
            if (b <= rising.front() || a >= rising.back()) continue;
            const double ca = std::max(a, rising.front()), cb = std::min(b, rising.back());
            const double va = v[i - 1] + (v[i] - v[i - 1]) * (b > a ? (ca - a) / (b - a) : 0.0);
            const double vb = v[i - 1] + (v[i] - v[i - 1]) * (b > a ? (cb - a) / (b - a) : 0.0);
            if (va >= mid && vb >= mid) high += cb - ca;
            else if (va >= mid || vb >= mid) {
                const double tc = vb == va ? ca : ca + (cb - ca) * (mid - va) / (vb - va);
                high += va >= mid ? tc - ca : cb - tc;
            }
        }
        m.dutyCycle = high / (rising.back() - rising.front());
    }
    m.ok = true;
    return m;
}

Json measureWaveformJson(const Json& request) {
    auto numbers = [](const Json& a) {
        std::vector<double> v;
        if (a.isArray())
            for (size_t i = 0; i < a.size(); ++i) v.push_back(a[i].isNumber() ? a[i].asNumber() : NAN);
        return v;
    };
    const Json& f = request.get("from");
    const Json& t = request.get("to");
    WaveformMeasurements m = measureWaveform(numbers(request.get("time")), numbers(request.get("values")),
                                             f.isNumber() ? f.asNumber() : NAN, t.isNumber() ? t.asNumber() : NAN);
    Json j = Json::object();
    j["ok"] = m.ok;
    j["error"] = m.error;
    if (!m.ok) return j;
    j["from"] = m.from;
    j["to"] = m.to;
    j["samples"] = m.samples;
    j["min"] = m.min;
    j["max"] = m.max;
    j["tMin"] = m.tMin;
    j["tMax"] = m.tMax;
    j["peakToPeak"] = m.peakToPeak;
    j["average"] = m.average;
    j["rms"] = m.rms;
    j["acRms"] = m.acRms;
    j["initial"] = m.initial;
    j["final"] = m.final;
    j["stepLike"] = m.stepLike;
    j["riseTime"] = m.riseTime;  // NaN → null
    j["fallTime"] = m.fallTime;
    j["overshootPercent"] = m.overshootPercent;
    j["settlingTime"] = m.settlingTime;
    j["period"] = m.period;
    j["frequency"] = m.frequency;
    j["dutyCycle"] = m.dutyCycle;
    j["cycles"] = m.cycles;
    return j;
}

}  // namespace sieda
