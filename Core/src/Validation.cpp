#include "sieda/Validation.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "sieda/CustomParts.hpp"
#include "sieda/DeviceModels.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/Units.hpp"

namespace sieda {

namespace {
std::string fmt(double v, const char* unit) { return formatEngineeringValue(v, unit, 3); }

/// Over a rating is a warning (the default ratings are typical parts); twice the rating will destroy any
/// part of that class and is an error.
Severity overload(double value, double rating) { return value >= 2.0 * rating ? Severity::Error : Severity::Warning; }
}  // namespace

std::vector<RuleViolation> validateCircuit(const Schematic& sch, const PartRatings& r) {
    std::vector<RuleViolation> out;
    auto add = [&](Severity s, const std::string& code, const std::string& msg, const Component& c) {
        RuleViolation v;
        v.severity = s;
        v.code = code;
        v.message = msg;
        static const char* ratingCodes[] = {"VAL_RESISTOR_POWER", "VAL_LED_CURRENT", "VAL_DIODE_CURRENT",
                                             "VAL_TRANSISTOR_RATING", "VAL_TRANSIENT_STRESS"};
        if (!r.derating.empty())
            for (const char* rc : ratingCodes)
                if (code == rc) v.message += " Limits include " + r.derating + ".";
        v.components = {c.id};
        v.location = c.position;
        v.hasLocation = true;
        out.push_back(std::move(v));
    };
    const auto& nets = sch.nets();
    const int gnd = sch.groundNet();

    // --- Standard (E-series) values.
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::Resistor && c.kind != ComponentKind::Capacitor && c.kind != ComponentKind::Inductor)
            continue;
        auto value = parseEngineeringValue(primaryValue(c.value));
        if (!value || *value <= 0) continue;  // invalid values are an ERC error already
        const char* unit = c.kind == ComponentKind::Resistor ? "Ω" : (c.kind == ComponentKind::Capacitor ? "F" : "H");
        if (c.kind == ComponentKind::Resistor) {
            if (!isStandardValue(*value, ESeries::E24) && !isStandardValue(*value, ESeries::E96))
                add(Severity::Info, "VAL_NONSTANDARD_VALUE",
                    c.ref + " = " + fmt(*value, unit) + " is not a standard E24/E96 resistor value (nearest E24: " +
                        fmt(nearestStandardValue(*value, ESeries::E24), unit) + ").", c);
        } else if (!isStandardValue(*value, ESeries::E12) && !isStandardValue(*value, ESeries::E24)) {
            add(Severity::Info, "VAL_NONSTANDARD_VALUE",
                c.ref + " = " + fmt(*value, unit) + " is not a standard E12 value (nearest: " +
                    fmt(nearestStandardValue(*value, ESeries::E12), unit) + ").", c);
        }
    }

    // --- Decoupling: every IC power pin on a non-ground rail should see a capacitor to ground.
    if (gnd >= 0) {
        for (const auto& c : sch.components()) {
            if (c.kind != ComponentKind::Custom) continue;
            const auto& pins = c.def().pins;
            std::set<int> reported;
            for (size_t i = 0; i < pins.size(); ++i) {
                if (static_cast<PinType>(pins[i].type) != PinType::PowerIn) continue;
                int net = sch.netOf({c.id, static_cast<int>(i)});
                if (net < 0 || net == gnd || reported.count(net)) continue;
                if (nets[static_cast<size_t>(net)].pins.size() < 2) continue;  // unconnected: ERC reports it
                bool decoupled = false;
                for (const auto& pr : nets[static_cast<size_t>(net)].pins) {
                    const Component* cap = sch.find(pr.component);
                    if (!cap || cap->kind != ComponentKind::Capacitor) continue;
                    int other = sch.netOf({cap->id, pr.pin == 0 ? 1 : 0});
                    if (other == gnd) decoupled = true;
                }
                reported.insert(net);
                if (!decoupled)
                    add(Severity::Warning, "VAL_NO_DECOUPLING",
                        c.ref + "." + pins[i].name + " (" + nets[static_cast<size_t>(net)].name +
                            ") has no decoupling capacitor to GND — add 100 nF close to the pin.", c);
            }
        }
    }

    // --- Ratings from the DC operating point.
    bool hasSource = false;
    for (const auto& c : sch.components())
        hasSource |= c.kind == ComponentKind::VoltageSource || c.kind == ComponentKind::CurrentSource;
    if (gnd < 0 || !hasSource) return out;

    Simulator sim(sch);
    DcResult dc = sim.dcOperatingPoint();
    if (!dc.converged) {
        RuleViolation v;
        v.severity = Severity::Warning;
        v.code = "VAL_DC_FAILED";
        v.message = "Ratings were not checked: the DC operating point did not converge (" + dc.error + ").";
        out.push_back(v);
        return out;
    }
    // Continuous ratings of a semiconductor: its part-number model if known, else the default part of its kind.
    struct Limits {
        double current, power;
        std::string part;
    };
    auto limitsFor = [&](const Component& c) -> Limits {
        if (const DeviceModel* m = findDeviceModel(c.kind, c.value))
            return {m->maxCurrent * r.currentFactor, m->maxPower * r.powerFactor, m->part};
        switch (c.kind) {
            case ComponentKind::Diode: return {r.diodeCurrent, 1e9, "1N4148"};
            case ComponentKind::NPN: return {r.npnCurrent, r.npnPower, "BC847"};
            case ComponentKind::NMOS: return {r.nmosCurrent, r.nmosPower, "2N7002"};
            default: return {1e9, 1e9, ""};
        }
    };
    std::map<int, std::set<std::string>> reported;  // component id → codes already raised
    auto addOnce = [&](Severity s, const std::string& code, const std::string& msg, const Component& c) {
        if (reported[c.id].insert(code).second) add(s, code, msg, c);
    };

    for (const auto& d : dc.devices) {
        const Component* c = sch.find(d.componentId);
        if (!c) continue;
        double i = std::fabs(d.current), p = std::fabs(d.power);
        switch (c->kind) {
            case ComponentKind::Resistor: {
                auto rated = powerRating(c->value);
                double limit = rated ? *rated * r.powerFactor : r.resistorPower;
                if (p > limit)
                    addOnce(overload(p, limit), "VAL_RESISTOR_POWER",
                            c->ref + " dissipates " + fmt(p, "W") + ", above the " + fmt(limit, "W") +
                                (rated ? " rating given in its value." : " rating of an 0805 resistor. Use a larger package "
                                                                        "(add the rating to the value, e.g. \"120 1W\") or a higher value."),
                            *c);
                break;
            }
            case ComponentKind::LED:
                if (d.voltage < -0.5)
                    addOnce(Severity::Warning, "VAL_REVERSE_BIAS",
                            c->ref + " is reverse-biased (V_AK = " + fmt(d.voltage, "V") + ") — check its orientation.", *c);
                else if (i > r.ledCurrent)
                    addOnce(overload(i, r.ledCurrent), "VAL_LED_CURRENT",
                            c->ref + " carries " + fmt(i, "A") + " (max " + fmt(r.ledCurrent, "A") +
                                "). Increase the series resistor.", *c);
                break;
            case ComponentKind::Diode: {
                Limits lim = limitsFor(*c);
                if (i > lim.current)
                    addOnce(overload(i, lim.current), "VAL_DIODE_CURRENT",
                            c->ref + " forward current " + fmt(i, "A") + " exceeds " + fmt(lim.current, "A") + " (" +
                                lim.part + ").", *c);
                break;
            }
            case ComponentKind::NPN:
            case ComponentKind::NMOS: {
                Limits lim = limitsFor(*c);
                if (i > lim.current || p > lim.power)
                    addOnce(std::max(overload(i, lim.current), overload(p, lim.power)), "VAL_TRANSISTOR_RATING",
                            c->ref + (c->kind == ComponentKind::NPN ? " I_C = " : " I_D = ") + fmt(i, "A") + ", P = " +
                                fmt(p, "W") + " exceeds " + lim.part + " ratings (" + fmt(lim.current, "A") + ", " +
                                fmt(lim.power, "W") + ").", *c);
                break;
            }
            case ComponentKind::VoltageSource:
                if (i > r.supplyCurrent)
                    addOnce(Severity::Warning, "VAL_SUPPLY_CURRENT",
                            c->ref + " delivers " + fmt(i, "A") + " — check for a short circuit or a missing load resistor.", *c);
                break;
            case ComponentKind::OpAmp:
                if (std::fabs(d.voltage) > r.opampRail)
                    addOnce(Severity::Info, "VAL_OPAMP_SATURATED",
                            c->ref + " output is saturated at " + fmt(d.voltage, "V") + " — check the gain and feedback.", *c);
                break;
            case ComponentKind::Fuse: {
                auto rating = parseEngineeringValue(c->value);
                if (rating && *rating > 0 && i > *rating)
                    addOnce(overload(i, *rating), "VAL_FUSE_OVERLOAD",
                            c->ref + " carries " + fmt(i, "A") + ", above its " + fmt(*rating, "A") + " rating.", *c);
                break;
            }
            default: break;
        }
    }

    // --- Switching stress: with SIN/PULSE stimulus, the DC point (t = 0) misses the real operating currents.
    // Simulate until steady (50–800 periods of the slowest source) and check RMS current / average power over the
    // last quarter against continuous ratings, and peak current against 4× the continuous rating.
    if (!r.transientStress) return out;
    double slowest = 0, fastest = 1e30;
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::VoltageSource && c.kind != ComponentKind::CurrentSource) continue;
        auto spec = SourceSpec::parse(c.value);
        if (!spec) continue;
        double period = 0;
        if (spec->kind == SourceSpec::Kind::Sine && spec->frequency > 0) period = 1.0 / spec->frequency;
        if (spec->kind == SourceSpec::Kind::Pulse && spec->period > 0) period = spec->period;
        if (period > 0) {
            slowest = std::max(slowest, period);
            fastest = std::min(fastest, period);
        }
    }
    if (slowest <= 0) return out;
    // Extend the window until the circuit has settled: RMS currents of the last two quarters agree within 5 %.
    TransientResult tr;
    for (double periods = 50; periods <= 800; periods *= 2) {
        double window = std::min(1.0, periods * slowest);
        double step = std::max(fastest / 50, window / 40000);
        tr = Simulator(sch).transient(window, step);
        if (!tr.ok) break;
        size_t n = tr.time.size(), q3 = n / 2, q4 = 3 * n / 4;
        bool settled = true;
        for (const auto& [id, samples] : tr.currents) {
            double a = 0, b = 0;
            for (size_t k = q3; k < q4; ++k) a += samples[k] * samples[k];
            for (size_t k = q4; k < n; ++k) b += samples[k] * samples[k];
            a = std::sqrt(a / std::max<size_t>(1, q4 - q3));
            b = std::sqrt(b / std::max<size_t>(1, n - q4));
            if (std::fabs(a - b) > 0.05 * std::max(a, b) + 1e-6) settled = false;
        }
        for (const auto& samples : tr.netVoltages) {  // slow rails (output capacitors) must have stopped moving too
            double a = 0, b = 0;
            for (size_t k = q3; k < q4; ++k) a += samples[k];
            for (size_t k = q4; k < n; ++k) b += samples[k];
            a /= static_cast<double>(std::max<size_t>(1, q4 - q3));
            b /= static_cast<double>(std::max<size_t>(1, n - q4));
            if (std::fabs(a - b) > 0.02 * std::max(std::fabs(a), std::fabs(b)) + 0.01) settled = false;
        }
        if (settled || window >= 1.0) break;
    }
    if (!tr.ok) {
        RuleViolation v;
        v.severity = Severity::Info;
        v.code = "VAL_TRANSIENT_SKIPPED";
        v.message = "Switching stress was not checked: " + tr.error;
        out.push_back(v);
        return out;
    }
    size_t n = tr.time.size(), from = 3 * n / 4;  // steady state: last quarter
    if (n < 4) return out;
    for (const auto& c : sch.components()) {
        auto ic = tr.currents.find(c.id);
        auto pc = tr.powers.find(c.id);
        if (ic == tr.currents.end() || pc == tr.powers.end()) continue;
        double sq = 0, pavg = 0, peak = 0;
        for (size_t k = from; k < n; ++k) {
            sq += ic->second[k] * ic->second[k];
            pavg += std::fabs(pc->second[k]);
            peak = std::max(peak, std::fabs(ic->second[k]));
        }
        double cnt = static_cast<double>(n - from);
        double rms = std::sqrt(sq / cnt);
        pavg /= cnt;
        double imax = 0, pmax = 0;
        std::string part;
        switch (c.kind) {
            case ComponentKind::Resistor: {
                auto rated = powerRating(c.value);
                pmax = rated ? *rated * r.powerFactor : r.resistorPower;
                part = rated ? fmt(*rated, "W") + " resistor" : "0805 resistor";
                break;
            }
            case ComponentKind::LED: imax = r.ledCurrent; part = "LED"; break;
            case ComponentKind::Diode:
            case ComponentKind::NPN:
            case ComponentKind::NMOS: {
                Limits lim = limitsFor(c);
                imax = lim.current;
                pmax = lim.power;
                part = lim.part;
                break;
            }
            default: continue;
        }
        if (reported[c.id].count("VAL_TRANSISTOR_RATING") || reported[c.id].count("VAL_RESISTOR_POWER") ||
            reported[c.id].count("VAL_LED_CURRENT") || reported[c.id].count("VAL_DIODE_CURRENT"))
            continue;
        bool overI = imax > 0 && rms > imax;
        bool overP = pmax > 0 && pavg > pmax;
        bool overPeak = imax > 0 && peak > 4 * imax;
        if (!overI && !overP && !overPeak) continue;
        Severity sev = Severity::Warning;
        if ((imax > 0 && rms >= 2 * imax) || (pmax > 0 && pavg >= 2 * pmax) || overPeak) sev = Severity::Error;
        std::string msg = c.ref + " switching stress: RMS " + fmt(rms, "A") + ", peak " + fmt(peak, "A") + ", average " +
                          fmt(pavg, "W") + " — exceeds " + part + " ratings (" +
                          (imax > 0 ? fmt(imax, "A") + " continuous, " + fmt(4 * imax, "A") + " peak" : std::string()) +
                          (imax > 0 && pmax > 0 ? ", " : "") + (pmax > 0 ? fmt(pmax, "W") : std::string()) + ").";
        addOnce(sev, "VAL_TRANSIENT_STRESS", msg, c);
    }
    return out;
}

}  // namespace sieda
