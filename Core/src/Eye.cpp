#include "sieda/Eye.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "sieda/Channel.hpp"

namespace sieda {

namespace {

double sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

size_t nextPow2(double n) {
    size_t p = 1;
    while (static_cast<double>(p) < n && p < (static_cast<size_t>(1) << 22)) p <<= 1;
    return p;
}

/// Q such that ½·erfc(Q/√2) = ber (bisection).
double qOfBer(double ber) {
    ber = std::clamp(ber, 1e-30, 0.4);
    double lo = 0, hi = 40;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        (0.5 * std::erfc(mid / std::sqrt(2.0)) > ber ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

/// Real time samples of a spectrum given on bins 0…K (Δf = 1/(N·dt)), zero above, raised-cosine taper on the top 20 %.
std::vector<double> toTime(const std::vector<cplx>& spec, size_t N, double dt) {
    std::vector<cplx> x(N, cplx(0, 0));
    const size_t K = std::min(spec.size(), N / 2 + 1);
    const size_t taper0 = K * 8 / 10;
    for (size_t k = 0; k < K; ++k) {
        double w = 1;
        if (k > taper0 && K > taper0 + 1) w = 0.5 * (1 + std::cos(kPi * static_cast<double>(k - taper0) / static_cast<double>(K - 1 - taper0)));
        const cplx v = spec[k] * w / dt;
        x[k] = v;
        if (k > 0 && k < N - k) x[N - k] = std::conj(v);
    }
    if (K == N / 2 + 1) x[N / 2] = cplx(x[N / 2].real(), 0);
    fft(x, true);
    std::vector<double> out(N);
    for (size_t n = 0; n < N; ++n) out[n] = x[n].real();
    return out;
}

/// Solves a small dense real system in place (Gaussian elimination, partial pivoting); false when singular.
bool solveReal(std::vector<std::vector<double>> a, std::vector<double>& b) {
    const size_t n = b.size();
    for (size_t c = 0; c < n; ++c) {
        size_t p = c;
        for (size_t r = c + 1; r < n; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-15) return false;
        std::swap(a[p], a[c]);
        std::swap(b[p], b[c]);
        for (size_t r = 0; r < n; ++r) {
            if (r == c) continue;
            const double f = a[r][c] / a[c][c];
            for (size_t k = c; k < n; ++k) a[r][k] -= f * a[c][k];
            b[r] -= f * b[c];
        }
    }
    for (size_t c = 0; c < n; ++c) b[c] /= a[c][c];
    return true;
}

struct Pulse {
    std::vector<double> p;  // one period of N samples
    size_t peak = 0;
};

size_t argmaxSigned(const std::vector<double>& p) {
    size_t best = 0;
    for (size_t n = 1; n < p.size(); ++n)
        if (p[n] > p[best]) best = n;
    return best;
}

/// Cursors of `p` at phase j (samples after peak − spu/2), from −pre to +post UI.
std::vector<double> cursorsAt(const std::vector<double>& p, size_t peak, int spu, int j, int pre, int post) {
    const long N = static_cast<long>(p.size());
    std::vector<double> c;
    const long s0 = static_cast<long>(peak) - spu / 2 + j;
    for (int i = -pre; i <= post; ++i) {
        long n = (s0 + static_cast<long>(i) * spu) % N;
        if (n < 0) n += N;
        c.push_back(p[static_cast<size_t>(n)]);
    }
    return c;
}

/// Best phase and worst-case (peak-distortion) eye height of a pulse, in units of the pulse.
std::pair<int, double> pdaBest(const std::vector<double>& p, size_t peak, int spu, int pre, int post) {
    int bestJ = spu / 2;
    double best = -std::numeric_limits<double>::infinity();
    for (int j = 0; j < spu; ++j) {
        const auto c = cursorsAt(p, peak, spu, j, pre, post);
        double isi = 0;
        for (size_t i = 0; i < c.size(); ++i)
            if (static_cast<int>(i) != pre) isi += std::fabs(c[i]);
        const double h = 2 * (c[static_cast<size_t>(pre)] - isi);
        if (h > best) {
            best = h;
            bestJ = j;
        }
    }
    return {bestJ, best};
}

/// Applies symbol-spaced taps c[m], m = −pre…post, to a periodic pulse.
std::vector<double> applyFfe(const std::vector<double>& p, const std::vector<double>& taps, int pre, int spu) {
    const long N = static_cast<long>(p.size());
    std::vector<double> out(p.size(), 0.0);
    for (size_t t = 0; t < taps.size(); ++t) {
        const long m = static_cast<long>(t) - pre;
        if (taps[t] == 0) continue;
        for (long n = 0; n < N; ++n) {
            long src = (n - m * spu) % N;
            if (src < 0) src += N;
            out[static_cast<size_t>(n)] += taps[t] * p[static_cast<size_t>(src)];
        }
    }
    return out;
}

/// Last sample after the peak (within half the window) where |p| exceeds `frac` of the peak, in UI after the peak.
int tailUi(const std::vector<double>& p, size_t peak, int spu, double frac) {
    const size_t N = p.size();
    const double lim = frac * std::fabs(p[peak]);
    size_t last = peak;
    for (size_t k = 1; k < N / 2; ++k)
        if (std::fabs(p[(peak + k) % N]) > lim) last = peak + k;
    return static_cast<int>((last - peak) / static_cast<size_t>(spu)) + 1;
}

}  // namespace

std::vector<int> prbsSequence(int order, size_t count) {
    int a = 7, b = 6;
    switch (order) {
        case 9: a = 9, b = 5; break;
        case 15: a = 15, b = 14; break;
        case 23: a = 23, b = 18; break;
        case 31: a = 31, b = 28; break;
        default: a = 7, b = 6; break;
    }
    std::vector<int> out;
    out.reserve(count);
    uint32_t reg = (a == 31) ? 0x7fffffffu : ((1u << a) - 1u);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t bit = ((reg >> (a - 1)) ^ (reg >> (b - 1))) & 1u;
        reg = ((reg << 1) | bit) & ((a == 31) ? 0x7fffffffu : ((1u << a) - 1u));
        out.push_back(static_cast<int>(bit));
    }
    return out;
}

cplx ctleResponse(double f, double dcGainDb, double peakHz) {
    const double A = std::pow(10.0, std::min(0.0, dcGainDb) / 20);
    const double fp1 = std::max(1.0, peakHz), fz = A * fp1, fp2 = 3 * fp1;
    const cplx s(0, f);
    return A * (1.0 + s / fz) / ((1.0 + s / fp1) * (1.0 + s / fp2));
}

std::vector<double> stepResponse(const TransferFn& transfer, double riseTime, double dt, double tStop) {
    if (!(dt > 0) || !(tStop > 0)) return {};
    const size_t N = std::max<size_t>(1024, nextPow2(2 * tStop / dt));
    const double df = 1 / (static_cast<double>(N) * dt);
    const double tr = std::max(1e-13, riseTime / 0.8);  // 0–100 % ramp
    const double fEval = std::min({0.5 / dt, std::max(20 / tr, 1e9), 200e9});
    const size_t K = std::min(N / 2, static_cast<size_t>(fEval / df)) + 1;
    std::vector<double> f(K);
    for (size_t k = 0; k < K; ++k) f[k] = static_cast<double>(k) * df;
    const std::vector<cplx> h = transfer(f);
    std::vector<cplx> spec(K);
    for (size_t k = 0; k < K; ++k) {
        // Derivative of the ramp step: a unit-area pulse of width tr.
        const cplx ramp = sinc(f[k] * tr) * std::exp(cplx(0, -kPi * f[k] * tr));
        spec[k] = (k < h.size() ? h[k] : cplx(0, 0)) * ramp;
    }
    const std::vector<double> d = toTime(spec, N, dt);
    std::vector<double> out;
    const size_t n = std::min(N, static_cast<size_t>(tStop / dt) + 1);
    double acc = 0;
    for (size_t k = 0; k < n; ++k) {
        acc += d[k] * dt;
        out.push_back(acc);
    }
    return out;
}

EyeResult simulateEye(const TransferFn& transfer, double amplitude, double vMid, double channelDelay, const EyeOptions& in) {
    EyeResult e;
    EyeOptions o = in;
    o.bitRate = std::clamp(o.bitRate, 1e6, 200e9);
    o.samplesPerUi = std::clamp(o.samplesPerUi, 8, 128);
    const int spu = o.samplesPerUi - o.samplesPerUi % 2;
    const double T = 1 / o.bitRate, dt = T / spu;
    const double rise = o.riseTime > 0 ? o.riseTime : 0.25 * T;
    const double tr = rise / 0.8;
    e.bitRate = o.bitRate;
    e.ui = T;
    e.dt = dt;
    e.samplesPerUi = spu;
    e.prbs = (o.prbs == 9 || o.prbs == 15 || o.prbs == 23 || o.prbs == 31) ? o.prbs : 7;
    e.vMid = vMid;
    e.amplitude = amplitude;

    // Time window and the frequency grid.
    const double window = std::max({128 * T, 80 * std::max(0.0, channelDelay) + 20 * tr, 4e-9});
    size_t N = nextPow2(window / dt);
    const size_t kMaxN = static_cast<size_t>(1) << 18;
    if (N > kMaxN) {
        N = kMaxN;
        e.notes.push_back("Time window limited to " + std::to_string(N / static_cast<size_t>(spu)) + " UI");
    }
    const double df = 1 / (static_cast<double>(N) * dt);
    const double fEval = std::min({0.5 / dt, std::max({10 / tr, 4 * o.bitRate, 1e9}), 150e9});
    const size_t K = std::min(N / 2, static_cast<size_t>(fEval / df)) + 1;
    std::vector<double> freq(K);
    for (size_t k = 0; k < K; ++k) freq[k] = static_cast<double>(k) * df;
    const std::vector<cplx> H = transfer(freq);
    if (H.size() != K) {
        e.error = "No channel response";
        return e;
    }
    std::vector<cplx> P(K);
    for (size_t k = 0; k < K; ++k)
        P[k] = T * sinc(freq[k] * T) * sinc(freq[k] * tr) * std::exp(cplx(0, -kPi * freq[k] * (T + tr)));
    const double peakHz = o.ctlePeakHz > 0 ? o.ctlePeakHz : o.bitRate / 2;
    double rawPeak = 0;
    auto pulseWith = [&](double ctleDb) {
        std::vector<cplx> spec(K);
        for (size_t k = 0; k < K; ++k) {
            cplx v = H[k] * P[k];
            if (ctleDb < 0) v *= ctleResponse(freq[k], ctleDb, peakHz);
            spec[k] = v;
        }
        std::vector<double> q = toTime(spec, N, dt);
        // A gain stage after the CTLE restores the unequalised main cursor, so eye heights compare in volts.
        const double pk = q[argmaxSigned(q)];
        if (ctleDb >= 0) rawPeak = pk;
        else if (pk > 0 && rawPeak > 0)
            for (double& x : q) x *= rawPeak / pk;
        return q;
    };
    if (o.ctle) pulseWith(0);  // reference main cursor
    const int preUi = 3;
    // CTLE: fixed or the DC gain (0 … −20 dB) with the largest worst-case eye.
    double ctleDb = o.ctle ? std::min(0.0, o.ctleDcGainDb) : 0.0;
    std::vector<double> p;
    if (o.ctle && o.ctleAuto) {
        double best = -std::numeric_limits<double>::infinity();
        for (int g = 0; g >= -20; --g) {
            std::vector<double> q = pulseWith(g);
            const size_t pk = argmaxSigned(q);
            const int post = std::min(tailUi(q, pk, spu, 1e-3), 200);
            const double h = pdaBest(q, pk, spu, preUi, post).second;
            if (h > best + 1e-12) {
                best = h;
                ctleDb = g;
                p = std::move(q);
            }
        }
    } else {
        p = pulseWith(ctleDb);
    }
    e.ctleDcGainDb = ctleDb;
    size_t peak = argmaxSigned(p);
    if (!(p[peak] > 0)) {
        e.error = "The channel passes no signal";
        return e;
    }
    // FFE.
    if (o.ffe) {
        const int pre = std::clamp(o.ffePre, 0, 4), post = std::clamp(o.ffePost, 0, 8);
        std::vector<double> taps;
        if (o.ffeAuto || o.ffeTaps.size() != static_cast<size_t>(pre + post + 1)) {
            const int j = pdaBest(p, peak, spu, preUi, std::min(tailUi(p, peak, spu, 1e-3), 200)).first;
            const int span = pre + post;
            const auto h = cursorsAt(p, peak, spu, j, span, span);  // h[span + i] = cursor i
            const size_t n = static_cast<size_t>(pre + post + 1);
            std::vector<std::vector<double>> a(n, std::vector<double>(n, 0.0));
            std::vector<double> b(n, 0.0);
            for (int k = -pre; k <= post; ++k)
                for (int m = -pre; m <= post; ++m) {
                    const int idx = k - m + span;
                    a[static_cast<size_t>(k + pre)][static_cast<size_t>(m + pre)] =
                        idx >= 0 && idx < static_cast<int>(h.size()) ? h[static_cast<size_t>(idx)] : 0.0;
                }
            b[static_cast<size_t>(pre)] = 1;
            if (solveReal(a, b)) taps = b;
            else {
                taps.assign(n, 0.0);
                taps[static_cast<size_t>(pre)] = 1;
            }
        } else {
            taps = o.ffeTaps;
        }
        double sum = 0;
        for (double t : taps) sum += std::fabs(t);
        if (sum > 0)
            for (double& t : taps) t /= sum;
        p = applyFfe(p, taps, pre, spu);
        e.ffeTaps = taps;
        peak = argmaxSigned(p);
        if (!(p[peak] > 0)) {
            e.error = "The equalised channel passes no signal";
            return e;
        }
    }
    int post = tailUi(p, peak, spu, 1e-4);
    if (post > 400) {
        post = 400;
        e.notes.push_back("Pulse response longer than 400 UI: the tail beyond is ignored");
    }
    const int pre = preUi;
    const auto [jBest, pda] = pdaBest(p, peak, spu, pre, post);
    e.pdaHeight = std::max(0.0, pda) * amplitude;

    // Bit stream: one full PRBS period (circular) when it fits, else a linear run.
    const size_t period = (static_cast<size_t>(1) << e.prbs) - 1;
    const bool circular = period <= std::max<size_t>(127, o.maxBits);
    const size_t M = circular ? period : std::max<size_t>(1024, o.maxBits);
    const std::vector<int> bits = prbsSequence(e.prbs, M);
    std::vector<int> s(M);
    for (size_t k = 0; k < M; ++k) s[k] = bits[k] ? 1 : -1;
    e.bits = M;
    if (!circular) e.notes.push_back("PRBS" + std::to_string(e.prbs) + ": the first " + std::to_string(M) + " bits");
    // Kernel q[i][j] = p(s0 + i·UI + j), i = −pre … post.
    const long Nl = static_cast<long>(N);
    const long s0 = static_cast<long>(peak) - spu / 2;
    const int L = pre + post + 1;
    std::vector<double> kern(static_cast<size_t>(L * spu));
    for (int i = -pre; i <= post; ++i)
        for (int j = 0; j < spu; ++j) {
            long n = (s0 + static_cast<long>(i) * spu + j) % Nl;
            if (n < 0) n += Nl;
            kern[static_cast<size_t>((i + pre) * spu + j)] = p[static_cast<size_t>(n)];
        }
    std::vector<double> y(M * static_cast<size_t>(spu), 0.0);
    for (size_t k = 0; k < M; ++k) {
        double* row = &y[k * static_cast<size_t>(spu)];
        for (int i = -pre; i <= post; ++i) {
            long src = static_cast<long>(k) - i;
            if (circular) {
                src %= static_cast<long>(M);
                if (src < 0) src += static_cast<long>(M);
            } else if (src < 0 || src >= static_cast<long>(M)) {
                continue;
            }
            const double sv = s[static_cast<size_t>(src)] * amplitude;
            const double* q = &kern[static_cast<size_t>((i + pre) * spu)];
            for (int j = 0; j < spu; ++j) row[j] += sv * q[j];
        }
    }
    const size_t k0 = circular ? 0 : static_cast<size_t>(post + pre);
    const size_t k1 = circular ? M : M - static_cast<size_t>(pre);
    auto sample = [&](long k, long j) {
        long n = k * spu + j;
        const long total = static_cast<long>(M) * spu;
        if (circular) {
            n %= total;
            if (n < 0) n += total;
        } else {
            n = std::clamp(n, 0L, total - 1);
        }
        return y[static_cast<size_t>(n)];
    };
    // Inner contour per phase, 2 UI centred on the best phase.
    const int cols = 2 * spu;
    e.cols = cols;
    e.upper.assign(static_cast<size_t>(cols), 0.0);
    e.lower.assign(static_cast<size_t>(cols), 0.0);
    std::vector<double> up(static_cast<size_t>(cols), std::numeric_limits<double>::infinity()),
        lo(static_cast<size_t>(cols), -std::numeric_limits<double>::infinity());
    double ymin = std::numeric_limits<double>::infinity(), ymax = -ymin;
    for (size_t k = k0 + 1; k + 1 < k1; ++k)
        for (int c = 0; c < cols; ++c) {
            const double v = sample(static_cast<long>(k), jBest - spu + c);
            if (s[k] > 0) up[static_cast<size_t>(c)] = std::min(up[static_cast<size_t>(c)], v);
            else lo[static_cast<size_t>(c)] = std::max(lo[static_cast<size_t>(c)], v);
            ymin = std::min(ymin, v);
            ymax = std::max(ymax, v);
        }
    if (!std::isfinite(ymin)) {
        e.error = "Too few bits";
        return e;
    }
    for (int c = 0; c < cols; ++c) {
        e.upper[static_cast<size_t>(c)] = vMid + (std::isfinite(up[static_cast<size_t>(c)]) ? up[static_cast<size_t>(c)] : ymax);
        e.lower[static_cast<size_t>(c)] = vMid + (std::isfinite(lo[static_cast<size_t>(c)]) ? lo[static_cast<size_t>(c)] : ymin);
    }
    const double eh = up[static_cast<size_t>(spu)] - lo[static_cast<size_t>(spu)];
    e.eyeHeight = std::max(0.0, eh);
    e.open = eh > 0;
    e.bestPhase = jBest * dt;
    // Threshold crossings of every transition, around the nominal boundary half a UI after the centre.
    std::vector<double> cross;
    size_t closed = 0;
    for (size_t k = k0 + 1; k + 2 < k1; ++k) {
        if (s[k] == s[k + 1]) continue;
        const long a = static_cast<long>(k) * spu + jBest, b = a + spu;
        const double nominal = a + spu / 2.0;
        double best = std::numeric_limits<double>::infinity();
        for (long n = a; n < b; ++n) {
            const double v0 = sample(0, n), v1 = sample(0, n + 1);
            if ((v0 < 0) == (v1 < 0)) continue;
            const double t = static_cast<double>(n) + v0 / (v0 - v1);
            if (std::fabs(t - nominal) < std::fabs(best - nominal)) best = t;
        }
        if (std::isfinite(best)) cross.push_back((best - nominal) * dt);
        else ++closed;
    }
    if (!cross.empty()) {
        const auto [mn, mx] = std::minmax_element(cross.begin(), cross.end());
        e.djPeakToPeak = *mx - *mn;
        double mean = 0, var = 0;
        for (double c : cross) mean += c;
        mean /= static_cast<double>(cross.size());
        for (double c : cross) var += (c - mean) * (c - mean);
        e.jitterRms = std::sqrt(var / static_cast<double>(cross.size()));
    }
    const double q = qOfBer(o.ber);
    e.totalJitter = e.djPeakToPeak + 2 * q * std::max(0.0, o.rjRms);
    if (e.open && closed == 0) {
        e.eyeWidth = std::max(0.0, T - e.djPeakToPeak);
        e.eyeWidthBer = std::max(0.0, T - e.totalJitter);
    } else if (closed) {
        e.notes.push_back(std::to_string(closed) + " transitions never cross the threshold: the eye is closed");
        e.open = false;
    }
    // Cursors at the best phase (V).
    {
        const int show = std::min(post, 16);
        const auto c = cursorsAt(p, peak, spu, jBest, pre, show);
        for (double v : c) e.cursors.push_back(v * amplitude);
        e.mainCursor = pre;
    }
    // Mask: a hexagon centred on the eye.
    if (o.maskWidthUi > 0 && o.maskHeight > 0) {
        const double W = std::min(0.99, o.maskWidthUi) * T, Hm = o.maskHeight;
        double margin = std::numeric_limits<double>::infinity();
        for (int c = 0; c < cols; ++c) {
            const double x = std::fabs(c - spu) * dt;
            if (x > W / 2) continue;
            const double m = Hm / 2 * std::min(1.0, (W / 2 - x) / (W / 4));
            margin = std::min({margin, up[static_cast<size_t>(c)] - m, -m - lo[static_cast<size_t>(c)]});
        }
        e.maskMargin = std::isfinite(margin) ? margin : 0;
        e.maskPass = e.maskMargin >= 0;
    }
    // Density plot.
    {
        const int rows = 64;
        e.rows = rows;
        const double pad = 0.05 * std::max(1e-6, ymax - ymin);
        e.vMin = vMid + ymin - pad;
        e.vMax = vMid + ymax + pad;
        std::vector<double> hits(static_cast<size_t>(rows * cols), 0.0);
        const double span = e.vMax - e.vMin;
        for (size_t k = k0 + 1; k + 1 < k1; ++k)
            for (int c = 0; c < cols; ++c) {
                const double v = vMid + sample(static_cast<long>(k), jBest - spu + c);
                const int r = std::clamp(static_cast<int>((e.vMax - v) / span * rows), 0, rows - 1);
                hits[static_cast<size_t>(r * cols + c)] += 1;
            }
        double mx = 0;
        for (double h : hits) mx = std::max(mx, h);
        e.density.resize(hits.size());
        for (size_t i = 0; i < hits.size(); ++i) e.density[i] = mx > 0 ? std::log1p(hits[i]) / std::log1p(mx) : 0;
    }
    // Pulse response for the plot: from 2 UI before the peak to the tail, ≤ 400 points.
    {
        const long from = static_cast<long>(peak) - 2L * spu - spu / 2, to = static_cast<long>(peak) + static_cast<long>(std::min(post, 64) + 1) * spu;
        const long step = std::max(1L, (to - from) / 400);
        for (long n = from; n <= to; n += step) {
            long idx = n % Nl;
            if (idx < 0) idx += Nl;
            e.pulseTime.push_back((n - from) * dt);
            e.pulse.push_back(p[static_cast<size_t>(idx)] * amplitude);
        }
    }
    return e;
}

Json eyeJson(const EyeResult& e) {
    Json j = Json::object();
    auto arr = [](const std::vector<double>& v) {
        Json a = Json::array();
        for (double x : v) a.push(std::isfinite(x) ? x : 0.0);
        return a;
    };
    j["error"] = e.error;
    j["bitRate"] = e.bitRate;
    j["ui"] = e.ui;
    j["prbs"] = e.prbs;
    j["bits"] = static_cast<double>(e.bits);
    j["samplesPerUi"] = e.samplesPerUi;
    j["vMid"] = e.vMid;
    j["amplitude"] = e.amplitude;
    j["eyeHeight"] = e.eyeHeight;
    j["eyeWidth"] = e.eyeWidth;
    j["eyeWidthBer"] = e.eyeWidthBer;
    j["bestPhase"] = e.bestPhase;
    j["pdaHeight"] = e.pdaHeight;
    j["djPeakToPeak"] = e.djPeakToPeak;
    j["jitterRms"] = e.jitterRms;
    j["totalJitter"] = e.totalJitter;
    j["open"] = e.open;
    j["maskMargin"] = e.maskMargin;
    j["maskPass"] = e.maskPass;
    j["ctleDcGainDb"] = e.ctleDcGainDb;
    j["ffeTaps"] = arr(e.ffeTaps);
    j["cursors"] = arr(e.cursors);
    j["mainCursor"] = e.mainCursor;
    j["cols"] = e.cols;
    j["rows"] = e.rows;
    j["vMin"] = e.vMin;
    j["vMax"] = e.vMax;
    j["density"] = arr(e.density);
    j["upper"] = arr(e.upper);
    j["lower"] = arr(e.lower);
    j["pulseTime"] = arr(e.pulseTime);
    j["pulse"] = arr(e.pulse);
    Json notes = Json::array();
    for (const auto& n : e.notes) notes.push(n);
    j["notes"] = notes;
    return j;
}

EyeOptions eyeOptionsFromJson(const Json& j) {
    EyeOptions o;
    o.bitRate = std::clamp(j.get("bitRate").asNumber(5e9), 1e6, 200e9);
    o.prbs = j.get("prbs").asInt(7);
    o.samplesPerUi = std::clamp(j.get("samplesPerUi").asInt(32), 8, 128);
    o.riseTime = std::clamp(j.get("riseTime").asNumber(0), 0.0, 1e-6);
    o.ctle = j.get("ctle").asBool(false);
    o.ctleAuto = j.get("ctleAuto").asBool(false);
    o.ctleDcGainDb = std::clamp(j.get("ctleDcGainDb").asNumber(-6), -30.0, 0.0);
    o.ctlePeakHz = std::clamp(j.get("ctlePeakHz").asNumber(0), 0.0, 200e9);
    o.ffe = j.get("ffe").asBool(false);
    o.ffeAuto = j.get("ffeAuto").asBool(false);
    o.ffePre = std::clamp(j.get("ffePre").asInt(1), 0, 4);
    o.ffePost = std::clamp(j.get("ffePost").asInt(2), 0, 8);
    if (j.get("ffeTaps").isArray())
        for (const auto& t : j.get("ffeTaps").items())
            if (t.isNumber() && std::isfinite(t.asNumber())) o.ffeTaps.push_back(std::clamp(t.asNumber(), -1.0, 1.0));
    o.rjRms = std::clamp(j.get("rjRms").asNumber(0), 0.0, 1e-9);
    o.ber = std::clamp(j.get("ber").asNumber(1e-12), 1e-20, 1e-3);
    o.maskWidthUi = std::clamp(j.get("maskWidthUi").asNumber(0), 0.0, 0.99);
    o.maskHeight = std::clamp(j.get("maskHeight").asNumber(0), 0.0, 100.0);
    o.maxBits = static_cast<size_t>(std::clamp(j.get("maxBits").asInt(32767), 127, 131071));
    return j.isObject() ? o : EyeOptions();
}

}  // namespace sieda
