#include "sieda/Validation.hpp"

#include <algorithm>
#include <cctype>
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
            // A capacitor to GND or to the part's own other supply / return pin (isolated domains, a module's input
            // and its return) decouples the pin.
            std::set<int> powerNets;
            for (size_t i = 0; i < pins.size(); ++i) {
                const auto t = static_cast<PinType>(pins[i].type);
                if (t == PinType::PowerIn || t == PinType::PowerOut)
                    if (int n = sch.netOf({c.id, static_cast<int>(i)}); n >= 0) powerNets.insert(n);
            }
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
                    if (other == gnd || (other != net && powerNets.count(other))) decoupled = true;
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
        hasSource |= isSourceKind(c.kind);
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
            case ComponentKind::Battery:
            case ComponentKind::ACSource:
                if (i > r.supplyCurrent)
                    addOnce(Severity::Warning, "VAL_SUPPLY_CURRENT",
                            c->ref + " delivers " + fmt(i, "A") + " — check for a short circuit or a missing load resistor.", *c);
                break;
            case ComponentKind::OpAmp:
                if (std::fabs(d.voltage) > r.opampRail)
                    addOnce(Severity::Info, "VAL_OPAMP_SATURATED",
                            c->ref + " output is saturated at " + fmt(d.voltage, "V") + " — check the gain and feedback.", *c);
                break;
            case ComponentKind::Custom: {
                const CustomPart* part = CustomPartRegistry::instance().find(c->customPart);
                if (!part || !part->spec.model.hasRegulator || d.subIndex != 0) break;
                const RegulatorModel& rm = part->spec.model.regulator;
                double pmax = rm.maxPower * r.powerFactor;
                if (d.power > pmax)
                    addOnce(overload(d.power, pmax), "VAL_REGULATOR_POWER",
                            c->ref + " (" + part->spec.name + ") dissipates " + fmt(d.power, "W") + " ((V_in − V_out) × " +
                                fmt(d.current, "A") + "), above its " + fmt(pmax, "W") +
                                " package limit — lower the input voltage, reduce the load or use a switching regulator.",
                            *c);
                if (d.state == 1 && !rm.charger)
                    addOnce(Severity::Warning, "VAL_REGULATOR_OVERLOAD",
                            c->ref + " (" + part->spec.name + ") is in current limit at " + fmt(rm.ilimit, "A") +
                                " — the load needs more than the regulator can supply.", *c);
                if (d.state == 3 && !rm.loadSwitch)
                    addOnce(Severity::Warning, "VAL_REGULATOR_DROPOUT",
                            c->ref + " (" + part->spec.name + ") is in dropout: only " + fmt(d.voltage, "V") +
                                " headroom for its " + fmt(rm.dropout, "V") + " dropout, so the output sags below " +
                                fmt(rm.vout, "V") + ".", *c);
                break;
            }
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
        if (!isSourceKind(c.kind)) continue;
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

std::vector<RuleViolation> designReview(const Schematic& sch) {
    std::vector<RuleViolation> out;
    auto add = [&](Severity s, const std::string& code, const std::string& msg, const Component& c) {
        RuleViolation v;
        v.severity = s;
        v.code = code;
        v.message = msg;
        v.components = {c.id};
        v.location = c.position;
        v.hasLocation = true;
        out.push_back(std::move(v));
    };
    const auto& nets = sch.nets();
    // --- An LED (or a series string of them) needs something that sets its current.
    std::set<int> seenLeds;
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::LED) continue;
        for (int p = 0; p < 2; ++p)  // an open end: it never lights (ERC names the pin; the review says what it means)
            if (const int n = sch.netOf({c.id, p}); !c.isNoConnect(p) && (n < 0 || nets[static_cast<size_t>(n)].pins.size() < 2))
                add(Severity::Warning, "VAL_LED_OPEN",
                    c.ref + "'s " + c.def().pins[static_cast<size_t>(p)].name + " pin joins nothing, so it never lights: wire it "
                            "(cathode to ground or its driver, anode to its resistor).", c);
        if (seenLeds.count(c.id)) continue;
        std::vector<const Component*> group{&c};
        seenLeds.insert(c.id);
        bool limited = false;
        for (size_t g = 0; g < group.size(); ++g)
            for (int p = 0; p < 2; ++p) {
                const int n = sch.netOf({group[g]->id, p});
                // In series means on the LED's own net: a resistor elsewhere on a supply rail does not count. A net of
                // just the LED and one other pin is its own whatever its name says (PWR_LED reads as a rail).
                if (n < 0) continue;
                const auto& pins = nets[static_cast<size_t>(n)].pins;
                if (sch.netRole(n) != NetRole::Signal) {
                    size_t parts = 0;  // the net's part pins: labels and power symbols only name it
                    for (const auto& pr : pins)
                        if (const Component* o = sch.find(pr.component); o && (o->kind == ComponentKind::PartUnit || !isNetSymbolKind(o->kind))) ++parts;
                    if (parts != 2) continue;
                }
                for (const auto& pr : pins) {
                    const Component* o = sch.find(pr.component);
                    if (!o || o == group[g]) continue;
                    if (o->kind == ComponentKind::Resistor || o->kind == ComponentKind::CurrentSource) limited = true;
                    else if (pins.size() == 2 && o->kind == ComponentKind::LED && seenLeds.insert(o->id).second)
                        group.push_back(o);
                }
            }
        if (!limited)
            add(Severity::Warning, "VAL_LED_NO_RESISTOR",
                c.ref + " has no series resistor: add one sized for its current (e.g. (V − Vf) / 5 mA), unless a "
                        "constant-current driver feeds it.", c);
    }

    // --- Reset / enable inputs must be tied to a level (pull-up, pull-down, a supply or a driver), not left open.
    static const std::set<std::string> controls = {"RST", "RESET", "NRST", "NRESET", "MCLR", "EN", "ENABLE", "CE",
                                                   "SHDN", "NSHDN", "CHIP_PU", "CHIP_EN", "PDN", "NPD", "SLEEP",
                                                   "NSLEEP", "STBY", "RUN", "BOOT0", "BOOT1", "BOOT", "MODE"};
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::Custom && c.kind != ComponentKind::PartUnit) continue;
        const auto& pins = c.def().pins;
        for (size_t i = 0; i < pins.size(); ++i) {
            const auto t = static_cast<PinType>(pins[i].type);
            if (t != PinType::Input && t != PinType::Bidirectional) continue;
            // Each function of a multi-function pin ("PC6/RST"), without its active-low marks ("~{RESET}", "!EN").
            bool control = false;
            std::string name;
            for (char ch : pins[i].name + "/") {
                if (ch == '/') {
                    control |= controls.count(name) > 0;
                    name.clear();
                } else if (ch != '~' && ch != '!' && ch != '{' && ch != '}') {
                    name += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
            }
            if (!control || c.isNoConnect(static_cast<int>(i))) continue;
            const int n = sch.netOf({c.id, static_cast<int>(i)});
            bool set = n >= 0 && sch.netRole(n) != NetRole::Signal;
            if (n >= 0)
                for (const auto& pr : nets[static_cast<size_t>(n)].pins) {
                    const Component* o = sch.find(pr.component);
                    if (!o || isNetSymbolKind(o->kind) || (o == &c && pr.pin == static_cast<int>(i))) continue;
                    // A resistor, a source or a driving pin sets the level; a switch, a connector or a capacitor
                    // (passive pins) leaves it open part of the time.
                    const auto ot = static_cast<PinType>(o->def().pins[static_cast<size_t>(pr.pin)].type);
                    if (o->kind == ComponentKind::Resistor || isSourceKind(o->kind) || ot == PinType::Output ||
                        ot == PinType::PowerOut || ot == PinType::Bidirectional || ot == PinType::OpenCollector)
                        set = true;
                }
            if (!set)
                add(Severity::Warning, "VAL_FLOATING_CONTROL",
                    c.ref + "." + pins[i].name + " is floating: tie it to its supply (or GND) through a resistor, or "
                                                   "drive it, so the part starts in a known state.", c);
        }
    }

    // ESP32-C3 strapping pins GPIO2 / GPIO8 (GPIO9 = BOOT is a control above) read at reset: left open, the boot mode
    // rests on the chip's weak defaults.
    for (const auto& c : sch.components()) {
        std::string part = c.def().name;
        for (auto& ch : part) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (part.rfind("ESP32-C3", 0) != 0 && part.rfind("ESP32_C3", 0) != 0) continue;
        const auto& pins = c.def().pins;
        for (size_t i = 0; i < pins.size(); ++i) {
            const std::string head = pins[i].name.substr(0, pins[i].name.find('/'));
            if (head != "GPIO2" && head != "IO2" && head != "GPIO8" && head != "IO8") continue;
            const int n = c.isNoConnect(static_cast<int>(i)) ? -1 : sch.netOf({c.id, static_cast<int>(i)});
            if (n < 0 || nets[static_cast<size_t>(n)].pins.size() < 2)
                add(Severity::Info, "VAL_STRAPPING_PIN",
                    c.ref + "." + pins[i].name + " is a strapping pin left open: its level at reset picks the boot mode; "
                                                   "tie it as the datasheet's strapping table says (often a 10k pull-up).", c);
        }
    }

    // A USB connector's data lines with no ESD / TVS part on them: a cable plugged in brings ESD straight to the chip.
    for (const auto& c : sch.components()) {
        std::string part = c.def().name;
        for (auto& ch : part) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (part.find("USB") == std::string::npos) continue;
        bool open = false;
        for (size_t i = 0; i < c.def().pins.size() && !open; ++i) {
            const std::string& name = c.def().pins[i].name;
            if (name != "D+" && name != "D-" && name != "DP" && name != "DM") continue;
            const int n = sch.netOf({c.id, static_cast<int>(i)});
            bool guarded = n < 0;
            for (const auto& pr : n < 0 ? std::vector<PinRef>{} : nets[static_cast<size_t>(n)].pins) {
                const Component* o = sch.find(pr.component);
                std::string on = o ? o->def().name + " " + o->value : "";
                for (auto& ch : on) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                for (const char* w : {"ESD", "TVS", "USBLC", "PRTR", "TPD", "SRV05", "PESD"}) guarded = guarded || on.find(w) != std::string::npos;
            }
            open = !guarded;
        }
        if (open)
            add(Severity::Info, "VAL_USB_ESD",
                c.ref + "'s USB data lines have no ESD protection: add a low-capacitance TVS array (e.g. USBLC6-2) by the connector.", c);
    }

    // Capacitors on `net` whose other pin is on ground (`highFrequency`: 1 µF or less, not bulk).
    auto capToGround = [&](int net, bool highFrequency = false) {
        int n = 0;
        for (const auto& pr : nets[static_cast<size_t>(net)].pins) {
            const Component* cap = sch.find(pr.component);
            const auto farads = cap ? parseEngineeringValue(primaryValue(cap->value)) : std::nullopt;
            if (highFrequency && farads && *farads > 1.01e-6) continue;
            if (cap && cap->kind == ComponentKind::Capacitor && cap->def().pins.size() == 2) {
                const int other = sch.netOf({cap->id, pr.pin == 0 ? 1 : 0});
                n += other >= 0 && sch.netRole(other) == NetRole::Ground;
            }
        }
        return n;
    };
    // --- One decoupling capacitor per supply pin: a rail feeding VCC and AVCC needs two, each at its pin.
    std::map<int, std::vector<std::string>> supplyPins;  // net → "U1.VCC" … (one per part and pin name)
    std::map<int, int> regulatorPins;  // net → a regulator's input pins on it: any capacitor serves those
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::Custom && c.kind != ComponentKind::PartUnit) continue;
        std::set<std::string> seen;
        const auto& pins = c.def().pins;
        const bool regulator = std::any_of(pins.begin(), pins.end(), [](const PinDef& p) { return static_cast<PinType>(p.type) == PinType::PowerOut; });
        for (size_t i = 0; i < pins.size(); ++i) {
            const int n = sch.netOf({c.id, static_cast<int>(i)});
            if (static_cast<PinType>(pins[i].type) == PinType::PowerIn && n >= 0 && sch.netRole(n) == NetRole::Power &&
                seen.insert(pins[i].name).second) {
                supplyPins[n].push_back(c.ref + "." + pins[i].name);
                regulatorPins[n] += regulator;
            }
        }
    }
    for (const auto& [n, list] : supplyPins) {
        // A bulk capacitor does not decouple a load's pin; a regulator's input takes the 1–10 µF its datasheet asks.
        const int loads = static_cast<int>(list.size()) - regulatorPins[n];
        const int caps = capToGround(n, true);
        if (capToGround(n) > 0 && (caps < loads || capToGround(n) < static_cast<int>(list.size()))) {
            std::string names;
            for (const auto& x : list) names += (names.empty() ? "" : ", ") + x;
            RuleViolation v;
            v.severity = Severity::Warning;
            v.code = "VAL_DECOUPLING_PER_PIN";
            v.message = nets[static_cast<size_t>(n)].name + " feeds " + std::to_string(list.size()) + " supply pins (" + names +
                        ") but has " + std::to_string(caps) + " decoupling capacitor" + (caps == 1 ? "" : "s") + " of 1 µF or less" +
                        ": one 100 nF per supply pin, each placed at its pin.";
            out.push_back(std::move(v));
        }
    }
    // --- A crystal needs a load capacitor to ground on each side.
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::Custom) continue;
        const CustomPart* cp = CustomPartRegistry::instance().find(c.customPart);
        std::string name = cp ? cp->spec.name : c.value;
        for (auto& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (name.find("CRYSTAL") == std::string::npos && name.rfind("XTAL", 0) != 0) continue;
        const auto& pins = c.def().pins;
        for (size_t i = 0; i < pins.size(); ++i) {
            const int n = sch.netOf({c.id, static_cast<int>(i)});
            if (n < 0 || sch.netRole(n) != NetRole::Signal || capToGround(n)) continue;
            add(Severity::Warning, "VAL_CRYSTAL_LOAD",
                c.ref + "." + pins[i].name + " (" + nets[static_cast<size_t>(n)].name + ") has no load capacitor to GND: "
                "a crystal needs one on each side, about 2 × (its load capacitance − 3 pF stray), e.g. 22 pF for 14 pF.", c);
        }
    }

    // --- An op-amp without output limits swings ±15 V: on lower supplies the simulation would not clip where the part
    // does (a built-in op-amp has no supply pins; its value sets the limits).
    double supply = 0;
    for (const auto& c : sch.components())
        if (isVoltageSourceKind(c.kind))
            if (auto v = parseEngineeringValue(primaryValue(c.value))) supply = std::max(supply, std::fabs(*v));
    if (supply > 0 && supply < 15)
        for (const auto& c : sch.components())
            if (c.kind == ComponentKind::OpAmp && c.value.find("VOH=") == std::string::npos) {
                char v[32];
                std::snprintf(v, sizeof v, "%g", supply);
                add(Severity::Warning, "VAL_OPAMP_RAILS",
                    c.ref + " swings ±15 V (its value sets no output limits) while the supply is " + v +
                        " V: give it the part's limits, e.g. value \"" + c.value + " VOH=" + v + " VOL=0\" (rail to "
                        "rail; an LM358 reaches about 1.5 V below its supply), so the simulation clips where the part does.", c);
            }
    return out;
}

}  // namespace sieda
