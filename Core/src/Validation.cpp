#include "sieda/Validation.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "sieda/CustomParts.hpp"
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
        auto value = parseEngineeringValue(c.value);
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
    for (const auto& d : dc.devices) {
        const Component* c = sch.find(d.componentId);
        if (!c) continue;
        double i = std::fabs(d.current), p = std::fabs(d.power);
        switch (c->kind) {
            case ComponentKind::Resistor:
                if (p > r.resistorPower)
                    add(overload(p, r.resistorPower), "VAL_RESISTOR_POWER",
                        c->ref + " dissipates " + fmt(p, "W") + ", above the " + fmt(r.resistorPower, "W") +
                            " rating of an 0805 resistor. Use a larger package or a higher value.", *c);
                break;
            case ComponentKind::LED:
                if (d.voltage < -0.5)
                    add(Severity::Warning, "VAL_REVERSE_BIAS",
                        c->ref + " is reverse-biased (V_AK = " + fmt(d.voltage, "V") + ") — check its orientation.", *c);
                else if (i > r.ledCurrent)
                    add(overload(i, r.ledCurrent), "VAL_LED_CURRENT",
                        c->ref + " carries " + fmt(i, "A") + " (max " + fmt(r.ledCurrent, "A") +
                            "). Increase the series resistor.", *c);
                break;
            case ComponentKind::Diode:
                if (i > r.diodeCurrent)
                    add(overload(i, r.diodeCurrent), "VAL_DIODE_CURRENT",
                        c->ref + " forward current " + fmt(i, "A") + " exceeds " + fmt(r.diodeCurrent, "A") + ".", *c);
                break;
            case ComponentKind::NPN:
                if (i > r.npnCurrent || p > r.npnPower)
                    add(std::max(overload(i, r.npnCurrent), overload(p, r.npnPower)), "VAL_TRANSISTOR_RATING",
                        c->ref + " I_C = " + fmt(i, "A") + ", P = " + fmt(p, "W") + " exceeds BC847 ratings (" +
                            fmt(r.npnCurrent, "A") + ", " + fmt(r.npnPower, "W") + ").", *c);
                break;
            case ComponentKind::NMOS:
                if (i > r.nmosCurrent || p > r.nmosPower)
                    add(std::max(overload(i, r.nmosCurrent), overload(p, r.nmosPower)), "VAL_TRANSISTOR_RATING",
                        c->ref + " I_D = " + fmt(i, "A") + ", P = " + fmt(p, "W") + " exceeds 2N7002 ratings (" +
                            fmt(r.nmosCurrent, "A") + ", " + fmt(r.nmosPower, "W") + ").", *c);
                break;
            case ComponentKind::VoltageSource:
                if (i > r.supplyCurrent)
                    add(Severity::Warning, "VAL_SUPPLY_CURRENT",
                        c->ref + " delivers " + fmt(i, "A") + " — check for a short circuit or a missing load resistor.", *c);
                break;
            case ComponentKind::OpAmp:
                if (std::fabs(d.voltage) > r.opampRail)
                    add(Severity::Info, "VAL_OPAMP_SATURATED",
                        c->ref + " output is saturated at " + fmt(d.voltage, "V") + " — check the gain and feedback.", *c);
                break;
            case ComponentKind::Fuse: {
                auto rating = parseEngineeringValue(c->value);
                if (rating && *rating > 0 && i > *rating)
                    add(overload(i, *rating), "VAL_FUSE_OVERLOAD",
                        c->ref + " carries " + fmt(i, "A") + ", above its " + fmt(*rating, "A") + " rating.", *c);
                break;
            }
            default: break;
        }
    }
    return out;
}

}  // namespace sieda
