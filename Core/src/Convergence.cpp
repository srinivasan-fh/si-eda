// Convergence aids: adaptive source stepping for the DC operating point (the last resort after Newton, Gmin stepping
// and fixed source stepping) and the adaptive / trapezoidal transient with local-truncation-error step control.
#include <algorithm>
#include <cmath>
#include <deque>

#include "SimulatorInternal.hpp"
#include "sieda/Units.hpp"

namespace sieda {

using namespace simdetail;

void Simulator::limitBuiltinJunctions(const Element& e, std::array<double, 3>& v) const {
    if (e.type == ElemType::Diode) {
        const double nvt = e.emission * kVt;
        double vd = v[0] - v[1];
        if (e.vjValid) vd = pnjlim(vd, e.vjOld[0], nvt, junctionVcrit(nvt, e.is));
        e.vjOld[0] = vd;
        e.vjValid = true;
        v[0] = v[1] + vd;
    } else if (e.type == ElemType::NPN) {  // B, C, E
        double vbe = v[0] - v[2], vbc = v[0] - v[1];
        if (e.vjValid) {
            vbe = pnjlim(vbe, e.vjOld[0], kVt, junctionVcrit(kVt, e.is));
            vbc = pnjlim(vbc, e.vjOld[1], kVt, junctionVcrit(kVt, e.is));
        }
        e.vjOld = {vbe, vbc};
        e.vjValid = true;
        v[2] = v[0] - vbe;
        v[1] = v[0] - vbc;
    }
}

bool Simulator::retryWithLimiting(double t0, double h, std::string& error) {
    struct Restore {
        bool& flag;
        bool was;
        ~Restore() { flag = was; }
    } restore{limitJunctions_, limitJunctions_};
    limitJunctions_ = true;
    const std::vector<Element> saved = elements_;
    const std::vector<double> start = x_;
    // Sub-steps that halve on failure and grow on success, each committed as it converges.
    const double end = t0 + h;
    double t = t0, hs = h / 20;
    std::vector<double> trial;
    for (int guard = 0; t < end - 1e-12 * h; ++guard) {
        const double step = std::min(hs, end - t);
        trial = x_;
        int iters = 0;
        if (guard < 200000 && solve(t + step, step, trial, iters, 0.0, 1.0)) {
            x_ = trial;
            lastReadings_ = readings(x_, step);
            stepH_ = step;
            updateState();
            t += step;
            hs = std::min(hs * 2, h / 4);
            continue;
        }
        hs = step / 4;
        if (guard >= 200000 || hs < h * 1e-7) {
            elements_ = saved;
            x_ = start;
            error = "Transient analysis failed to converge at t = " + formatEngineeringValue(t, "s");
            return false;
        }
    }
    return true;
}

bool Simulator::adaptiveSourceStepping(std::vector<double>& x, int& iterations) {
    std::fill(x.begin(), x.end(), 0.0);
    int iters = 0;
    if (!solve(0, 0, x, iters, 0.0, 0.0)) return false;
    iterations += iters;
    std::vector<double> good = x;
    double level = 0, delta = 0.02;
    while (level < 1.0) {
        const double next = std::min(1.0, level + delta);
        x = good;
        bool ok = solve(0, 0, x, iters, 0.0, next);
        if (!ok) {  // the same level with a little Gmin, then without it from there
            x = good;
            ok = solve(0, 0, x, iters, 1e-6, next) && solve(0, 0, x, iters, 0.0, next);
        }
        iterations += iters;
        if (ok) {
            level = next;
            good = x;
            delta = std::min(0.25, delta * 2);
        } else {
            delta /= 4;
            if (delta < 1e-7) return false;
        }
        if (iterations > 6000) return false;  // a bound on the work (about 20 full Newton solves), never an endless ramp
    }
    x = good;
    return true;
}

std::vector<double> Simulator::sourceBreakpoints(double tStop) const {
    std::vector<double> out;
    for (const auto& e : elements_) {
        if ((e.type != ElemType::VSource && e.type != ElemType::ISource) || e.source.kind != SourceSpec::Kind::Pulse) continue;
        const SourceSpec& src = e.source;
        if (src.shape == SourceSpec::Shape::Pwl) {
            for (size_t i = 0; i < src.pwl.size(); i += 2)
                if (src.pwl[i] > 0 && src.pwl[i] <= tStop) out.push_back(src.pwl[i]);
            continue;
        }
        if (src.shape == SourceSpec::Shape::Exp) {
            for (double t : {src.td, src.td2})
                if (t > 0 && t <= tStop) out.push_back(t);
            continue;
        }
        if (src.shape == SourceSpec::Shape::Spice) {
            const double per = src.period > 0 ? src.period : 1e300;
            const double cycles = src.period > 0 ? std::floor(std::max(0.0, tStop - src.td) / per) + 1 : 1;
            if (cycles > 1e6) continue;
            for (double k = 0; k < cycles; ++k) {
                const double base = src.td + k * (src.period > 0 ? per : 0.0);
                for (double t : {base, base + src.tr, base + src.tr + src.pw, base + src.tr + src.pw + src.tf})
                    if (t > 0 && t <= tStop) out.push_back(t);
            }
            continue;
        }
        const double p = e.source.period;
        if (!(p > 0)) continue;
        const double cycles = std::floor(tStop / p) + 1;
        if (cycles > 1e6) continue;  // far too fast to resolve edge by edge: the step control alone handles it
        for (double k = 0; k < cycles; ++k) {
            for (double t : {k * p, k * p + e.source.duty * p})
                if (t > 0 && t <= tStop) out.push_back(t);
        }
    }
    out.push_back(tStop);
    std::sort(out.begin(), out.end());
    std::vector<double> unique;
    for (double t : out)
        if (unique.empty() || t - unique.back() > 1e-12 * std::max(tStop, 1e-30)) unique.push_back(t);
    return unique;
}

TransientResult Simulator::transient(const TransientOptions& o) {
    aids_ = true;
    if (!o.trapezoidal && !o.adaptive) return transient(o.tStop, o.tStep);  // fixed backward Euler, with the aids
    TransientResult res;
    if (!(o.tStop > 0) || !(o.tStep > 0) || !std::isfinite(o.tStop) || !std::isfinite(o.tStep)) {
        res.error = "Transient analysis needs positive stop time and step.";
        return res;
    }
    if (!(o.reltol > 0 && o.reltol <= 0.1) || !(o.vntol > 0) || o.maxStep < 0) {
        res.error = "Transient tolerances: reltol in (0, 0.1], vntol above 0, max step not negative.";
        return res;
    }
    trap_ = false;
    struct Limits {  // junction limiting for the new modes; the established fixed-step BE path never sets it
        bool& flag;
        ~Limits() { flag = false; }
    } limits{limitJunctions_};
    limitJunctions_ = true;
    if (!begin(res.error)) return res;
    const auto& nets = sch_.nets();
    res.netVoltages.assign(nets.size(), {});
    auto record = [&](double t) {
        res.time.push_back(t);
        for (size_t i = 0; i < nets.size(); ++i) res.netVoltages[i].push_back(nodeV(x_, netToNode_[i]));
        for (const auto& r : lastReadings_) {
            if (r.subIndex > 0) continue;
            res.currents[r.componentId].push_back(r.current);
            res.powers[r.componentId].push_back(r.power);
        }
    };
    record(0);
    struct Reset {  // the integration state flag is the simulator's: restore it however the run ends
        bool& flag;
        ~Reset() { flag = false; }
    } reset{trap_};

    if (!o.adaptive || !mcus_.empty()) {
        // Fixed steps with the chosen rule (firmware co-simulation needs the fixed step).
        trap_ = o.trapezoidal;
        double tStep = o.tStep;
        if (o.tStop / tStep > 200000) tStep = o.tStop / 200000;
        const int steps = static_cast<int>(std::ceil(o.tStop / tStep - 1e-9));
        for (int s = 1; s <= steps; ++s) {
            if (simulationStopRequested()) {
                res.error = kSimulationStopped;
                return res;
            }
            const double t = std::min(s * tStep, o.tStop);
            if (!advance(t - t_, res.error)) {
                res.mcus = mcuReports();
                return res;
            }
            record(t);
        }
        res.mcus = mcuReports();
        res.ok = true;
        return res;
    }

    // Adaptive steps: Newton at t + h; the local truncation error from divided differences of the node voltages
    // (backward Euler: h²·|x''|/2, trapezoidal: h³·|x'''|/12) against reltol·|x| + vntol, with SPICE's TRTOL of 7.
    // A rejected step is retried shorter; an accepted one sets the next. Steps land on every PULSE edge, and the
    // step after an edge is backward Euler (the trapezoidal rule rings on a discontinuity).
    const std::vector<double> breaks = sourceBreakpoints(o.tStop);
    const double hmax = o.maxStep > 0 ? o.maxStep : o.tStep;
    const double hmin = std::max(o.tStop * 1e-13, 1e-18);
    double h = std::min(o.tStep, hmax) * 0.1;
    std::deque<std::pair<double, std::vector<double>>> history;
    history.push_back({0.0, x_});
    bool afterBreak = true;
    size_t bp = 0;
    std::vector<double> trial;
    std::vector<char> inductorBranch(static_cast<size_t>(unknowns_), 0);  // inductor currents join the error estimate
    for (const auto& e : elements_)
        if (e.type == ElemType::Inductor && e.branch >= 0) inductorBranch[static_cast<size_t>(e.branch)] = 1;
    while (t_ < o.tStop * (1 - 1e-12)) {
        if (simulationStopRequested()) {
            res.error = kSimulationStopped;
            return res;
        }
        while (bp < breaks.size() && breaks[bp] <= t_ + hmin) ++bp;
        double step = std::min(h, hmax);
        bool hitsBreak = false;
        if (bp < breaks.size() && t_ + step >= breaks[bp] - hmin) {
            step = breaks[bp] - t_;
            hitsBreak = true;
        }
        const bool useTrap = o.trapezoidal && !afterBreak;
        const int order = useTrap ? 2 : 1;
        trap_ = useTrap;
        trial = x_;
        int iters = 0;
        // An ideal edge belongs to the next step: the step that lands on it sees the sources just before it.
        const double tEval = hitsBreak ? breaks[bp] - std::max(hmin, 1e-12 * breaks[bp]) : t_ + step;
        if (!solve(tEval, step, trial, iters, 0.0, 1.0)) {
            h = step / 8;
            if (h < hmin) {
                res.error = "Transient analysis failed to converge at t = " + formatEngineeringValue(t_, "s") +
                            " (time step too small).";
                return res;
            }
            continue;
        }
        // Local truncation error.
        double err = 0;
        // The step that lands on a source edge takes the discontinuity: it is not judged by its truncation error.
        const bool haveLte = !hitsBreak && static_cast<int>(history.size()) >= order + 1;
        if (haveLte) {
            const size_t m = history.size();
            const double tn = t_ + step;
            for (int i = 0; i < unknowns_; ++i) {
                const bool node = isNode_[static_cast<size_t>(i)] != 0;
                if (!node && !inductorBranch[static_cast<size_t>(i)]) continue;
                const size_t ii = static_cast<size_t>(i);
                const double x2 = trial[ii], x1 = history[m - 1].second[ii], x0 = history[m - 2].second[ii];
                const double t1 = history[m - 1].first, t0 = history[m - 2].first;
                const double d21 = (x2 - x1) / (tn - t1), d10 = (x1 - x0) / (t1 - t0);
                const double dd2 = (d21 - d10) / (tn - t0);
                double lte;
                if (order == 1) {
                    lte = step * step * std::fabs(dd2);
                } else {
                    const double xm = history[m - 3].second[ii], tm = history[m - 3].first;
                    const double d0m = (x0 - xm) / (t0 - tm);
                    const double dd2b = (d10 - d0m) / (t1 - tm);
                    const double dd3 = (dd2 - dd2b) / (tn - tm);
                    lte = step * step * step * std::fabs(dd3) / 2;
                }
                const double tol = 7.0 * (o.reltol * std::max(std::fabs(x2), std::fabs(x1)) + (node ? o.vntol : 1e-9));
                err = std::max(err, lte / tol);
            }
        }
        if (haveLte && err > 1) {  // reject
            h = step * std::max(0.25, 0.9 * std::pow(err, -1.0 / (order + 1)));
            if (h < hmin) {
                res.error = "Transient analysis: the time step fell below " + formatEngineeringValue(hmin, "s") + " at t = " +
                            formatEngineeringValue(t_, "s") + ".";
                return res;
            }
            continue;
        }
        // Accept.
        x_ = trial;
        stepH_ = step;
        lastReadings_ = readings(x_, step);
        updateState();
        t_ = hitsBreak ? breaks[bp] : t_ + step;
        record(t_);
        if (res.time.size() > 200000) {
            res.error = "Transient analysis took more than 200 000 time steps: raise the maximum step or the tolerance.";
            return res;
        }
        if (hitsBreak) {
            afterBreak = true;
            history.clear();
            history.push_back({t_, x_});
            h = std::max(hmin, std::min(o.tStep, hmax));
        } else {
            afterBreak = false;
            history.push_back({t_, x_});
            if (history.size() > 4) history.pop_front();
            const double grow = haveLte ? 0.9 * std::pow(std::max(err, 1e-12), -1.0 / (order + 1)) : 2.0;
            h = step * std::clamp(grow, 0.25, 2.0);
        }
    }
    res.ok = true;
    return res;
}

}  // namespace sieda
