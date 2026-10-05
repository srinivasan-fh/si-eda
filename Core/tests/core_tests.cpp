// SiEDA core unit tests — dependency-free; run via `ctest` or directly.
#include <map>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <algorithm>

#include "sieda/Avr.hpp"
#include "sieda/CustomParts.hpp"
#include "sieda/DeviceModels.hpp"
#include "sieda/Bom.hpp"
#include "sieda/Embedded.hpp"
#include "sieda/Export.hpp"
#include "sieda/Fabrication.hpp"
#include "sieda/Firmware.hpp"
#include "sieda/Industry.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/Project.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/Reliability.hpp"
#include "sieda/Aerospace.hpp"
#include "sieda/Isolation.hpp"
#include "sieda/Medical.hpp"
#include "sieda/Retail.hpp"
#include "sieda/Appliance.hpp"
#include "sieda/Memory.hpp"
#include "sieda/Naval.hpp"
#include "sieda/Automotive.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/StandardParts.hpp"
#include "sieda/Validation.hpp"
#include "sieda/Verification.hpp"
#include "sieda/Units.hpp"
#include "sieda/sieda_c.h"

extern "C" int sieda_c_api_smoke_test(void);

using namespace sieda;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        ++g_checks;                                                                                    \
        double _a = (a), _b = (b);                                                                     \
        if (!(std::fabs(_a - _b) <= (tol))) {                                                          \
            ++g_failures;                                                                              \
            std::printf("    FAILED %s:%d: %s = %g, expected %g ± %g\n", __FILE__, __LINE__, #a, _a, _b, \
                        (double)(tol));                                                                \
        }                                                                                              \
    } while (0)

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
struct Register {
    Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
#define TEST(name)                                     \
    static void name();                                \
    static Register reg_##name(#name, name);           \
    static void name()

// Helpers ---------------------------------------------------------------
int pin(const Schematic& s, int comp, const char* name) { return s.pinIndex(comp, name); }
void wire(Schematic& s, int a, const char* pa, int b, const char* pb) {
    int id = s.connect({a, pin(s, a, pa)}, {b, pin(s, b, pb)});
    CHECK(id >= 0);
}
double netV(const Schematic& s, const DcResult& r, int comp, const char* p) {
    int n = s.netOf({comp, pin(s, comp, p)});
    return n < 0 ? NAN : r.netVoltages[static_cast<size_t>(n)];
}
const DeviceReading* reading(const DcResult& r, int comp) {
    for (const auto& d : r.devices)
        if (d.componentId == comp) return &d;
    return nullptr;
}

/// 5 V → 330 Ω → red LED → GND, the classic indicator circuit.
Project ledProject() {
    Project p;
    p.name = "LED indicator";
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "330", {80, -40});
    int d = s.addComponent(ComponentKind::LED, "Red", {160, -40});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", d, "A");
    wire(s, d, "K", g, "GND");
    wire(s, v, "-", g, "GND");
    p.schematicChanged();
    return p;
}
}  // namespace

// ======================================================================= units & json

TEST(units_parse) {
    CHECK_NEAR(*parseEngineeringValue("4.7k"), 4700, 1e-9);
    CHECK_NEAR(*parseEngineeringValue("4k7"), 4700, 1e-9);
    CHECK_NEAR(*parseEngineeringValue("2R2"), 2.2, 1e-12);
    CHECK_NEAR(*parseEngineeringValue("100nF"), 100e-9, 1e-18);
    CHECK_NEAR(*parseEngineeringValue("10uF"), 10e-6, 1e-15);
    CHECK_NEAR(*parseEngineeringValue("1meg"), 1e6, 1e-6);
    CHECK_NEAR(*parseEngineeringValue("1M"), 1e6, 1e-6);
    CHECK_NEAR(*parseEngineeringValue("1m"), 1e-3, 1e-12);
    CHECK_NEAR(*parseEngineeringValue("5V"), 5, 1e-12);
    CHECK_NEAR(*parseEngineeringValue("1e-6"), 1e-6, 1e-15);
    CHECK_NEAR(*parseEngineeringValue("-3.3"), -3.3, 1e-12);
    CHECK(!parseEngineeringValue("abc"));
    CHECK(!parseEngineeringValue(""));
    CHECK(formatEngineeringValue(4700, "Ω") == "4.7kΩ");
    CHECK(formatEngineeringValue(1e-7, "F") == "100nF");
}

TEST(source_specs) {
    auto dc = SourceSpec::parse("12V");
    CHECK(dc && dc->kind == SourceSpec::Kind::DC && dc->dc == 12);
    auto sine = SourceSpec::parse("SIN(0 1 1k)");
    CHECK(sine && sine->kind == SourceSpec::Kind::Sine);
    CHECK_NEAR(sine->valueAt(0.25e-3), 1.0, 1e-9);
    auto pulse = SourceSpec::parse("PULSE(0 5 1m)");
    CHECK(pulse && pulse->kind == SourceSpec::Kind::Pulse);
    CHECK_NEAR(pulse->valueAt(0), 0.0, 1e-12);
    CHECK_NEAR(pulse->valueAt(0.1e-3), 5.0, 1e-12);
    CHECK_NEAR(pulse->valueAt(0.7e-3), 0.0, 1e-12);
    CHECK(!SourceSpec::parse("SIN(0 1)"));
    CHECK(!SourceSpec::parse("banana"));
}

TEST(json_roundtrip) {
    Json j = Json::parse(R"({"a":[1,2.5,"x\né"],"b":{"c":true,"d":null},"e":-1e-3})");
    CHECK(j.get("a").size() == 3);
    CHECK(j.get("a")[2].asString() == "x\n\xc3\xa9");
    CHECK(j.get("b").get("c").asBool());
    CHECK(j.get("b").get("d").isNull());
    CHECK_NEAR(j.get("e").asNumber(), -1e-3, 1e-15);
    Json k = Json::parse(j.dump(true));
    CHECK(k.dump() == j.dump());
    bool threw = false;
    try {
        Json::parse("{\"a\":}");
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
    // Copy-on-write: mutating a copy must not affect the original.
    Json copy = j;
    copy["new"] = 1;
    CHECK(!j.has("new") && copy.has("new"));
}

// ======================================================================= schematic

TEST(schematic_connectivity) {
    Schematic s;
    int r1 = s.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int l1 = s.addComponent(ComponentKind::NetLabel, "VCC", {-50, 0});
    int l2 = s.addComponent(ComponentKind::NetLabel, "VCC", {150, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 50});
    CHECK(s.find(r1)->ref == "R1" && s.find(r2)->ref == "R2");
    wire(s, l1, "N", r1, "1");
    wire(s, l2, "N", r2, "2");
    wire(s, r1, "2", g, "GND");
    CHECK(s.netOf({r1, 0}) == s.netOf({r2, 1}));  // merged through identically named labels
    CHECK(s.nets()[static_cast<size_t>(s.netOf({r1, 0}))].name == "VCC");
    CHECK(s.netOf({r1, 1}) == s.groundNet());
    CHECK(s.connect({r1, 0}, {l1, 0}) == -1);  // duplicate wire rejected
    CHECK(s.connect({r1, 0}, {r1, 5}) == -1);  // invalid pin rejected

    // Rotation: pin "2" at (+30,0) rotated 90° clockwise on screen lands below the origin.
    s.rotateComponent(r1, 90);
    Vec2 p = s.pinPosition({r1, 1});
    CHECK_NEAR(p.x, 0, 1e-9);
    CHECK_NEAR(p.y, 30, 1e-9);

    CHECK(s.removeComponent(r1));
    CHECK(s.wires().size() == 1);
}

TEST(erc_rules) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "abc", {100, 0});
    wire(s, v, "+", v, "-");
    auto has = [&](const std::string& code) {
        for (const auto& e : s.runERC())
            if (e.code == code) return true;
        return false;
    };
    CHECK(has("ERC_NO_GROUND"));
    CHECK(has("ERC_SHORTED_SOURCE"));
    CHECK(has("ERC_INVALID_VALUE"));
    CHECK(has("ERC_FLOATING_COMPONENT"));
    (void)r;

    Schematic t;
    int v2 = t.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int d = t.addComponent(ComponentKind::LED, "Red", {100, 0});
    int g = t.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(t, v2, "+", d, "A");
    wire(t, d, "K", g, "GND");
    wire(t, v2, "-", g, "GND");
    bool warned = false;
    for (const auto& e : t.runERC()) warned |= e.code == "ERC_NO_CURRENT_LIMIT";
    CHECK(warned);

    Project led = ledProject();
    int errors = 0;
    for (const auto& e : led.schematic.runERC()) errors += e.severity == Severity::Error;
    CHECK(errors == 0);
}

// ======================================================================= simulation

TEST(sim_voltage_divider) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    CHECK_NEAR(netV(s, r, r1, "2"), 5.0, 1e-6);
    CHECK_NEAR(reading(r, v)->current, 0.5e-3, 1e-9);
    CHECK_NEAR(reading(r, v)->power, -5e-3, 1e-9);
    CHECK_NEAR(reading(r, r1)->power, 2.5e-3, 1e-9);
}

TEST(sim_led_forward_voltage) {
    Project p = ledProject();
    Simulator sim(p.schematic);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    const Component* led = p.schematic.findByRef("D1");
    double vf = netV(p.schematic, r, led->id, "A");
    CHECK(vf > 1.7 && vf < 2.2);
    double i = reading(r, led->id)->current;
    CHECK(i > 8e-3 && i < 10.5e-3);
}

TEST(sim_rc_transient) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 5 20m)", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int c = s.addComponent(ComponentKind::Capacitor, "1u", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", c, "1");
    wire(s, c, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto res = sim.transient(5e-3, 5e-6);
    CHECK(res.ok);
    int net = s.netOf({c, 0});
    // After one time constant (1 ms) the capacitor should be at ~63 % of 5 V.
    size_t idx = 200;
    CHECK_NEAR(res.time[idx], 1e-3, 1e-9);
    CHECK_NEAR(res.netVoltages[static_cast<size_t>(net)][idx], 5 * (1 - std::exp(-1.0)), 0.03);
    CHECK_NEAR(res.netVoltages[static_cast<size_t>(net)].back(), 5 * (1 - std::exp(-5.0)), 0.03);
}

TEST(sim_rl_dc_and_transient) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 1 1)", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "100", {100, 0});
    int l = s.addComponent(ComponentKind::Inductor, "10m", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", l, "1");
    wire(s, l, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto res = sim.transient(1e-3, 1e-6);  // tau = L/R = 100 µs
    CHECK(res.ok);
    auto& il = res.currents[l];
    CHECK_NEAR(il[100], 0.01 * (1 - std::exp(-1.0)), 3e-4);
    CHECK_NEAR(il.back(), 0.01, 1e-4);
}

TEST(sim_npn_switch_saturates) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int rb = s.addComponent(ComponentKind::Resistor, "10k", {100, 0});
    int rc = s.addComponent(ComponentKind::Resistor, "1k", {200, -80});
    int q = s.addComponent(ComponentKind::NPN, "BC847", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", rb, "1");
    wire(s, rb, "2", q, "B");
    wire(s, v, "+", rc, "1");
    wire(s, rc, "2", q, "C");
    wire(s, q, "E", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    double vce = netV(s, r, q, "C");
    double vbe = netV(s, r, q, "B");
    CHECK(vce < 0.3);
    CHECK(vbe > 0.6 && vbe < 0.85);
    CHECK_NEAR(reading(r, q)->current, (5 - vce) / 1000, 1e-5);
}

TEST(sim_npn_active_region) {
    // Base current 10 µA, beta 200 → Ic ≈ 2 mA, collector load small enough to stay active.
    Schematic s;
    int vcc = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
    int ib = s.addComponent(ComponentKind::CurrentSource, "10u", {100, 0});
    int rc = s.addComponent(ComponentKind::Resistor, "1k", {200, -80});
    int q = s.addComponent(ComponentKind::NPN, "BC847", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, ib, "+", q, "B");
    wire(s, ib, "-", g, "GND");
    wire(s, vcc, "+", rc, "1");
    wire(s, rc, "2", q, "C");
    wire(s, q, "E", g, "GND");
    wire(s, vcc, "-", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    CHECK_NEAR(reading(r, q)->current, 2e-3, 0.1e-3);
}

TEST(sim_opamp_noninverting) {
    Schematic s;
    int vin = s.addComponent(ComponentKind::VoltageSource, "1", {0, 0});
    int u = s.addComponent(ComponentKind::OpAmp, "LM358", {100, 0});
    int rf = s.addComponent(ComponentKind::Resistor, "10k", {150, -60});
    int rg = s.addComponent(ComponentKind::Resistor, "10k", {50, -60});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, vin, "+", u, "IN+");
    wire(s, vin, "-", g, "GND");
    wire(s, u, "OUT", rf, "2");
    wire(s, rf, "1", u, "IN-");
    wire(s, rg, "2", u, "IN-");
    wire(s, rg, "1", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    CHECK_NEAR(netV(s, r, u, "OUT"), 2.0, 1e-3);
}

TEST(sim_nmos_low_side_switch) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int rd = s.addComponent(ComponentKind::Resistor, "1k", {100, -80});
    int m = s.addComponent(ComponentKind::NMOS, "2N7002", {100, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", rd, "1");
    wire(s, rd, "2", m, "D");
    wire(s, v, "+", m, "G");
    wire(s, m, "S", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    CHECK(netV(s, r, m, "D") < 0.1);
}

TEST(sim_blue_white_leds_conduct) {
    // Wide-gap LEDs (blue/white, Is ≈ 1e-26) need junction exponents near 60; a fixed exp() knee at 40 left them open.
    for (const char* colour : {"Blue", "White", "Red", "Green"}) {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "100", {80, 0});
        int d = s.addComponent(ComponentKind::LED, colour, {160, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", r, "1");
        wire(s, r, "2", d, "A");
        wire(s, d, "K", g, "GND");
        wire(s, v, "-", g, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        double i = 0, vf = 0;
        for (const auto& dev : dc.devices)
            if (dev.componentId == d) {
                i = dev.current;
                vf = dev.voltage;
            }
        bool wide = std::string(colour) == "Blue" || std::string(colour) == "White";
        std::printf("    %-5s Vf %.3f V, I %.2f mA\n", colour, vf, i * 1000);
        CHECK(i > 0.010 && i < 0.035);
        CHECK(wide ? (vf > 2.6 && vf < 3.3) : (vf > 1.7 && vf < 2.2));
    }
}

TEST(device_models_and_switching_stress) {
    CHECK(findDeviceModel(ComponentKind::NMOS, "SI2302") != nullptr);
    CHECK(findDeviceModel(ComponentKind::NMOS, "si2302cds-t1") != nullptr);
    CHECK(findDeviceModel(ComponentKind::NMOS, "BC847") == nullptr);  // wrong kind
    CHECK(findDeviceModel(ComponentKind::Diode, "SS14")->maxCurrent == 1.0);
    CHECK_NEAR(primaryValue("120 2W") == "120" ? 1 : 0, 1, 0);
    CHECK_NEAR(*powerRating("120 2W"), 2.0, 1e-12);
    CHECK_NEAR(*powerRating("0R1 1/2W"), 0.5, 1e-12);
    CHECK_NEAR(*powerRating("4k7/250mW"), 0.25, 1e-12);
    CHECK(primaryValue("4k7/250mW") == "4k7");
    CHECK(!powerRating("4k7"));

    // Coreless motor channel: 3.7 V LiPo, 1.2 Ω + 50 µH winding, 20 kHz PWM, Schottky flyback.
    auto motor = [](const char* fet, const char* winding) {
        Schematic s;
        int b = s.addComponent(ComponentKind::VoltageSource, "3.7", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int r = s.addComponent(ComponentKind::Resistor, winding, {80, 0});
        int l = s.addComponent(ComponentKind::Inductor, "50u", {160, 0});
        int q = s.addComponent(ComponentKind::NMOS, fet, {240, 0});
        int d = s.addComponent(ComponentKind::Diode, "SS14", {240, -80});
        int vg = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 3.3 50u 0.5)", {0, 160});
        int rg = s.addComponent(ComponentKind::Resistor, "100", {120, 160});
        wire(s, b, "+", r, "1");
        wire(s, r, "2", l, "1");
        wire(s, l, "2", q, "D");
        wire(s, q, "S", g, "GND");
        wire(s, b, "-", g, "GND");
        wire(s, d, "A", q, "D");
        wire(s, d, "K", b, "+");
        wire(s, vg, "+", rg, "1");
        wire(s, rg, "2", q, "G");
        wire(s, vg, "-", g, "GND");
        return s;
    };
    auto peakDrain = [&](const char* fet) {
        Schematic s = motor(fet, "1.2 5W");
        TransientResult tr = Simulator(s).transient(1e-3, 0.5e-6);
        double peak = 0;
        for (const auto& c : s.components())
            if (c.kind == ComponentKind::NMOS)
                for (double i : tr.currents[c.id]) peak = std::max(peak, std::fabs(i));
        return peak;
    };
    double si = peakDrain("SI2302"), generic = peakDrain("2N7002");
    std::printf("    peak drain current: SI2302 %.2f A, 2N7002 %.2f A\n", si, generic);
    CHECK(si > 1.0 && si < 3.5);  // a real logic-level FET drives the coreless motor
    CHECK(generic < 0.3);         // the small-signal part saturates

    auto codes = [](const std::vector<RuleViolation>& v, const char* code) {
        int n = 0;
        for (const auto& f : v) n += f.code == code;
        return n;
    };
    // 2N7002 in a motor drive: DC (t = 0, gate low) sees nothing, the switching-stress check catches it.
    Schematic weak = motor("2N7002", "1.2 5W");
    CHECK(codes(validateCircuit(weak), "VAL_TRANSIENT_STRESS") >= 1);
    // SI2302 with a power-rated winding model: within ratings.
    Schematic good = motor("SI2302", "1.2 5W");
    auto findings = validateCircuit(good);
    for (const auto& f : findings)
        if (f.severity != Severity::Info) std::printf("    unexpected: %s %s\n", f.code.c_str(), f.message.c_str());
    CHECK(codes(findings, "VAL_TRANSIENT_STRESS") == 0);
    // The same winding as a plain 0805 resistor is flagged (2.3 W average).
    CHECK(codes(validateCircuit(motor("SI2302", "1.2")), "VAL_TRANSIENT_STRESS") >= 1);
}

TEST(behavioural_chip_models) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    auto net = [](const Schematic& s, const DcResult& dc, int comp, const char* pin) {
        return dc.netVoltages[static_cast<size_t>(s.netOf({comp, s.pinIndex(comp, pin)}))];
    };
    auto codes = [](const std::vector<RuleViolation>& v, const char* code) {
        int n = 0;
        for (const auto& f : v) n += f.code == code;
        return n;
    };
    // LDO from a 1S LiPo: regulates, then drops out as the battery sags.
    auto ldo = [&](const char* vin, const char* load) {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, vin, {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int u = s.addCustomComponent(partId("XC6206P332"), "", {100, 0});
        int r = s.addComponent(ComponentKind::Resistor, load, {200, 0});
        wire(s, v, "+", u, "VIN");
        wire(s, u, "VSS", g, "GND");
        wire(s, u, "VOUT", r, "1");
        wire(s, r, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        return std::make_pair(s, u);
    };
    {
        auto [s, u] = ldo("3.7", "150");  // 22 mA
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        CHECK_NEAR(net(s, dc, u, "VOUT"), 3.3, 0.01);
        CHECK(codes(validateCircuit(s), "VAL_REGULATOR_DROPOUT") == 0);
    }
    {
        auto [s, u] = ldo("3.4", "150");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(net(s, dc, u, "VOUT") < 3.2 && net(s, dc, u, "VOUT") > 3.05);
        CHECK(codes(validateCircuit(s), "VAL_REGULATOR_DROPOUT") == 1);
    }
    {
        auto [s, u] = ldo("3.7", "5");  // would need 660 mA from a 250 mA LDO
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        CHECK(net(s, dc, u, "VOUT") < 1.4);  // 250 mA × 5 Ω
        CHECK(codes(validateCircuit(s), "VAL_REGULATOR_OVERLOAD") == 1);
    }
    {
        // Output back-fed from a higher rail: the regulator must not sink.
        auto [s, u] = ldo("3.7", "1k");
        int v2 = s.addComponent(ComponentKind::VoltageSource, "5", {300, 0});
        int g2 = s.addComponent(ComponentKind::Ground, "", {300, 80});
        wire(s, v2, "+", u, "VOUT");
        wire(s, v2, "-", g2, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        for (const auto& d : dc.devices)
            if (d.componentId == u && d.subIndex == 0) CHECK(std::fabs(d.current) < 1e-6);
    }
    // LM317 adjustable: 1.25 V × (1 + 715/240) + I_ADJ·R2 ≈ 5.01 V; a 7805 dissipating 7 W in TO-220 free air is an error.
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "12", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int u = s.addCustomComponent(partId("LM317"), "", {100, 0});
        int r1 = s.addComponent(ComponentKind::Resistor, "240", {200, 0});
        int r2 = s.addComponent(ComponentKind::Resistor, "715", {200, 80});
        wire(s, v, "+", u, "IN");
        wire(s, u, "OUT", r1, "1");
        wire(s, r1, "2", u, "ADJ");
        wire(s, u, "ADJ", r2, "1");
        wire(s, r2, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK_NEAR(net(s, dc, u, "OUT"), 1.25 * (1 + 715.0 / 240.0) + 50e-6 * 715, 0.01);  // includes I_ADJ · R2
    }
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "12", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int u = s.addCustomComponent(partId("LM7805"), "", {100, 0});
        int r = s.addComponent(ComponentKind::Resistor, "5 10W", {200, 0});
        wire(s, v, "+", u, "IN");
        wire(s, u, "GND", g, "GND");
        wire(s, u, "OUT", r, "1");
        wire(s, r, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        auto f = validateCircuit(s);
        CHECK(codes(f, "VAL_REGULATOR_POWER") == 1);
        bool error = false;
        for (const auto& x : f) error |= x.code == "VAL_REGULATOR_POWER" && x.severity == Severity::Error;
        CHECK(error);
    }
    // TP4056 charging a 3.7 V cell from USB: constant current 1 A, no overload finding (CC is normal for a charger).
    {
        Schematic s;
        int usb = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
        int bat = s.addComponent(ComponentKind::VoltageSource, "3.7", {300, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int g2 = s.addComponent(ComponentKind::Ground, "", {300, 80});
        int u = s.addCustomComponent(partId("TP4056"), "", {150, 0});
        wire(s, usb, "+", u, "VCC");
        wire(s, u, "GND", g, "GND");
        wire(s, u, "BAT", bat, "+");
        wire(s, usb, "-", g, "GND");
        wire(s, bat, "-", g2, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        for (const auto& d : dc.devices)
            if (d.componentId == u && d.subIndex == 0) {
                CHECK_NEAR(d.current, 1.0, 1e-3);
                CHECK(d.state == 1);
            }
        CHECK(codes(validateCircuit(s), "VAL_REGULATOR_OVERLOAD") == 0);
    }
    // IC supply current: an ATmega328P draws its operating current from the rail.
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "3.3", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int u = s.addCustomComponent(partId("ATmega328P"), "", {150, 0});
        wire(s, v, "+", u, "7");
        wire(s, u, "8", g, "GND");
        wire(s, v, "-", g, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        double supply = 0;
        for (const auto& d : dc.devices)
            if (d.componentId == v) supply = d.current;
        CHECK_NEAR(supply, 0.004, 2e-4);
    }
    // 2×4 header footprint: two columns 2.54 mm apart, IDC numbering.
    {
        auto part = CustomPartRegistry::instance().get(partId("NRF24L01_Module"));
        CHECK(part->footprint.pads.size() == 8);
        CHECK_NEAR(part->footprint.pads[1].offset.x - part->footprint.pads[0].offset.x, 2.54, 1e-9);
        CHECK_NEAR(part->footprint.pads[2].offset.y - part->footprint.pads[0].offset.y, 2.54, 1e-9);
    }
}

TEST(sim_no_ground_reports_error) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    wire(s, v, "+", r, "1");
    wire(s, v, "-", r, "2");
    Simulator sim(s);
    auto res = sim.dcOperatingPoint();
    CHECK(!res.converged);
    CHECK(!res.error.empty());
}

// ======================================================================= PCB

namespace {
int drcErrors(const Project& p, std::string* first = nullptr) {
    int n = 0;
    for (const auto& v : p.pcb.runDRC(p.schematic))
        if (v.severity == Severity::Error) {
            if (first && n == 0) *first = v.code + ": " + v.message;
            ++n;
        }
    return n;
}

Project amplifierProject() {
    // Non-inverting amplifier + LED driver + decoupling — a denser board.
    Project p;
    auto& s = p.schematic;
    int j = s.addComponent(ComponentKind::Connector, "PWR", {0, 0});
    int vcc = s.addComponent(ComponentKind::NetLabel, "VCC", {-40, -10});
    int gnd = s.addComponent(ComponentKind::Ground, "", {-40, 30});
    wire(s, j, "1", vcc, "N");
    wire(s, j, "2", gnd, "GND");
    int u = s.addComponent(ComponentKind::OpAmp, "LM358", {200, 0});
    int rin = s.addComponent(ComponentKind::Resistor, "10k", {100, 10});
    int rb = s.addComponent(ComponentKind::Resistor, "10k", {100, 60});
    int rf = s.addComponent(ComponentKind::Resistor, "22k", {200, -60});
    int rg = s.addComponent(ComponentKind::Resistor, "10k", {120, -60});
    int c1 = s.addComponent(ComponentKind::Capacitor, "100n", {300, 60});
    int q = s.addComponent(ComponentKind::NPN, "BC847", {320, 0});
    int rl = s.addComponent(ComponentKind::Resistor, "330", {320, -80});
    int d = s.addComponent(ComponentKind::LED, "Green", {380, -80});
    int rbq = s.addComponent(ComponentKind::Resistor, "4k7", {270, 0});
    int l2 = s.addComponent(ComponentKind::NetLabel, "VCC", {80, 10});
    int l3 = s.addComponent(ComponentKind::NetLabel, "VCC", {300, -120});
    int g2 = s.addComponent(ComponentKind::Ground, "", {100, 100});
    int g3 = s.addComponent(ComponentKind::Ground, "", {320, 60});
    wire(s, l2, "N", rin, "1");
    wire(s, rin, "2", u, "IN+");
    wire(s, rb, "1", u, "IN+");
    wire(s, rb, "2", g2, "GND");
    wire(s, rg, "1", g2, "GND");
    wire(s, rg, "2", u, "IN-");
    wire(s, rf, "1", u, "IN-");
    wire(s, rf, "2", u, "OUT");
    wire(s, u, "OUT", rbq, "1");
    wire(s, rbq, "2", q, "B");
    wire(s, q, "E", g3, "GND");
    wire(s, l3, "N", rl, "1");
    wire(s, rl, "2", d, "A");
    wire(s, d, "K", q, "C");
    wire(s, c1, "1", l3, "N");
    wire(s, c1, "2", g3, "GND");
    p.schematicChanged();
    return p;
}
}  // namespace

TEST(pcb_place_and_route_led) {
    Project p = ledProject();
    p.pcb.settings.width = 30;
    p.pcb.settings.height = 20;
    p.pcb.autoPlace(p.schematic, true);
    for (const auto& c : p.schematic.components())
        if (c.hasFootprint()) CHECK(c.pcb.placed);
    CHECK(!p.pcb.ratsnest(p.schematic).empty());
    RouteStats st = p.pcb.autoRoute(p.schematic);
    CHECK(st.failed == 0);
    CHECK(st.connections == 3);
    CHECK(p.pcb.ratsnest(p.schematic).empty());
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
}

TEST(pcb_route_amplifier_drc_clean) {
    Project p = amplifierProject();
    p.pcb.settings.width = 40;
    p.pcb.settings.height = 30;
    p.pcb.autoPlace(p.schematic, true);
    RouteStats st = p.pcb.autoRoute(p.schematic);
    std::printf("    %d/%d connections, %d vias, %.1f mm\n", st.routed, st.connections, st.vias, st.trackLength);
    CHECK(st.failed == 0);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
}

TEST(pcb_fit_board_keeps_design_valid) {
    Project p = amplifierProject();
    p.pcb.settings.width = 80;
    p.pcb.settings.height = 60;
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    size_t tracks = p.pcb.tracks.size();
    CHECK(p.pcb.fitBoardToComponents(p.schematic, 2.0));
    CHECK(p.pcb.settings.width < 80);
    CHECK(p.pcb.settings.height < 60);
    CHECK(p.pcb.tracks.size() == tracks);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);  // shifted copper must still connect and keep clearance
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
    for (const auto& c : p.schematic.components())
        if (c.hasFootprint()) {
            Rect cy = p.pcb.courtyard(c);
            CHECK(cy.x0 >= 0 && cy.y0 >= 0 && cy.x1 <= p.pcb.settings.width && cy.y1 <= p.pcb.settings.height);
        }
}

TEST(pcb_drc_detects_short_and_unrouted) {
    Project p = ledProject();
    p.pcb.autoPlace(p.schematic, true);
    auto pads = p.pcb.pads(p.schematic);
    // Draw a track between two pads of different nets → short.
    Track t;
    t.layer = kTopLayer;
    t.width = 0.25;
    for (const auto& a : pads)
        for (const auto& b : pads)
            if (a.net != b.net && !a.throughHole && !b.throughHole && a.componentId == b.componentId) {
                t.a = a.position;
                t.b = b.position;
                t.net = a.net;
            }
    p.pcb.addTrack(t);
    bool shortFound = false, unrouted = false;
    for (const auto& v : p.pcb.runDRC(p.schematic)) {
        shortFound |= v.code == "DRC_SHORT";
        unrouted |= v.code == "DRC_UNROUTED";
    }
    CHECK(shortFound);
    CHECK(unrouted);
}

TEST(pcb_routing_survives_schematic_edit) {
    Project p = ledProject();
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    size_t before = p.pcb.tracks.size();
    CHECK(before > 0);
    // Adding an unrelated component shifts net indices; copper must be re-associated, not dropped.
    p.schematic.addComponent(ComponentKind::Resistor, "1k", {500, 500});
    p.schematicChanged();
    CHECK(p.pcb.tracks.size() == before);
    // Deleting every part leaves the copper without any pads: it is orphaned and must be pruned.
    std::vector<int> ids;
    for (const auto& c : p.schematic.components()) ids.push_back(c.id);
    for (int id : ids) p.schematic.removeComponent(id);
    p.schematicChanged();
    CHECK(p.pcb.tracks.empty());
    CHECK(p.pcb.vias.empty());
}

TEST(pcb_single_layer_routing) {
    Project p = ledProject();
    p.pcb.settings.layerCount = 1;
    p.pcb.settings.width = 30;
    p.pcb.settings.height = 20;
    p.pcb.autoPlace(p.schematic, true);
    RouteStats st = p.pcb.autoRoute(p.schematic);
    CHECK(st.failed == 0);
    CHECK(st.vias == 0);
    CHECK(p.pcb.vias.empty());
    for (const auto& t : p.pcb.tracks) CHECK(t.layer == 0);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
}

TEST(pcb_multi_layer_routing) {
    for (int layers : {4, 6}) {
        Project p = amplifierProject();
        p.pcb.settings.layerCount = layers;
        p.pcb.settings.width = 30;
        p.pcb.settings.height = 24;
        p.pcb.autoPlace(p.schematic, true);
        RouteStats st = p.pcb.autoRoute(p.schematic);
        std::printf("    %d layers: %d/%d routed, %d vias\n", layers, st.routed, st.connections, st.vias);
        CHECK(st.failed == 0);
        for (const auto& t : p.pcb.tracks) CHECK(t.layer >= 0 && t.layer < layers);
        std::string first;
        CHECK(drcErrors(p, &first) == 0);
        if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
        // Every layer exports as Gerber; inner layers are marked "Inr".
        std::string inner = exportCopperGerber(p.schematic, p.pcb, 1);
        CHECK(inner.find("Copper,L2,Inr") != std::string::npos);
        std::string bottom = exportGerber(p.schematic, p.pcb, GerberLayer::BottomCopper);
        CHECK(bottom.find("Copper,L" + std::to_string(layers) + ",Bot") != std::string::npos);
        Mesh layerMesh = buildCopperLayerMesh(p.schematic, p.pcb, 0);
        CHECK(layerMesh.vertexCount() > 0);
    }
}

TEST(pcb_dense_board_uses_inner_layers) {
    // A crowded grid of resistors with crossing connections: 4 layers should route at least as much as 2.
    auto build = [](int layers) {
        Project p;
        auto& s = p.schematic;
        std::vector<int> r;
        for (int i = 0; i < 12; ++i) r.push_back(s.addComponent(ComponentKind::Resistor, "1k", {i * 80.0, 0}));
        for (int i = 0; i < 12; ++i) s.connect({r[static_cast<size_t>(i)], 0}, {r[static_cast<size_t>((i * 5 + 3) % 12)], 1});
        p.pcb.settings.layerCount = layers;
        p.pcb.settings.width = 16;
        p.pcb.settings.height = 14;
        p.pcb.autoPlace(p.schematic, true);
        return p.pcb.autoRoute(p.schematic);
    };
    RouteStats two = build(2), four = build(4);
    std::printf("    2 layers %d/%d (%d vias), 4 layers %d/%d (%d vias)\n", two.routed, two.connections, two.vias,
                four.routed, four.connections, four.vias);
    CHECK(four.routed >= two.routed);
}

TEST(pcb_router_keeps_vias_out_of_smd_pads) {
    // Dense SMD board: every via the router drops must sit outside SMD pads (no DRC_VIA_IN_PAD).
    for (int layers : {2, 4, 6}) {
        Project p = amplifierProject();
        p.pcb.settings.layerCount = layers;
        p.pcb.settings.width = 22;
        p.pcb.settings.height = 16;
        p.pcb.autoPlace(p.schematic, true);
        RouteStats st = p.pcb.autoRoute(p.schematic);
        int viaInPad = 0;
        for (const auto& v : p.pcb.runDRC(p.schematic)) viaInPad += v.code == "DRC_VIA_IN_PAD";
        // With wider (automotive) rules the router still keeps exact via-to-pad clearance.
        Project wide = amplifierProject();
        wide.pcb.settings.layerCount = layers;
        wide.pcb.settings.applyPreset("Automotive (IPC-6012 Class 3/A)");
        wide.pcb.settings.width = 26;
        wide.pcb.settings.height = 20;
        wide.pcb.autoPlace(wide.schematic, true);
        wide.pcb.autoRoute(wide.schematic);
        int clearance = 0;
        for (const auto& v : wide.pcb.runDRC(wide.schematic))
            clearance += v.code == "DRC_CLEARANCE_RULE" || v.code == "DRC_CLEARANCE" || v.code == "DRC_SHORT";
        CHECK(clearance == 0);
        std::printf("    %d layers: %d/%d routed, %d vias, %d via-in-pad\n", layers, st.routed, st.connections, st.vias, viaInPad);
        CHECK(viaInPad == 0);
    }
}

namespace {
CustomPartSpec ne555Spec(const std::string& package) {
    CustomPartSpec s;
    s.name = "NE555";
    s.manufacturer = "Texas Instruments";
    s.description = "Precision timer";
    s.package.type = package;
    const char* names[8] = {"GND", "TRIG", "OUT", "RESET", "CONT", "THRES", "DISCH", "VCC"};
    const PinType types[8] = {PinType::PowerIn, PinType::Input, PinType::Output, PinType::Input,
                              PinType::Passive, PinType::Input, PinType::OpenCollector, PinType::PowerIn};
    for (int i = 0; i < 8; ++i) s.pins.push_back({std::to_string(i + 1), names[i], types[i], ""});
    return s;
}
}  // namespace

TEST(custom_part_generation) {
    auto& reg = CustomPartRegistry::instance();
    auto dip = reg.registerPart(ne555Spec("DIP"));
    CHECK(dip->footprint.pads.size() == 8);
    CHECK(dip->footprint.pads[0].throughHole);
    CHECK(dip->footprint.pads[0].pinIndex == 0 && dip->footprint.pads[7].pinIndex == 7);
    CHECK(dip->def.pins.size() == 8);
    CHECK(dip->def.pins[0].offset.x < 0 && dip->def.pins[7].offset.x > 0);  // DIP order: 1 left, 8 right
    CHECK(dip->def.pins[0].offset.y == dip->def.pins[7].offset.y);          // pins 1 and 8 face each other
    CHECK(reg.registerPart(ne555Spec("DIP"))->id == dip->id);               // idempotent
    CHECK(reg.registerPart(ne555Spec("SOIC"))->id != dip->id);

    // Every package generates a footprint whose pads do not overlap.
    struct Case { const char* pkg; int pins; size_t pads; };
    for (Case c : {Case{"SOIC", 14, 14}, Case{"TSSOP", 20, 20}, Case{"QFN", 16, 16}, Case{"LQFP", 32, 32},
                   Case{"SOT23", 5, 5}, Case{"SOT23", 3, 3}, Case{"HEADER", 6, 6}, Case{"TO220", 3, 3}, Case{"DIP", 28, 28}}) {
        CustomPartSpec s;
        s.name = std::string("T_") + c.pkg + std::to_string(c.pins);
        s.package.type = c.pkg;
        for (int i = 1; i <= c.pins; ++i) s.pins.push_back({std::to_string(i), "P" + std::to_string(i), PinType::Passive, ""});
        auto part = reg.registerPart(s);
        CHECK(part->footprint.pads.size() == c.pads);
        bool overlap = false;
        const auto& pads = part->footprint.pads;
        for (size_t i = 0; i < pads.size(); ++i)
            for (size_t j = i + 1; j < pads.size(); ++j)
                overlap |= rectRectDistance(Rect::centered(pads[i].offset, pads[i].size.x, pads[i].size.y),
                                            Rect::centered(pads[j].offset, pads[j].size.x, pads[j].size.y)) < 0.15;
        if (overlap) std::printf("    overlapping pads in %s\n", s.name.c_str());
        CHECK(!overlap);
    }

    // QFN exposed pad from an "EP" pin.
    CustomPartSpec q;
    q.name = "QFN_EP";
    q.package.type = "QFN";
    for (int i = 1; i <= 16; ++i) q.pins.push_back({std::to_string(i), "P" + std::to_string(i), PinType::Passive, ""});
    q.pins.push_back({"EP", "GND", PinType::PowerIn, ""});
    CHECK(reg.registerPart(q)->footprint.pads.size() == 17);

    // Validation.
    CustomPartSpec bad = ne555Spec("SOIC");
    bad.pins[1].number = "1";
    bool threw = false;
    try { reg.registerPart(bad); } catch (const JsonError&) { threw = true; }
    CHECK(threw);

    // Lenient JSON (as produced by language models).
    CustomPartSpec parsed = customPartSpecFromJson(Json::parse(
        R"({"name":"LM7805","package":{"type":"TO-220"},"pins":[{"number":1,"name":"IN","type":"power"},{"number":2,"name":"GND","type":"ground"},{"number":3,"name":"OUT","type":"power out"}]})"));
    CHECK(parsed.package.type == "TO220");
    CHECK(parsed.pins.size() == 3 && parsed.pins[0].type == PinType::PowerIn && parsed.pins[2].type == PinType::PowerOut);
    int pinsFromName = 0;
    (void)pinsFromName;
    CustomPartSpec soic = customPartSpecFromJson(Json::parse(R"({"name":"X","package":"SOIC-8","pins":[{"number":"1","name":"A"}]})"));
    CHECK(soic.package.type == "SOIC" && soic.package.pinCount == 8);
}

TEST(custom_part_in_design) {
    Project p;
    std::string id = p.addCustomPart(ne555Spec("SOIC"));
    auto& s = p.schematic;
    int u = s.addCustomComponent(id, "", {200, 0});
    CHECK(u >= 0);
    CHECK(s.find(u)->ref == "U1");
    CHECK(s.find(u)->value == "NE555");
    CHECK(s.pinIndex(u, "VCC") == 7);
    CHECK(s.pinIndex(u, "8") == 7);  // datasheet pin number
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    int r = s.addComponent(ComponentKind::Resistor, "1k", {100, -60});
    s.connect({v, 1}, {g, 0});
    s.connect({v, 0}, {r, 0});
    s.connect({r, 1}, {u, s.pinIndex(u, "VCC")});

    auto codes = [&]() {
        std::vector<std::string> out;
        for (const auto& e : s.runERC()) out.push_back(e.code);
        return out;
    };
    auto has = [](const std::vector<std::string>& v, const char* c) { return std::find(v.begin(), v.end(), c) != v.end(); };
    CHECK(has(codes(), "ERC_POWER_PIN_UNCONNECTED"));  // GND pin open
    s.connect({u, s.pinIndex(u, "GND")}, {g, 0});
    CHECK(!has(codes(), "ERC_POWER_PIN_UNCONNECTED"));
    CHECK(has(codes(), "ERC_NOT_SIMULATED"));

    // DC still solves around the un-modelled IC.
    Simulator sim(s);
    CHECK(sim.dcOperatingPoint().converged);

    // Place, route and persist.
    p.schematicChanged();
    p.pcb.autoPlace(p.schematic, true);
    RouteStats st = p.pcb.autoRoute(p.schematic);
    CHECK(st.failed == 0);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
    Json saved = p.toJson();
    Project q = Project::fromJson(Json::parse(saved.dump()));
    CHECK(q.customLibrary.size() == 1);
    CHECK(q.schematic.findByRef("U1")->def().name == "NE555");
    CHECK(q.toJson().dump() == saved.dump());
    CHECK(exportBomCsv(q.schematic).find("SOIC-8") != std::string::npos);
    CHECK(!p.removeCustomPart(id));  // still used

    // Editing the part (rename a pin, drop pin 5) keeps wires on matching pin numbers.
    CustomPartSpec edited = ne555Spec("SOIC");
    edited.pins[7].name = "VDD";
    edited.pins.erase(edited.pins.begin() + 4);
    std::string id2 = p.addCustomPart(edited);
    size_t wiresBefore = s.wires().size();
    CHECK(s.replaceCustomPart(id, id2) == 1);
    CHECK(s.wires().size() == wiresBefore);
    CHECK(s.pinIndex(u, "VDD") >= 0);
    CHECK(p.removeCustomPart(id));
}

TEST(standard_values_and_parts) {
    CHECK_NEAR(nearestStandardValue(4800, ESeries::E24), 4700, 1e-9);
    CHECK_NEAR(nearestStandardValue(9.6e-9, ESeries::E12), 10e-9, 1e-18);   // rolls into the next decade
    CHECK_NEAR(nearestStandardValue(330, ESeries::E12), 330, 1e-9);
    CHECK_NEAR(nearestStandardValue(4990, ESeries::E96), 4990, 1e-9);
    CHECK(isStandardValue(4700, ESeries::E24));
    CHECK(isStandardValue(1e5, ESeries::E12));
    CHECK(!isStandardValue(4800, ESeries::E24));
    CHECK(!isStandardValue(-1, ESeries::E24));

    CHECK(standardParts().size() >= 12);
    CHECK(findStandardPart("ne555") != nullptr);
    CHECK(findStandardPart("does-not-exist") == nullptr);
    for (const auto& sp : standardParts()) {
        auto part = CustomPartRegistry::instance().registerPart(sp.spec);
        CHECK(part->footprint.pads.size() >= sp.spec.pins.size());
        // Every package pad maps to a pin (no anonymous pads on the standard parts).
        for (const auto& pad : part->footprint.pads) CHECK(pad.pinIndex >= 0);
    }
    const StandardPart* mega = findStandardPart("ATmega328P");
    CHECK(mega && mega->spec.pins.size() == 28 && mega->spec.pins[6].name == "VCC");
}

namespace {
bool hasCode(const std::vector<RuleViolation>& list, const std::string& code) {
    return std::any_of(list.begin(), list.end(), [&](const RuleViolation& v) { return v.code == code; });
}

/// V1 (+) → R → device pin A, device pin B → GND.
Schematic seriesCircuit(const char* volts, const char* ohms, ComponentKind kind, const char* value, bool reversed = false) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, volts, {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, ohms, {100, 0});
    int d = s.addComponent(kind, value, {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    s.connect({v, 0}, {r, 0});
    s.connect({r, 1}, {d, reversed ? 1 : 0});
    s.connect({d, reversed ? 0 : 1}, {g, 0});
    s.connect({v, 1}, {g, 0});
    return s;
}
}  // namespace

TEST(circuit_validation_rules) {
    // Clean indicator: no warnings.
    Project ok = ledProject();
    for (const auto& v : validateCircuit(ok.schematic)) CHECK(v.severity == Severity::Info);

    CHECK(hasCode(validateCircuit(seriesCircuit("5", "4.8k", ComponentKind::LED, "Red")), "VAL_NONSTANDARD_VALUE"));
    CHECK(!hasCode(validateCircuit(seriesCircuit("5", "4.7k", ComponentKind::LED, "Red")), "VAL_NONSTANDARD_VALUE"));
    CHECK(!hasCode(validateCircuit(seriesCircuit("5", "4.99k", ComponentKind::LED, "Red")), "VAL_NONSTANDARD_VALUE"));  // E96
    CHECK(hasCode(validateCircuit(seriesCircuit("5", "47", ComponentKind::LED, "Red")), "VAL_LED_CURRENT"));
    CHECK(hasCode(validateCircuit(seriesCircuit("5", "330", ComponentKind::LED, "Red", true)), "VAL_REVERSE_BIAS"));
    auto heavy = validateCircuit(seriesCircuit("12", "100", ComponentKind::Resistor, "100"));
    CHECK(hasCode(heavy, "VAL_RESISTOR_POWER"));
    auto shorted = validateCircuit(seriesCircuit("5", "0.5", ComponentKind::Resistor, "0.5"));  // 5 A
    CHECK(hasCode(shorted, "VAL_SUPPLY_CURRENT"));
    CHECK(hasCode(validateCircuit(seriesCircuit("12", "10", ComponentKind::Fuse, "100m")), "VAL_FUSE_OVERLOAD"));
    CHECK(hasCode(validateCircuit(seriesCircuit("5", "1", ComponentKind::Diode, "1N4148")), "VAL_DIODE_CURRENT"));

    // Saturated op-amp: gain of 1000 on a 1 V input.
    Schematic s;
    int vin = s.addComponent(ComponentKind::VoltageSource, "1", {0, 0});
    int u = s.addComponent(ComponentKind::OpAmp, "LM358", {100, 0});
    int rf = s.addComponent(ComponentKind::Resistor, "1M", {150, -60});
    int rg = s.addComponent(ComponentKind::Resistor, "1k", {50, -60});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    s.connect({vin, 0}, {u, 0});
    s.connect({vin, 1}, {g, 0});
    s.connect({u, 2}, {rf, 1});
    s.connect({rf, 0}, {u, 1});
    s.connect({rg, 1}, {u, 1});
    s.connect({rg, 0}, {g, 0});
    CHECK(hasCode(validateCircuit(s), "VAL_OPAMP_SATURATED"));

    // Decoupling on a standard IC's supply rail.
    Project p;
    std::string id = p.addCustomPart(findStandardPart("NE555")->spec);
    auto& sch = p.schematic;
    int timer = sch.addCustomComponent(id, "", {200, 0});
    int v = sch.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int gnd = sch.addComponent(ComponentKind::Ground, "", {0, 80});
    sch.connect({v, 0}, {timer, sch.pinIndex(timer, "8")});
    sch.connect({v, 1}, {gnd, 0});
    sch.connect({timer, sch.pinIndex(timer, "1")}, {gnd, 0});
    CHECK(hasCode(validateCircuit(sch), "VAL_NO_DECOUPLING"));
    int c = sch.addComponent(ComponentKind::Capacitor, "100n", {260, 0});
    sch.connect({c, 0}, {timer, sch.pinIndex(timer, "8")});
    sch.connect({c, 1}, {gnd, 0});
    CHECK(!hasCode(validateCircuit(sch), "VAL_NO_DECOUPLING"));
}

TEST(pcb_rule_presets_and_manufacturability_checks) {
    Project p = ledProject();
    CHECK(designRulePresets().size() == 13);
    CHECK(p.pcb.settings.applyPreset("Prototype (Conservative)"));
    CHECK_NEAR(p.pcb.settings.trackWidth, 0.30, 1e-9);
    CHECK(!p.pcb.settings.applyPreset("nope"));
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());

    // Persisted with the project.
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.rulePreset == "Prototype (Conservative)");
    CHECK_NEAR(q.pcb.settings.minHoleToHole, 0.50, 1e-9);

    auto codes = [](const Project& pr) {
        std::vector<std::string> v;
        for (const auto& e : pr.pcb.runDRC(pr.schematic)) v.push_back(e.code);
        return v;
    };
    auto has = [](const std::vector<std::string>& v, const char* c) { return std::find(v.begin(), v.end(), c) != v.end(); };

    // Thin track, tiny drill, thin annular ring, crowded holes, stub, acute join, via in pad.
    Project bad = p;
    int net = bad.pcb.tracks.front().net;
    Track thin = bad.pcb.tracks.front();
    thin.width = 0.08;
    bad.pcb.addTrack(thin);
    Via v1;
    v1.net = net;
    v1.position = {3, 3};
    v1.drill = 0.1;
    v1.diameter = 0.25;
    bad.pcb.addVia(v1);
    Via v2 = v1;
    v2.position = {3.3, 3};
    v2.drill = 0.3;
    v2.diameter = 0.6;
    bad.pcb.addVia(v2);
    Track stub;
    stub.net = net;
    stub.layer = 0;
    stub.width = 0.25;
    stub.a = {2, 8};
    stub.b = {6, 8};
    bad.pcb.addTrack(stub);
    Track acute = stub;
    acute.a = {6, 8};
    acute.b = {2, 9};
    bad.pcb.addTrack(acute);
    auto pads = bad.pcb.pads(bad.schematic);
    for (const auto& pad : pads)
        if (!pad.throughHole && pad.net >= 0) {
            Via inPad;
            inPad.net = pad.net;
            inPad.position = pad.position;
            bad.pcb.addVia(inPad);
            break;
        }
    auto c = codes(bad);
    CHECK(has(c, "DRC_TRACK_WIDTH"));
    CHECK(has(c, "DRC_DRILL_SIZE"));
    CHECK(has(c, "DRC_ANNULAR_RING"));
    CHECK(has(c, "DRC_HOLE_SPACING"));
    CHECK(has(c, "DRC_DANGLING_TRACK"));
    CHECK(has(c, "DRC_ACUTE_ANGLE"));
    CHECK(has(c, "DRC_VIA_IN_PAD"));

    // IPC-2221: ~1 A on 0.3 mm outer track at 10 °C rise needs ≈0.3 mm; 3 A needs far more.
    CHECK(ipc2221TrackWidth(1.0, 10, 1, false) > 0.25 && ipc2221TrackWidth(1.0, 10, 1, false) < 0.40);
    CHECK(ipc2221TrackWidth(1.0, 10, 1, true) > ipc2221TrackWidth(1.0, 10, 1, false));
    Project power = ledProject();
    power.schematic.setValue(power.schematic.findByRef("R1")->id, "1");  // ~3 A through a 0.3 mm track
    power.schematicChanged();
    power.pcb.settings.applyPreset("Prototype (Conservative)");
    power.pcb.settings.autoSizeNets = false;  // route at the default width to provoke the finding
    power.pcb.autoPlace(power.schematic, true);
    power.pcb.autoRoute(power.schematic);
    CHECK(has(codes(power), "DRC_TRACK_CURRENT"));

    // Graded clearance: between the fab minimum and the design rule is a warning, not an error.
    Project graded = ledProject();
    graded.pcb.autoPlace(graded.schematic, true);
    graded.pcb.autoRoute(graded.schematic);
    graded.pcb.settings.clearance = 5.0;  // absurd design rule, fab minimum unchanged
    auto gv = graded.pcb.runDRC(graded.schematic);
    CHECK(std::any_of(gv.begin(), gv.end(), [](const RuleViolation& v) { return v.code == "DRC_CLEARANCE_RULE"; }));
    CHECK(std::none_of(gv.begin(), gv.end(), [](const RuleViolation& v) { return v.code == "DRC_CLEARANCE"; }));
}

// ======================================================================= persistence & exports

TEST(design_verification_pipeline) {
    auto stage = [](const VerificationReport& r, const char* id) -> const VerificationStage* {
        for (const auto& s : r.stages)
            if (s.id == id) return &s;
        return nullptr;
    };

    // Empty design fails at ERC; later stages are skipped.
    VerificationReport empty = verifyDesign(Project{});
    CHECK(empty.verdict == StageStatus::Fail);
    CHECK(empty.stages.size() == 8);
    CHECK(stage(empty, "routing")->status == StageStatus::Skipped);

    // Schematic only: no placement yet.
    Project p = ledProject();
    for (const auto& c : p.schematic.components()) p.schematic.find(c.id)->pcb.placed = false;
    p.pcb.clearRouting();
    VerificationReport unplaced = verifyDesign(p);
    CHECK(stage(unplaced, "erc")->status != StageStatus::Fail);
    CHECK(stage(unplaced, "simulation")->status == StageStatus::Pass);
    CHECK(stage(unplaced, "placement")->status == StageStatus::Fail);
    CHECK(!unplaced.passed());

    // Placed but not routed.
    p.pcb.autoPlace(p.schematic, true);
    VerificationReport unrouted = verifyDesign(p);
    CHECK(stage(unrouted, "placement")->status == StageStatus::Pass);
    CHECK(stage(unrouted, "routing")->status == StageStatus::Fail);
    CHECK(unrouted.verdict == StageStatus::Fail);

    // Fully routed: every stage passes and every manufacturing file is checked.
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    VerificationReport done = verifyDesign(p);
    for (const auto& s : done.stages)
        if (s.status == StageStatus::Fail) std::printf("    stage %s failed: %s\n", s.id.c_str(), s.summary.c_str());
    CHECK(done.passed());
    CHECK(done.errors == 0);
    const VerificationStage* mfg = stage(done, "manufacturing");
    CHECK(mfg && mfg->status == StageStatus::Pass);
    // Copper per layer, mask ×2, silk, outline, drill, BOM, PnP, netlist, then paste, designators, IPC-356, job, zip.
    CHECK(mfg && mfg->details.size() == static_cast<size_t>(p.pcb.settings.layerCount + 4 + 4 + 5));
    std::string md = done.toMarkdown();
    CHECK(md.find("# Design Verification Report") != std::string::npos);
    CHECK(md.find("| Routing Completion | PASS |") != std::string::npos);
    Json j = Json::parse(done.toJson().dump());
    CHECK(j["verdict"].asString() == stageStatusName(done.verdict));
    CHECK(j["stages"].size() == 8);

    // Four layers: one copper Gerber per layer is verified.
    p.pcb.settings.layerCount = 4;
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    VerificationReport four = verifyDesign(p);
    CHECK(stage(four, "manufacturing")->details.size() == 17);
    CHECK(four.passed());

    // An overloaded resistor fails circuit validation and therefore the design.
    Project hot = p;
    for (const auto& c : p.schematic.components())
        if (c.kind == ComponentKind::Resistor) hot.schematic.setValue(c.id, "10");
    VerificationReport overloaded = verifyDesign(hot);
    CHECK(stage(overloaded, "validation")->status == StageStatus::Fail);
    CHECK(!overloaded.passed());
}

TEST(industry_profiles_and_derating) {
    CHECK(industryProfiles().size() == 21);
    for (const char* id : {"general", "robotics", "uav", "power", "automotive", "rf", "space", "marine", "industrial",
                           "medical", "defence", "networking", "vlsi", "memory"}) {
        const IndustryProfile* p = findIndustry(id);
        CHECK(p != nullptr);
        if (!p) continue;
        BoardSettings b;
        CHECK(b.applyPreset(p->rulePreset));  // every profile names a real preset
        CHECK(!p->guidance.empty() && !p->standards.empty());
    }
    CHECK(findIndustry("SPACE") != nullptr);
    CHECK(findIndustry("nope") == nullptr);

    const IndustryProfile& space = *findIndustry("space");
    PartRatings r = deratedRatings(space);
    CHECK_NEAR(r.resistorPower, 0.0625, 1e-12);
    CHECK_NEAR(r.ledCurrent, 0.015, 1e-12);
    CHECK(r.derating.find("Space") != std::string::npos);
    CHECK(deratedRatings(*findIndustry("general")).derating.empty());

    // Project: the profile sets rules and altitude and survives a save/load.
    Project p = ledProject();
    CHECK(!p.applyIndustry("unknown"));
    CHECK(p.applyIndustry("space"));
    CHECK(p.pcb.settings.rulePreset == "Space (IPC-6012 Class 3/A, ECSS)");
    CHECK(p.pcb.settings.highAltitude);
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.industry == "space");
    CHECK(q.pcb.settings.highAltitude);

    // A 330 Ω LED at 5 V (~9 mA) is fine in general, but a 150 Ω one (~20 mA) exceeds the space-derated 15 mA.
    Project led = ledProject();
    for (const auto& c : led.schematic.components())
        if (c.kind == ComponentKind::Resistor) led.schematic.setValue(c.id, "150");
    auto hasLedFinding = [](const std::vector<RuleViolation>& v) {
        for (const auto& f : v)
            if (f.code == "VAL_LED_CURRENT") return f.message.find("derating") != std::string::npos;
        return false;
    };
    CHECK(!hasLedFinding(validateCircuit(led.schematic, led.partRatings())));
    led.applyIndustry("space");
    CHECK(hasLedFinding(validateCircuit(led.schematic, led.partRatings())));

    led.pcb.autoPlace(led.schematic, true);
    CHECK(led.pcb.autoRoute(led.schematic).failed == 0);
    VerificationReport report = verifyDesign(led);
    CHECK(report.industryName == "Space");
    CHECK(report.toMarkdown().find("Industry profile:** Space") != std::string::npos);
}

TEST(router_keeps_voltage_spacing) {
    // 48 V (PoE/telecom) board on fine High-Speed rules: the router must keep the IPC-2221 B2 0.6 mm spacing, not the
    // 0.15 mm design rule, between the 48 V and ground copper; the design rule itself is left unchanged.
    Project p = ledProject();
    for (const auto& c : p.schematic.components()) {
        if (c.kind == ComponentKind::VoltageSource) p.schematic.setValue(c.id, "48");
        if (c.kind == ComponentKind::Resistor) p.schematic.setValue(c.id, "4.7k 1W");
    }
    p.schematicChanged();
    CHECK(p.pcb.settings.applyPreset("High-Speed Digital (100 Ω diff)"));
    CHECK_NEAR(voltageRoutingClearance(p.schematic, false), 0.6, 1e-12);
    CHECK_NEAR(voltageRoutingClearance(ledProject().schematic, false), 0.1, 1e-12);  // 5 V: the design rule wins
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    CHECK_NEAR(p.pcb.settings.clearance, 0.15, 1e-12);
    for (const auto& v : p.pcb.runDRC(p.schematic)) CHECK(v.code != "DRC_HV_CLEARANCE");
}

// ======================================================================= microcontroller simulation

namespace {
const std::string& exampleHex(const char* id) {
    const FirmwareExample* e = findFirmwareExample(id);
    static const std::string none;
    return e ? e->hex : none;
}

/// 5 V supply, an ATmega328P (or ATtiny85) with VCC/GND (and AVCC) wired, firmware attached. Returns the MCU id.
int mcuBoard(Project& p, const char* part, const char* firmware, int& gnd) {
    auto& s = p.schematic;
    std::string id = p.addCustomPart(findStandardPart(part)->spec);
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    gnd = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int u = s.addCustomComponent(id, part, {200, 0});
    wire(s, v, "-", gnd, "GND");
    wire(s, v, "+", u, "VCC");
    wire(s, u, "GND", gnd, "GND");
    if (s.pinIndex(u, "AVCC") >= 0) wire(s, u, "AVCC", v, "+");
    if (firmware) s.setFirmware(u, exampleHex(firmware), firmware, 0);
    p.schematicChanged();
    return u;
}

int countEdges(const std::vector<double>& v, double threshold) {
    int edges = 0;
    for (size_t i = 1; i < v.size(); ++i) edges += (v[i - 1] < threshold) != (v[i] < threshold);
    return edges;
}
}  // namespace

TEST(intel_hex_parser) {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 70; ++i) bytes.push_back(static_cast<uint8_t>(i * 7));
    std::string hex = toIntelHex(bytes);
    HexImage img = parseIntelHex(hex);
    CHECK(img.ok());
    CHECK(img.bytes == bytes);
    CHECK(!parseIntelHex(":10000000FFFF\n").ok());
    std::string bad = hex;
    bad[10] = bad[10] == '0' ? '1' : '0';  // corrupt a data digit: checksum fails
    CHECK(parseIntelHex(bad).error.find("checksum") != std::string::npos);
    CHECK(parseIntelHex("hello").error.find("Intel HEX") != std::string::npos);
    CHECK(!parseIntelHex(":00000001FF\n").ok());  // no data
    CHECK(mcuModelForPart("ATmega328P") == McuModel::ATmega328P);
    CHECK(mcuModelForPart("atmega328p-pu") == McuModel::ATmega328P);
    CHECK(mcuModelForPart("ATtiny85") == McuModel::ATtiny85);
    CHECK(!mcuModelForPart("NE555"));
    CHECK(firmwareExamples().size() >= 9);
    for (const auto& e : firmwareExamples()) CHECK(parseIntelHex(e.hex).ok());
}

TEST(avr_cpu_matches_reference_simulator) {
    // avr-gcc output of Core/tests/firmware/alu.c; the expected text is simavr's output for the same image.
    const std::string expected = "-23\n79\n-7700\n-14\n-2\n43\n88\n2\n2\n64\n25\n-25\n-12024\n-30585\n-38\n-147\n-1544\n26784\n8571\n3\n19456\n-987641976\n819560599\n-80004\n-4941\n-7716050\n4000065537\n376121344\n61034\n14742\n488281\n-3962745\n3600000000\n3554416254\n2802067423\n3596950572\n229283573\n3256818826\n610\n15129\n-456\n3141\n64159\n-1256636\n31006\n1\n-987654\n9\n0\nSiEDA-AVR\n-17\n-3\n0\n4\n4\n5\n9\n22\n1\nEND\n";
    HexImage img = parseIntelHex(exampleHex("cpu_selftest"));
    CHECK(img.ok());
    AvrMcu mcu(McuModel::ATmega328P);
    std::string err;
    CHECK(mcu.loadFirmware(img.bytes, err));
    mcu.setSupply(5.0);
    std::string out;
    for (int i = 0; i < 400 && out.find("END\n") == std::string::npos; ++i) {
        mcu.run(16000);
        out += mcu.takeSerialOutput();
    }
    CHECK(out == expected);
    CHECK(mcu.fault().empty());
}

TEST(avr_peripherals_standalone) {
    AvrMcu mcu(McuModel::ATmega328P);
    std::string err;
    CHECK(mcu.loadFirmware(parseIntelHex(exampleHex("arduino_analog_serial")).bytes, err));
    mcu.setSupply(5.0);
    mcu.setPinVoltage(mcu.pinIndex("PC0"), 2.5);
    std::string out;
    for (int i = 0; i < 160; ++i) {  // 160 ms
        mcu.run(16000);
        out += mcu.takeSerialOutput();
    }
    // millis() and Serial.println at 115200: "0 512", "50 512", "100 512", "150 512" (the Arduino core's drift included).
    CHECK(out.rfind("0 512\r\n50 512\r\n100 512\r\n150 512\r\n", 0) == 0);
    // Flash larger than the chip is refused.
    AvrMcu tiny(McuModel::ATtiny85);
    CHECK(!tiny.loadFirmware(std::vector<uint8_t>(9000, 0), err));
    CHECK(err.find("flash") != std::string::npos);
    CHECK(tiny.pinIndex("PB5/RST") == 5);
    CHECK(tiny.pinIndex("PD0") == -1);
}

TEST(mcu_blinks_an_led_in_the_circuit) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATmega328P", "arduino_blink", gnd);
    auto& s = p.schematic;
    int r = s.addComponent(ComponentKind::Resistor, "330", {350, 0});
    int d = s.addComponent(ComponentKind::LED, "Red", {450, 0});
    wire(s, u, "PB5", r, "1");
    wire(s, r, "2", d, "A");
    wire(s, d, "K", gnd, "GND");
    p.schematicChanged();
    Simulator sim(s);
    TransientResult tr = sim.transient(1.0, 1e-4);
    CHECK(tr.ok);
    CHECK(tr.mcus.size() == 1 && tr.mcus[0].running);
    CHECK(tr.mcus[0].status.find("16 MHz") != std::string::npos);
    int ledNet = s.netOf({r, 0});
    const auto& v = tr.netVoltages[static_cast<size_t>(ledNet)];
    // 200 ms on / 200 ms off: edges at 0, 0.2, 0.4, 0.6, 0.8 and 1.0 s (Arduino delay() is micros()-accurate).
    std::vector<double> edges;
    for (size_t i = 1; i < v.size(); ++i)
        if ((v[i - 1] < 2.5) != (v[i] < 2.5)) edges.push_back(tr.time[i]);
    CHECK(edges.size() == 6);
    for (size_t i = 0; i < edges.size(); ++i) CHECK_NEAR(edges[i], 0.2 * static_cast<double>(i), 0.0015);
    double peak = *std::max_element(v.begin(), v.end());
    CHECK(peak > 4.5 && peak < 5.0);  // 25 Ω output resistance drops a little under the LED current
    double iMax = *std::max_element(tr.currents[d].begin(), tr.currents[d].end());
    CHECK(iMax > 0.007 && iMax < 0.011);  // (5 V − ~2 V) / (330 + 25) Ω
    // Pin at 0.3 s is off, at 0.1 s on.
    CHECK(v[1000] > 4.5);
    CHECK(v[3000] < 0.5);
}

TEST(mcu_adc_pwm_and_serial_in_the_circuit) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATmega328P", "arduino_analog_serial", gnd);
    auto& s = p.schematic;
    int supply = s.addComponent(ComponentKind::VoltageSource, "5", {0, 200});  // separate source for the divider
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, 200});
    int r2 = s.addComponent(ComponentKind::Resistor, "30k", {100, 300});
    wire(s, supply, "-", gnd, "GND");
    wire(s, supply, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", gnd, "GND");
    wire(s, r1, "2", u, "PC0");  // A0 = 3.75 V → 767
    // PWM on pin 9 (PB1) into an RC low-pass: the filtered voltage follows the duty (767 / 4 = 191 → 3.75 V).
    int rf = s.addComponent(ComponentKind::Resistor, "10k", {400, 0});
    int cf = s.addComponent(ComponentKind::Capacitor, "10u", {500, 50});
    wire(s, u, "PB1", rf, "1");
    wire(s, rf, "2", cf, "1");
    wire(s, cf, "2", gnd, "GND");
    p.schematicChanged();
    Simulator sim(s);
    TransientResult tr = sim.transient(0.6, 2e-5);
    CHECK(tr.ok);
    CHECK(tr.mcus.size() == 1);
    const std::string& serial = tr.mcus[0].serial;
    CHECK(serial.find("0 767\r\n") == 0);
    CHECK(serial.find(" 767\r\n", 10) != std::string::npos);
    const auto& vf = tr.netVoltages[static_cast<size_t>(s.netOf({rf, 1}))];
    double avg = 0;
    for (size_t i = vf.size() - 5000; i < vf.size(); ++i) avg += vf[i];
    avg /= 5000;
    CHECK_NEAR(avg, 5.0 * 191 / 255, 0.12);
    // JSON carries the serial monitor text and the status.
    Json j = p.transientToJson(tr);
    CHECK(j.get("mcus").items().size() == 1);
    CHECK(j.get("mcus").items()[0].get("serial").asString().find("767") != std::string::npos);
}

TEST(mcu_button_interrupt_with_live_switch) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATmega328P", "button_interrupt", gnd);
    auto& s = p.schematic;
    int sw = s.addComponent(ComponentKind::Switch, "open", {300, 100});
    int r = s.addComponent(ComponentKind::Resistor, "470", {350, 0});
    int d = s.addComponent(ComponentKind::LED, "Green", {450, 0});
    wire(s, u, "PD2", sw, "1");
    wire(s, sw, "2", gnd, "GND");
    wire(s, u, "PB5", r, "1");
    wire(s, r, "2", d, "A");
    wire(s, d, "K", gnd, "GND");
    p.schematicChanged();
    Simulator sim(s);
    std::string err;
    CHECK(sim.begin(err));
    auto ledVolts = [&]() { return sim.netVoltages()[static_cast<size_t>(s.netOf({r, 0}))]; };
    for (int i = 0; i < 200; ++i) CHECK(sim.advance(1e-4, err));  // 20 ms released
    CHECK(ledVolts() < 0.5);
    for (int press = 0; press < 3; ++press) {
        sim.setSwitch(sw, true);
        for (int i = 0; i < 100; ++i) sim.advance(1e-4, err);
        CHECK(ledVolts() > 4.0);  // LED on while pressed
        sim.setSwitch(sw, false);
        for (int i = 0; i < 100; ++i) sim.advance(1e-4, err);
        CHECK(ledVolts() < 0.5);
    }
    auto reports = sim.mcuReports();
    CHECK(reports.size() == 1);
    CHECK(reports[0].serial == "press 1\npress 2\npress 3\n");
    CHECK_NEAR(sim.time(), 0.08, 1e-9);
}

TEST(attiny85_and_mcu_status) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATtiny85", "tiny_blink", gnd);
    auto& s = p.schematic;
    int r = s.addComponent(ComponentKind::Resistor, "1k", {350, 0});
    wire(s, u, "PB0", r, "1");
    wire(s, r, "2", gnd, "GND");
    p.schematicChanged();
    TransientResult tr = Simulator(s).transient(0.5, 1e-4);
    CHECK(tr.ok);
    CHECK(tr.mcus.size() == 1 && tr.mcus[0].model == "ATtiny85");
    CHECK(tr.mcus[0].status.find("8 MHz") != std::string::npos);
    CHECK(countEdges(tr.netVoltages[static_cast<size_t>(s.netOf({r, 0}))], 2.5) == 5);  // toggles every 100 ms

    // No firmware: a clear status, pins stay high-impedance.
    s.setFirmware(u, "", "", 0);
    TransientResult none = Simulator(s).transient(0.01, 1e-4);
    CHECK(none.ok && !none.mcus[0].running);
    CHECK(none.mcus[0].status.find("No firmware") != std::string::npos);
    CHECK(std::fabs(none.netVoltages[static_cast<size_t>(s.netOf({r, 0}))].back()) < 1e-3);

    // A 1.5 V supply holds the chip in reset.
    s.setFirmware(u, exampleHex("tiny_blink"), "tiny_blink", 0);
    for (const auto& c : s.components())
        if (c.kind == ComponentKind::VoltageSource) s.setValue(c.id, "1.5");
    TransientResult low = Simulator(s).transient(0.01, 1e-4);
    CHECK(!low.mcus[0].running);
    CHECK(low.mcus[0].status.find("reset") != std::string::npos);
}

TEST(firmware_persists_with_the_project) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATmega328P", "blink", gnd);
    p.schematic.setFirmware(u, exampleHex("blink"), "blink.hex", 8e6);
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    const Component* c = q.schematic.findByRef(p.schematic.find(u)->ref);
    CHECK(c != nullptr);
    if (!c) return;
    CHECK(c->firmware == exampleHex("blink"));
    CHECK(c->firmwareName == "blink.hex");
    CHECK_NEAR(c->clockHz, 8e6, 1e-6);
    Json snap = q.snapshot();
    bool found = false;
    for (const auto& j : snap.get("components").items())
        if (j.get("ref").asString() == c->ref) {
            found = true;
            CHECK(j.get("mcu").get("model").asString() == "ATmega328P");
            CHECK(j.get("mcu").get("firmwareName").asString() == "blink.hex");
            CHECK(j.get("mcu").get("firmwareBytes").asInt() > 100);
            CHECK_NEAR(j.get("mcu").get("clockHz").asNumber(), 8e6, 1e-6);
        }
    CHECK(found);
}

TEST(live_simulation_c_api) {
    SiedaProject* p = sieda_project_new("live");
    // ATmega328P from the standard library, through the C API like the app does.
    Json parts = Json::parse(std::string([] {
        char* j = sieda_standard_parts_json();
        std::string s = j;
        sieda_string_free(j);
        return s;
    }()));
    std::string specJson;
    for (const auto& item : parts.items())
        if (item.get("spec").get("name").asString() == "ATmega328P") specJson = item.get("spec").dump();
    CHECK(!specJson.empty());
    char* err = nullptr;
    char* partJson = sieda_custom_part_register(p, specJson.c_str(), &err);
    CHECK(partJson != nullptr);
    std::string partId = Json::parse(partJson).get("id").asString();
    sieda_string_free(partJson);
    int u = sieda_add_custom_component(p, partId.c_str(), "ATmega328P", 200, 0, 0, "U1");
    int v = sieda_add_component(p, static_cast<int32_t>(ComponentKind::VoltageSource), "5", 0, 0, 0, "V1");
    int g = sieda_add_component(p, static_cast<int32_t>(ComponentKind::Ground), "0", 0, 100, 0, "GND1");
    int sw = sieda_add_component(p, static_cast<int32_t>(ComponentKind::Switch), "push", 300, 100, 0, "SW1");
    int r = sieda_add_component(p, static_cast<int32_t>(ComponentKind::Resistor), "330", 350, 0, 0, "R1");
    int d = sieda_add_component(p, static_cast<int32_t>(ComponentKind::LED), "Red", 450, 0, 0, "D1");
    auto pin = [&](int c, const char* name) { return sieda_find_pin(p, c, name); };
    CHECK(sieda_connect(p, v, pin(v, "+"), u, pin(u, "VCC")) >= 0);
    CHECK(sieda_connect(p, v, pin(v, "-"), g, 0) >= 0);
    CHECK(sieda_connect(p, u, pin(u, "GND"), g, 0) >= 0);
    CHECK(sieda_connect(p, u, pin(u, "PD2"), sw, 0) >= 0);
    CHECK(sieda_connect(p, sw, 1, g, 0) >= 0);
    CHECK(sieda_connect(p, u, pin(u, "PB5"), r, 0) >= 0);
    CHECK(sieda_connect(p, r, 1, d, 0) >= 0);
    CHECK(sieda_connect(p, d, 1, g, 0) >= 0);
    char* hex = sieda_firmware_example_hex("button_interrupt");
    CHECK(hex != nullptr);
    CHECK(sieda_set_firmware(p, u, hex, "Button", 0, &err) == 1);
    CHECK(sieda_set_firmware(p, r, hex, "Button", 0, &err) == 0);  // a resistor is not a microcontroller
    CHECK(err != nullptr && std::string(err).find("not a microcontroller") != std::string::npos);
    sieda_string_free(err);
    err = nullptr;
    CHECK(sieda_set_firmware(p, u, ":nonsense", "x", 0, &err) == 0);
    sieda_string_free(err);
    err = nullptr;
    sieda_string_free(hex);

    SiedaLiveSim* live = sieda_live_start(p, &err);
    CHECK(live != nullptr);
    auto state = [&]() {
        char* j = sieda_live_state(live);
        Json out = Json::parse(j);
        sieda_string_free(j);
        return out;
    };
    auto led = [&](const Json& st) {
        for (const auto& l : st.get("leds").items())
            if (l.get("ref").asString() == "D1") return l.get("brightness").asNumber();
        return -1.0;
    };
    CHECK(sieda_live_run(live, 0.02, 1e-4, 50, &err) == 1);
    Json s0 = state();
    CHECK_NEAR(s0.get("time").asNumber(), 0.02, 1e-9);
    CHECK(led(s0) < 0.05);
    CHECK(s0.get("switches").items().size() == 1);
    CHECK(s0.get("switches").items()[0].get("momentary").asBool());
    CHECK(!s0.get("switches").items()[0].get("closed").asBool());
    CHECK(s0.get("trace").get("time").items().size() == 50);
    CHECK(!s0.get("trace").get("nets").items().empty());
    sieda_live_set_switch(live, sw, 1);  // press and hold
    CHECK(sieda_live_run(live, 0.01, 1e-4, 50, &err) == 1);
    Json s1 = state();
    CHECK(led(s1) > 0.4);  // ~8 mA of 15 mA full glow
    CHECK(s1.get("switches").items()[0].get("closed").asBool());
    CHECK(s1.get("mcus").items()[0].get("serial").asString() == "press 1\n");
    CHECK(s1.get("mcus").items()[0].get("running").asBool());
    sieda_live_set_switch(live, sw, 0);
    CHECK(sieda_live_run(live, 0.01, 1e-4, 50, &err) == 1);
    CHECK(led(state()) < 0.05);
    sieda_live_serial_input(live, u, "ignored");  // the button firmware does not read; must not disturb anything
    CHECK(sieda_live_run(live, 0.005, 1e-4, 10, &err) == 1);
    sieda_live_free(live);

    // A circuit without ground cannot start.
    SiedaProject* bad = sieda_project_new("bad");
    sieda_add_component(bad, static_cast<int32_t>(ComponentKind::Resistor), "1k", 0, 0, 0, "R1");
    CHECK(sieda_live_start(bad, &err) == nullptr);
    CHECK(err != nullptr);
    sieda_string_free(err);
    sieda_project_free(bad);
    sieda_project_free(p);
}

TEST(ipc2221_voltage_clearance) {
    CHECK_NEAR(ipc2221Clearance(12, false), 0.1, 1e-12);
    CHECK_NEAR(ipc2221Clearance(48, false), 0.6, 1e-12);
    CHECK_NEAR(ipc2221Clearance(230, false), 1.25, 1e-12);
    CHECK_NEAR(ipc2221Clearance(400, false), 2.5, 1e-12);
    CHECK_NEAR(ipc2221Clearance(1000, false), 5.0, 1e-12);
    CHECK_NEAR(ipc2221Clearance(230, true), 6.4, 1e-12);
    CHECK(ipc2221Clearance(-230, false) == ipc2221Clearance(230, false));

    // 230 V across an 0805 resistor: its pads are ~1 mm apart, below the 1.25 mm B2 spacing.
    auto build = [](const char* volts) {
        Project p;
        auto& s = p.schematic;
        int v = s.addComponent(ComponentKind::VoltageSource, volts, {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "1M", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", r, "1");
        wire(s, r, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        p.schematicChanged();
        p.pcb.autoPlace(p.schematic, true);
        p.pcb.autoRoute(p.schematic);
        int hv = 0;
        for (const auto& f : p.pcb.runDRC(p.schematic)) hv += f.code == "DRC_HV_CLEARANCE";
        return hv;
    };
    CHECK(build("5") == 0);
    CHECK(build("230") > 0);
    CHECK(build("SIN(0 325 50)") > 0);  // mains peak, not the 0 V DC value
}

TEST(no_connect_flags) {
    Project p;
    auto& s = p.schematic;
    std::string id = CustomPartRegistry::instance().registerPart(findStandardPart("NE555")->spec)->id;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    int u = s.addCustomComponent(id, "", {150, 0});
    wire(s, v, "+", u, "8");
    wire(s, u, "1", g, "GND");
    wire(s, v, "-", g, "GND");
    auto count = [&](const char* code) {
        int n = 0;
        for (const auto& f : s.runERC()) n += f.code == code;
        return n;
    };
    CHECK(count("ERC_UNCONNECTED_PIN") == 6);  // pins 2–7 open
    for (int pin = 1; pin <= 6; ++pin) CHECK(s.setPinNoConnect(u, pin, true));
    CHECK(!s.setPinNoConnect(u, 99, true));
    CHECK(count("ERC_UNCONNECTED_PIN") == 0);
    // A flagged pin that gets wired is reported.
    wire(s, u, "4", v, "+");
    CHECK(count("ERC_NC_CONNECTED") == 1);
    // Persisted with the project and visible in the snapshot.
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.schematic.find(u)->noConnect.size() == 6);
    CHECK(p.snapshot().dump().find("\"noConnect\":true") != std::string::npos);
    CHECK(s.setPinNoConnect(u, 3, false));
    CHECK(s.find(u)->noConnect.size() == 5);
}

TEST(net_classes_and_fine_pitch) {
    // Net class: a 1 A LED-strip feed gets a wide track; DRC stays clean around it.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    int r = s.addComponent(ComponentKind::Resistor, "4.7 10W", {120, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {120, 80});
    int d = s.addComponent(ComponentKind::LED, "Red", {200, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", g, "GND");
    wire(s, v, "+", r2, "1");
    wire(s, r2, "2", d, "A");
    wire(s, d, "K", g, "GND");
    wire(s, v, "-", g, "GND");
    p.schematicChanged();
    auto widths = p.pcb.autoNetWidths(s);
    CHECK(!widths.empty());
    for (auto& [net, w] : widths) std::printf("    net class %s = %.2f mm\n", net.c_str(), w);
    p.pcb.settings.width = 30;
    p.pcb.settings.height = 22;
    p.pcb.autoPlace(s, true);
    CHECK(p.pcb.autoRoute(s).failed == 0);
    double widest = 0;
    for (const auto& t : p.pcb.tracks) widest = std::max(widest, t.width);
    CHECK(widest >= 0.45 - 1e-9);
    int bad = 0;
    for (const auto& f : p.pcb.runDRC(s))
        if (f.severity != Severity::Info && f.code != "DRC_TRACK_CURRENT") {
            ++bad;
            std::printf("    %s %s\n", f.code.c_str(), f.message.c_str());
        }
    CHECK(bad == 0);
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.netWidths == p.pcb.settings.netWidths);

    // Fine pitch: an MPU-6050 (0.5 mm QFN) wired up on Class 3 rules has no design-rule clearance findings from
    // its own pad gaps, and tracks entering its pads are necked down to the pad width.
    Project f;
    auto& fs = f.schematic;
    std::string imu = CustomPartRegistry::instance().registerPart(findStandardPart("MPU-6050")->spec)->id;
    int u = fs.addCustomComponent(imu, "", {200, 0});
    int vv = fs.addComponent(ComponentKind::VoltageSource, "3.3", {0, 0});
    int gg = fs.addComponent(ComponentKind::Ground, "", {0, 80});
    int c1 = fs.addComponent(ComponentKind::Capacitor, "100n", {100, 80});
    int c2 = fs.addComponent(ComponentKind::Capacitor, "2.2n", {300, 80});
    int c3 = fs.addComponent(ComponentKind::Capacitor, "100n", {300, 160});
    wire(fs, vv, "+", u, "13");
    wire(fs, u, "8", u, "13");
    wire(fs, u, "18", gg, "GND");
    wire(fs, u, "9", gg, "GND");
    wire(fs, u, "1", gg, "GND");
    wire(fs, u, "11", gg, "GND");
    wire(fs, vv, "-", gg, "GND");
    wire(fs, c1, "1", u, "13");
    wire(fs, c1, "2", gg, "GND");
    wire(fs, c2, "1", u, "20");
    wire(fs, c2, "2", gg, "GND");
    wire(fs, c3, "1", u, "10");
    wire(fs, c3, "2", gg, "GND");
    f.schematicChanged();
    f.pcb.settings.applyPreset("IPC-2221 Class 3");
    f.pcb.settings.width = 24;
    f.pcb.settings.height = 20;
    f.pcb.autoPlace(fs, true);
    RouteStats st = f.pcb.autoRoute(fs);
    std::printf("    QFN board: %d/%d routed\n", st.routed, st.connections);
    CHECK(st.failed == 0);
    int clearance = 0;
    for (const auto& x : f.pcb.runDRC(fs))
        if (x.code == "DRC_CLEARANCE_RULE" || x.code == "DRC_CLEARANCE" || x.code == "DRC_SHORT") {
            ++clearance;
            std::printf("    %s %s\n", x.code.c_str(), x.message.c_str());
        }
    CHECK(clearance == 0);
    bool necked = false;
    for (const auto& t : f.pcb.tracks) necked |= t.width < f.pcb.settings.trackWidth - 1e-9;
    CHECK(necked);
}

TEST(board_outline_holes_and_pours) {
    // Outline presets.
    BoardSettings b;
    b.setOutline(boardOutlinePreset("quad-x", 100, 40, 12));
    CHECK_NEAR(b.width, 100, 1e-6);
    CHECK_NEAR(b.height, 100, 1e-6);
    CHECK(b.contains({50, 50}));
    CHECK(b.contains({88, 88}));    // on a diagonal arm
    CHECK(!b.contains({3, 50}));    // between two arms
    CHECK(!b.contains({50, 97}));
    CHECK(b.edgeDistance({50, 50}) > 15);
    CHECK(b.edgeDistance({3, 50}) < 0);
    CHECK(b.rectInside(Rect::centered({50, 50}, 10, 10), 1));
    CHECK(!b.rectInside(Rect::centered({50, 72}, 10, 10), 0));
    BoardSettings r;
    r.setOutline(boardOutlinePreset("rounded", 30, 20, 4));
    CHECK(r.contains({15, 10}) && !r.contains({0.3, 0.3}) && r.contains({0.3, 10}));
    CHECK(boardOutlinePreset("hexagon", 10, 10, 0).empty());

    // Amplifier on a quad-X frame with a 30.5 mm M3 mounting pattern and ground pours on both sides.
    Project p = amplifierProject();
    auto& s = p.schematic;
    p.pcb.settings.setOutline(boardOutlinePreset("quad-x", 80, 40, 12));
    for (double dx : {-15.25, 15.25})
        for (double dy : {-15.25, 15.25}) p.pcb.settings.holes.push_back({{40 + dx, 40 + dy}, 3.2, 6.4});
    p.pcb.zones.push_back({"GND", 1, false, 0});
    p.pcb.zones.push_back({"GND", 0, false, 0});
    p.pcb.autoPlace(s, true);
    for (const auto& c : s.components())
        if (c.hasFootprint()) CHECK(p.pcb.settings.rectInside(p.pcb.courtyard(c), 0));
    RouteStats st = p.pcb.autoRoute(s);
    std::printf("    quad-X board: %d/%d routed, %d vias\n", st.routed, st.connections, st.vias);
    CHECK(st.failed == 0);
    CHECK(p.pcb.ratsnest(s).empty());
    const auto& fills = p.pcb.zoneFills(s);
    CHECK(fills.size() == 2);
    for (const auto& f : fills) {
        CHECK(f.net >= 0 && f.islands >= 1 && f.area() > 200);
        for (const auto& rc : f.rects) {  // poured copper stays inside the outline and out of the hole keep-outs
            CHECK(p.pcb.settings.edgeDistance(rc.center()) >= p.pcb.settings.edgeClearance);
            CHECK(p.pcb.settings.holeDistance(rc.center()) > 0);
        }
    }
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
    // Ground tracks are only short stubs to the pour: no ground track current warning.
    for (const auto& f : p.pcb.runDRC(s)) CHECK(f.code != "DRC_TRACK_CURRENT");

    std::string edge = exportGerber(s, p.pcb, GerberLayer::EdgeCuts);
    size_t lines = 0;
    for (size_t at = edge.find("D01*"); at != std::string::npos; at = edge.find("D01*", at + 1)) ++lines;
    CHECK(lines == p.pcb.settings.outline.size());
    CHECK(exportCopperGerber(s, p.pcb, 1).find("G36*") != std::string::npos);
    std::string npth = exportExcellonDrill(s, p.pcb, false);
    CHECK(npth.find("T1C3.200") != std::string::npos && npth.find("NPTH") != std::string::npos);
    Mesh m = buildAssemblyMesh(s, p.pcb);
    CHECK(m.triangleCount() > 100);

    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.outline.size() == p.pcb.settings.outline.size());
    CHECK(q.pcb.settings.holes.size() == 4);
    CHECK(q.pcb.zones.size() == 2 && q.pcb.zones[0].net == "GND" && q.pcb.zones[0].layer == 1);
    CHECK(q.pcb.ratsnest(q.schematic).empty());
    Json snap = q.snapshot();
    CHECK(snap.get("zoneFills").items().size() == 2);
    CHECK(snap.get("board").get("holes").items().size() == 4);

    // Off-board and hole keep-out findings.
    Project bad = ledProject();
    bad.pcb.settings.width = 30;
    bad.pcb.settings.height = 20;
    bad.pcb.autoPlace(bad.schematic, true);
    bad.pcb.settings.holes.push_back({bad.schematic.components()[1].pcb.position, 3.2, 6.4});
    bool keepout = false;
    for (const auto& e : bad.pcb.runDRC(bad.schematic)) keepout |= e.code == "DRC_HOLE_KEEPOUT";
    CHECK(keepout);

    // Four-layer board with a reserved ground plane: no other net routes on it, the ground pads reach it by vias.
    Project f = amplifierProject();
    f.pcb.settings.layerCount = 4;
    f.pcb.settings.width = 40;
    f.pcb.settings.height = 30;
    f.pcb.zones.push_back({"GND", 1, true, 0});
    f.pcb.autoPlace(f.schematic, true);
    RouteStats fs = f.pcb.autoRoute(f.schematic);
    CHECK(fs.failed == 0);
    int gnd = -1;
    for (const auto& n : f.schematic.nets())
        if (n.name == "GND") gnd = n.index;
    for (const auto& t : f.pcb.tracks) CHECK(t.layer != 1 || t.net == gnd);
    CHECK(f.pcb.zoneFills(f.schematic)[0].area() > 0.6 * 40 * 30);
    CHECK(drcErrors(f, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
}

TEST(project_json_roundtrip) {
    Project p = amplifierProject();
    p.requirements = "Amplify a sensor signal and light an LED.";
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    Json saved = p.toJson();
    Project q = Project::fromJson(Json::parse(saved.dump(true)));
    CHECK(q.name == p.name);
    CHECK(q.requirements == p.requirements);
    CHECK(q.schematic.components().size() == p.schematic.components().size());
    CHECK(q.schematic.wires().size() == p.schematic.wires().size());
    CHECK(q.pcb.tracks.size() == p.pcb.tracks.size());
    CHECK(q.pcb.vias.size() == p.pcb.vias.size());
    CHECK(q.toJson().dump() == saved.dump());
    // New components after reload must not collide with restored ids.
    int id = q.schematic.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    for (const auto& c : p.schematic.components()) CHECK(c.id != id);
    bool threw = false;
    try {
        Project::fromJson(Json::parse("{\"format\":\"other\"}"));
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(exports_are_well_formed) {
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    std::string spice = exportSpiceNetlist(p.schematic, "amp");
    CHECK(spice.find(".end") != std::string::npos);
    CHECK(spice.find("XU1") != std::string::npos);
    CHECK(spice.find(".subckt OPAMP_IDEAL") != std::string::npos);
    std::string bom = exportBomCsv(p.schematic);
    CHECK(bom.rfind("Item,Quantity", 0) == 0);
    CHECK(bom.find("R_0805") != std::string::npos);
    for (auto layer : {GerberLayer::TopCopper, GerberLayer::BottomCopper, GerberLayer::TopMask, GerberLayer::TopSilk,
                       GerberLayer::EdgeCuts}) {
        std::string g = exportGerber(p.schematic, p.pcb, layer);
        CHECK(g.find("%FSLAX46Y46*%") != std::string::npos);
        CHECK(g.size() > 6 && g.substr(g.size() - 5) == "M02*\n");
    }
    std::string top = exportGerber(p.schematic, p.pcb, GerberLayer::TopCopper);
    CHECK(top.find("D03*") != std::string::npos);
    CHECK(top.find("D01*") != std::string::npos);
    std::string drill = exportExcellonDrill(p.schematic, p.pcb);
    CHECK(drill.rfind("M48", 0) == 0);
    CHECK(drill.find("T1C1.000") != std::string::npos || drill.find("C1.000") != std::string::npos);
    std::string pnp = exportPickAndPlaceCsv(p.schematic);
    CHECK(pnp.find("U1") != std::string::npos);
}

TEST(mesh_is_valid) {
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    Mesh m = buildAssemblyMesh(p.schematic, p.pcb);
    CHECK(m.vertexCount() > 100);
    CHECK(m.indices.size() % 3 == 0);
    CHECK(m.normals.size() == m.positions.size());
    CHECK(m.colors.size() == m.vertexCount() * 4);
    bool finite = true, inRange = true;
    for (float f : m.positions) finite &= std::isfinite(f);
    for (uint32_t i : m.indices) inRange &= i < m.vertexCount();
    CHECK(finite);
    CHECK(inRange);
    // Top face of the board must face up.
    CHECK(m.normals[1] > 0.99f);
    std::string stl = exportStl(m, "board");
    CHECK(stl.rfind("solid board", 0) == 0);
    std::string obj = exportObj(m, "board");
    CHECK(obj.find("\nf ") != std::string::npos);
}

TEST(assembly_mesh_is_realistic) {
    // Every vertex carries what it is made of, so the 3D view can light mask, metal, solder and plastic differently.
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    Mesh m = buildAssemblyMesh(p.schematic, p.pcb);
    CHECK(m.surfaces.size() == m.vertexCount());
    for (uint8_t v : m.surfaces) CHECK(v < kSurfaceCount);
    for (Surface s : {Surface::Mask, Surface::Laminate, Surface::Finish, Surface::Gold, Surface::Tin, Surface::Solder,
                      Surface::Silk, Surface::Plastic, Surface::Ceramic, Surface::Glass, Surface::Hole, Surface::Marking})
        CHECK(m.surfaceVertices(s) > 0);
    // The board's top face is still the first quad, in the mask colour.
    CHECK(m.surfaces[0] == static_cast<uint8_t>(Surface::Mask) && m.normals[1] > 0.99f);

    // A bare board (no parts): pads show their finish, no solder joints, leads or bodies.
    Mesh bare = buildAssemblyMesh(p.schematic, p.pcb, {false, true, true});
    CHECK(bare.surfaceVertices(Surface::Solder) == 0 && bare.surfaceVertices(Surface::Plastic) == 0);
    CHECK(bare.surfaceVertices(Surface::Finish) > 0 && bare.surfaceVertices(Surface::Silk) > 0);
    // Silkscreen legends: reference designators make the silk far richer than the bare outlines.
    size_t parts = 0;
    for (const auto& c : p.schematic.components()) parts += c.hasFootprint() && c.pcb.placed ? 1 : 0;
    CHECK(bare.surfaceVertices(Surface::Silk) > parts * 4 * 24 * 2);

    // The X-ray copper layers are all finish.
    Mesh layer = buildCopperLayerMesh(p.schematic, p.pcb, 0);
    CHECK(layer.surfaces.size() == layer.vertexCount() && layer.surfaceVertices(Surface::Finish) == layer.vertexCount());
}

TEST(silkscreen_stroke_font) {
    // Every character a reference or part value uses draws something; unknown ones fall back to '?'.
    for (char ch : std::string("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcxyz-+./_?")) {
        Mesh t;
        t.addText(std::string(1, ch), {0, 0}, 1.0, 0.0, 0.01, Rgba{});
        CHECK(t.vertexCount() > 0);
    }
    Mesh space;
    space.addText(" ", {0, 0}, 1.0, 0.0, 0.01, Rgba{});
    CHECK(space.vertexCount() == 0);
    Mesh unknown;
    unknown.addText("~", {0, 0}, 1.0, 0.0, 0.01, Rgba{});
    CHECK(unknown.vertexCount() > 0);

    // Width grows with length and height; text is centred, and mirrored text (bottom legend) is its reflection.
    CHECK(Mesh::textWidth("R10", 1.0) > Mesh::textWidth("R1", 1.0));
    CHECK(std::fabs(Mesh::textWidth("U1", 2.0) - 2 * Mesh::textWidth("U1", 1.0)) < 1e-9);
    Mesh a, b;
    a.addText("R7", {10, 5}, 1.0, 0.0, 0.01, Rgba{});
    b.addText("R7", {10, 5}, 1.0, 0.0, 0.01, Rgba{}, true);
    double minA = 1e9, maxA = -1e9, sumA = 0, sumB = 0;
    for (size_t v = 0; v < a.vertexCount(); ++v) {
        minA = std::min(minA, double(a.positions[v * 3]));
        maxA = std::max(maxA, double(a.positions[v * 3]));
        sumA += a.positions[v * 3] - 10;
        sumB += b.positions[v * 3] - 10;
    }
    CHECK(std::fabs((minA + maxA) / 2 - 10) < 0.2);
    CHECK(a.vertexCount() == b.vertexCount());
    CHECK(std::fabs(sumA + sumB) < 1e-3);  // x mirrored about the centre
    // Text tops point to board −Y (the top of the PCB view): the '7' bar sits at the smallest Z.
    double minZ = 1e9, maxZ = -1e9;
    for (size_t v = 0; v < a.vertexCount(); ++v) {
        minZ = std::min(minZ, double(a.positions[v * 3 + 2]));
        maxZ = std::max(maxZ, double(a.positions[v * 3 + 2]));
    }
    CHECK(minZ < 5 - 0.4 && maxZ > 5 + 0.4);
}

namespace {
/// Positions (x, y, z) of the vertices tagged with `s`.
std::vector<Vec3> surfacePoints(const Mesh& m, Surface s) {
    std::vector<Vec3> pts;
    for (size_t v = 0; v < m.vertexCount(); ++v)
        if (m.surfaces[v] == static_cast<uint8_t>(s)) pts.push_back({m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]});
    return pts;
}
bool anyIn(const std::vector<Vec3>& pts, Rect r, double y0, double y1) {
    for (const auto& p : pts)
        if (p.x >= r.x0 && p.x <= r.x1 && p.z >= r.y0 && p.z <= r.y1 && p.y >= y0 && p.y <= y1) return true;
    return false;
}
}  // namespace

TEST(realistic_assembly_geometry) {
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    const auto& s = p.schematic;
    const double t = p.pcb.settings.thickness;
    Mesh m = buildAssemblyMesh(s, p.pcb);
    const auto solder = surfacePoints(m, Surface::Solder), tin = surfacePoints(m, Surface::Tin);
    const auto silk = surfacePoints(m, Surface::Silk), marking = surfacePoints(m, Surface::Marking);
    const auto glass = surfacePoints(m, Surface::Glass), ceramic = surfacePoints(m, Surface::Ceramic);
    const auto pads = p.pcb.pads(s);

    for (const auto& pad : pads) {
        const Component* c = s.find(pad.componentId);
        if (!c || c->pcb.bottom) continue;
        if (!pad.throughHole) {
            // A solder dome sits on every top SMD pad, inside the pad and just above the copper.
            CHECK(anyIn(solder, pad.bounds(), 0.0, 0.2));
        } else {
            // Through-hole joints are on the solder side, under the board.
            CHECK(anyIn(solder, pad.bounds().inflated(0.05), -t - 0.4, -t));
        }
    }
    // Every SOIC lead reaches its pad: tinned metal over each pad that sticks out of the package body.
    int socs = 0, leds = 0, chips = 0, smdPads = 0, thPads = 0;
    for (const auto& pad : pads) (pad.throughHole ? thPads : smdPads) += 1;
    CHECK(smdPads > 10 && thPads >= 2);
    for (const auto& c : s.components()) {
        if (c.def().footprint != "SOIC8_OPAMP") continue;
        ++socs;
        int reached = 0, total = 0;
        for (const auto& pad : pads) {
            if (pad.componentId != c.id) continue;
            ++total;
            reached += anyIn(tin, pad.bounds(), 0.0, 0.3) ? 1 : 0;
        }
        CHECK(total == 8 && reached == 8);
        // The part number is etched on the package top and a pin-1 dimple sits on it.
        const FootprintDef* fp = Library::instance().footprint(c.def().footprint);
        Rect body = Rect::centered(c.pcb.position, fp->body.width + 0.01, fp->body.depth + 0.01);
        if (((c.pcb.rotation / 90) % 2 + 2) % 2 == 1) body = Rect::centered(c.pcb.position, fp->body.depth + 0.01, fp->body.width + 0.01);
        CHECK(anyIn(marking, body, fp->body.height, fp->body.height + 0.1));
    }
    // Each placed part's reference is printed on the silkscreen above its outline.
    for (const auto& c : s.components()) {
        if (!c.hasFootprint() || !c.pcb.placed || c.ref.empty()) continue;
        Rect r = p.pcb.courtyard(c);
        Rect label{r.x0 - 3, r.y0 - 2.5, r.x1 + 3, r.y0};
        CHECK(anyIn(silk, label, 0.0, 0.1));
    }
    // Glass only on LEDs, ceramic only on chip passives.
    for (const auto& c : s.components()) {
        if (!c.hasFootprint() || !c.pcb.placed) continue;
        Rect r = p.pcb.courtyard(c);
        const std::string fp = c.def().footprint;
        const bool chip = fp.rfind("R_", 0) == 0 || fp.rfind("C_", 0) == 0;
        if (c.kind == ComponentKind::LED) {
            ++leds;
            CHECK(anyIn(glass, r, 0.0, 10.0));
        }
        if (chip) {
            ++chips;
            CHECK(anyIn(ceramic, r, 0.0, 10.0) && !anyIn(glass, r, 0.0, 10.0));
        }
    }
    CHECK(socs == 1 && leds == 1 && chips >= 6);
    for (const auto& g : glass) CHECK(g.y >= 0);  // no LED glass inside or under the board here

    // Vias: a dark bore of the drill's radius through the board and plated lands on both outer layers.
    p.pcb.vias.push_back({});
    Via& v = p.pcb.vias.back();
    v.position = {1.5, 1.5};
    v.drill = 0.3;
    v.diameter = 0.6;
    Mesh withVia = buildAssemblyMesh(s, p.pcb);
    double boreR = 0, boreTop = -1e9, boreBottom = 1e9;
    for (const auto& h : surfacePoints(withVia, Surface::Hole)) {
        double r = std::hypot(h.x - 1.5, h.z - 1.5);
        if (r > 0.4) continue;
        boreR = std::max(boreR, r);
        boreTop = std::max(boreTop, h.y);
        boreBottom = std::min(boreBottom, h.y);
    }
    CHECK(std::fabs(boreR - 0.15) < 1e-3);
    CHECK(boreTop > 0 && boreBottom < -t);
    const auto finish = surfacePoints(withVia, Surface::Finish);
    CHECK(anyIn(finish, Rect::centered({1.5, 1.5}, 0.62, 0.62), -0.001, 0.04));
    CHECK(anyIn(finish, Rect::centered({1.5, 1.5}, 0.62, 0.62), -t - 0.04, -t + 0.001));
}

TEST(realistic_assembly_bottom_side) {
    // A part flipped to the bottom: its body, solder and mirrored legend hang under the board.
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    int flipped = -1;
    for (auto& c : p.schematic.mutableComponents())
        if (flipped < 0 && c.kind == ComponentKind::Resistor && c.pcb.placed) {
            c.pcb.bottom = true;
            flipped = c.id;
        }
    CHECK(flipped >= 0);
    const auto& s = p.schematic;
    const double t = p.pcb.settings.thickness;
    Mesh m = buildAssemblyMesh(s, p.pcb);
    Rect r = p.pcb.courtyard(*s.find(flipped));
    CHECK(anyIn(surfacePoints(m, Surface::Ceramic), r, -t - 5, -t));
    CHECK(!anyIn(surfacePoints(m, Surface::Ceramic), r, 0.0, 5.0));
    CHECK(anyIn(surfacePoints(m, Surface::Solder), r.inflated(0.5), -t - 0.3, -t));
    Rect label{r.x0 - 3, r.y0 - 2.5, r.x1 + 3, r.y0};
    CHECK(anyIn(surfacePoints(m, Surface::Silk), label, -t - 0.1, -t));
}

TEST(realistic_assembly_mesh_stays_bounded) {
    // Big boards: the extra detail (legends, leads, joints) costs a bounded number of vertices per part and the mesh
    // builds quickly.
    Project p;
    auto& s = p.schematic;
    int prev = -1;
    for (int k = 0; k < 120; ++k) {
        int r = s.addComponent(ComponentKind::Resistor, "10k", {k * 30.0, 0});
        if (prev >= 0) wire(s, prev, "2", r, "1");
        prev = r;
    }
    p.pcb.settings.width = 160;
    p.pcb.settings.height = 120;
    p.pcb.autoPlace(s, true);
    size_t placed = 0;
    for (const auto& c : s.components()) placed += c.pcb.placed ? 1 : 0;
    CHECK(placed == 120);
    auto start = std::chrono::steady_clock::now();
    Mesh m = buildAssemblyMesh(s, p.pcb);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(seconds < 5.0);
    CHECK(m.vertexCount() < placed * 3000);
    CHECK(m.surfaces.size() == m.vertexCount() && m.colors.size() == m.vertexCount() * 4);
    // Exports still carry the realistic colours.
    CHECK(exportObj(m, "big").find("\nv ") != std::string::npos);
}

TEST(complex_design_flow_is_fast) {
    // Productivity: a dense 40-channel signal-conditioning board (40 op-amp stages, each with gain resistors, a
    // filter capacitor and an output connector pin, all on shared VCC / GND) goes from schematic to a placed,
    // routed, verified and meshed board in seconds.
    Project p;
    auto& s = p.schematic;
    int j = s.addComponent(ComponentKind::Connector, "PWR", {0, 0});
    int vcc = s.addComponent(ComponentKind::NetLabel, "VCC", {-40, -10});
    int gnd = s.addComponent(ComponentKind::Ground, "", {-40, 30});
    wire(s, j, "1", vcc, "N");
    wire(s, j, "2", gnd, "GND");
    constexpr int kStages = 40;
    for (int k = 0; k < kStages; ++k) {
        const double x = 100.0 + k * 120.0;
        int u = s.addComponent(ComponentKind::OpAmp, "LM358", {x, 0});
        int rin = s.addComponent(ComponentKind::Resistor, "10k", {x - 60, 10});
        int rf = s.addComponent(ComponentKind::Resistor, "47k", {x, -60});
        int rg = s.addComponent(ComponentKind::Resistor, "10k", {x - 40, -60});
        int cf = s.addComponent(ComponentKind::Capacitor, "1n", {x + 40, -60});
        int lv = s.addComponent(ComponentKind::NetLabel, "VCC", {x - 90, 10});
        int g = s.addComponent(ComponentKind::Ground, "", {x - 40, 60});
        int out = s.addComponent(ComponentKind::NetLabel, "OUT" + std::to_string(k), {x + 60, 0});
        wire(s, lv, "N", rin, "1");
        wire(s, rin, "2", u, "IN+");
        wire(s, u, "IN-", rg, "2");
        wire(s, rg, "1", g, "GND");
        wire(s, u, "IN-", rf, "1");
        wire(s, rf, "2", u, "OUT");
        wire(s, cf, "1", u, "IN-");
        wire(s, cf, "2", u, "OUT");
        wire(s, u, "OUT", out, "N");
    }
    size_t parts = 0;
    for (const auto& c : s.components()) parts += c.hasFootprint() ? 1 : 0;
    CHECK(parts == 1 + kStages * 5);  // the connector, then an op-amp, three resistors and a capacitor per stage

    using clock = std::chrono::steady_clock;
    auto secs = [](clock::time_point a) { return std::chrono::duration<double>(clock::now() - a).count(); };
    auto t0 = clock::now();
    p.pcb.autoPlace(s, true);
    p.pcb.fitBoardToComponents(s, 2.5);
    const double place = secs(t0);
    t0 = clock::now();
    const auto stats = p.pcb.autoRoute(s);
    const double route = secs(t0);
    t0 = clock::now();
    const auto report = verifyDesign(p);
    const double verify = secs(t0);
    t0 = clock::now();
    Mesh m = buildAssemblyMesh(s, p.pcb);
    const double mesh = secs(t0);
    std::printf("    flow: %zu parts, %d connections — place %.2f s, route %.2f s, verify %.2f s, 3D %.2f s\n", parts,
                stats.connections, place, route, verify, mesh);
    CHECK(stats.failed == 0);
    for (const auto& c : s.components()) CHECK(!c.hasFootprint() || c.pcb.placed);
    CHECK(m.vertexCount() > 0);
    CHECK(report.stages.size() == 8);
    // Generous budgets (sanitiser and debug builds run here too): a regression to the old quadratic scans or an
    // unoptimised core shows up as minutes.
    CHECK(place < 20.0);
    CHECK(route < 60.0);
    CHECK(verify < 20.0);
    CHECK(mesh < 5.0);
}

TEST(production_parts_catalog) {
    // The robotics / automotive / industrial catalog: every orderable part number is in the library on its real
    // package, with datasheet pin names, and registers as a placeable part.
    const char* catalog[] = {
        "ATMEGA328P-AU", "ATMEGA328P-PU", "STM32F103C8T6", "STM32F405RGT6", "ESP32-WROOM-32E", "RP2040", "L293DD", "L293D",
        "L298HN", "ULN2003ADR", "ULN2003A", "PCA9685PW", "IR2101S", "IR2110S", "TMC2209-LA", "MPU-6050", "MPU-9250",
        "ADS1115IDGS", "LM358DR", "LM358N", "LM393DR", "LM393N", "MAX9814ETD+T", "MAX4466EXK+T", "74HC595D", "74HC595",
        "PCF8574TS", "PCF8574N", "CH340G", "CP2102N-A02-GQFN28", "MAX485ESA+T", "SP485EEN-L", "TJA1050", "MCP2551-I/SN",
        "LM7805", "LM7812", "AMS1117-3.3", "LM2596S-5.0", "XL4015E1", "TP4056", "LM74700QDBVRQ1", "PC817X3NSZ0F",
        "TLP281-4", "STM32H743IIT6", "STM32F765VIT6", "LPC1768", "XC7A35T-1CSG324I", "CSD18540Q5B",
        "TMC2160-TA", "TMC5160A-TA", "AS5047D-ATSM", "INA240A1EDRQ1", "TCAN1042VDRQ1", "ADM2587EBRWZ",
        "LTC4359IMS8#PBF", "BMI160", "ICS-43434", "ATSAMD51J20A-AU", "MT48LC16M16A2TG-6A", "W9812G6KH-6", "IS42S16400J-7TL",
        "MT41K256M16HA-125", "AS4C256M16D3-12BCN", "MT40A512M16LY-062E",
    };
    for (const char* name : catalog) {
        const StandardPart* sp = findStandardPart(name);
        CHECK(sp != nullptr);
        if (!sp) {
            std::printf("    missing %s\n", name);
            continue;
        }
        bool ok = true;
        try {
            auto part = CustomPartRegistry::instance().registerPart(sp->spec);
            size_t connected = 0;
            for (const auto& pad : part->footprint.pads) connected += pad.pinIndex >= 0 ? 1 : 0;
            ok = !part->def.pins.empty() && part->footprint.pads.size() >= sp->spec.pins.size() - 1 && connected > 0;
        } catch (const std::exception& e) {
            std::printf("    %s: %s\n", name, e.what());
            ok = false;
        }
        CHECK(ok);
    }
    // The catalog's flyback diodes are diode values with their own simulation models (Schottky / rectifier).
    CHECK(findDeviceModel(ComponentKind::Diode, "1N5819") != nullptr);
    CHECK(findDeviceModel(ComponentKind::Diode, "1N4007") != nullptr);
    auto reg = [](const char* name) { return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec); };
    auto pinName = [](const std::shared_ptr<const CustomPart>& part, const PadDef& pad) {
        return pad.pinIndex >= 0 ? part->def.pins[static_cast<size_t>(pad.pinIndex)].name : std::string("-");
    };

    // Datasheet pin names from the generated catalog.
    CHECK(reg("LM358DR")->def.pins[7].name == "V+" || reg("LM358DR")->def.pins[7].number == "8");
    CHECK(reg("MAX485ESA+T")->def.pins[5].name == "A" && reg("MAX485ESA+T")->def.pins[6].name == "B");
    CHECK(reg("L298HN")->def.pins[3].name == "Vs");
    CHECK(reg("TCAN1042VDRQ1")->def.pins[4].name == "VIO");

    // FPGA: 324 balls in an 18 × 18 grid at 0.8 mm, named A1…V18 (no I / O / Q / S rows), pins on their balls.
    auto fpga = reg("XC7A35T-1CSG324I");
    CHECK(fpga->footprint.pads.size() == 324);
    CHECK(fpga->footprint.pads.front().round);
    CHECK(std::fabs(fpga->footprint.pads[1].offset.x - fpga->footprint.pads[0].offset.x - 0.8) < 1e-9);
    size_t mapped = 0;
    for (const auto& pad : fpga->footprint.pads) mapped += pad.pinIndex >= 0 ? 1 : 0;
    CHECK(mapped == 324);
    bool hasV18 = false, hasI = false;
    for (const auto& pin : fpga->def.pins) {
        hasV18 |= pin.number == "V18";
        hasI |= pin.number[0] == 'I' || pin.number[0] == 'O';
    }
    CHECK(hasV18 && !hasI);

    // D²PAK buck: five leads plus the tab soldered to pin 3 (GND); SOT-223 LDO: tab on pin 2 (VO).
    auto buck = reg("LM2596S-5.0");
    CHECK(buck->footprint.pads.size() == 6);
    CHECK(pinName(buck, buck->footprint.pads.back()) == "GND");
    CHECK(buck->footprint.pads.back().size.x > 9);
    auto ldo = reg("AMS1117-3.3");
    CHECK(ldo->footprint.pads.size() == 4 && pinName(ldo, ldo->footprint.pads.back()) == "VO");

    // Multiwatt-15: through-hole, staggered rows.
    auto l298 = reg("L298HN");
    CHECK(l298->footprint.pads.size() == 15 && l298->footprint.pads[0].throughHole);
    CHECK(l298->footprint.pads[0].offset.y != l298->footprint.pads[1].offset.y);

    // Power MOSFET SON: the drain tab joins the drain pins; TQFP-48 drivers get their exposed pad.
    auto fet = reg("CSD18540Q5B");
    CHECK(fet->footprint.pads.size() == 9 && pinName(fet, fet->footprint.pads.back()) == "D");
    CHECK(reg("TMC2160-TA")->footprint.pads.size() == 49);
    CHECK(reg("TMC2209-LA")->footprint.pads.size() == 29);
    // Wide-body SOIC and SC70 / SOT-23-5 packages.
    CHECK(reg("ADM2587EBRWZ")->footprint.body.width > 7);
    CHECK(reg("MAX4466EXK+T")->footprint.pads.size() == 5);
    // Linear regulators simulate.
    CHECK(findStandardPart("LM7812")->spec.model.hasRegulator && findStandardPart("AMS1117-3.3")->spec.model.hasRegulator);
}

TEST(passive_package_variants) {
    // Resistors, capacitors, inductors, diodes and LEDs can be fitted in the package the BOM calls for: chip sizes,
    // through-hole, tantalum and electrolytic capacitors, SMA / DO-41 diodes.
    Project p = amplifierProject();
    auto& s = p.schematic;
    int r = -1, c = -1, d = -1;
    for (const auto& comp : s.components()) {
        if (r < 0 && comp.kind == ComponentKind::Resistor) r = comp.id;
        if (c < 0 && comp.kind == ComponentKind::Capacitor) c = comp.id;
    }
    d = s.addComponent(ComponentKind::Diode, "1N4007", {500, 0});
    CHECK(r >= 0 && c >= 0 && d >= 0);
    CHECK(Library::packageVariants(ComponentKind::Resistor).size() == 5);
    CHECK(Library::packageVariants(ComponentKind::OpAmp).empty());
    for (ComponentKind k : {ComponentKind::Resistor, ComponentKind::Capacitor, ComponentKind::Inductor, ComponentKind::Diode,
                            ComponentKind::LED})
        for (const auto& v : Library::packageVariants(k)) CHECK(Library::instance().footprint(v) != nullptr);
    CHECK(Library::packageLabel("CP_Tant_B") == "Tantalum B (3528)");

    // Default footprint until a variant is chosen; invalid variants are refused.
    CHECK(s.find(r)->footprintName() == "R_0805" && s.find(r)->package.empty());
    CHECK(!s.setPackage(r, "D_SMA") && !s.setPackage(r, "nonsense"));
    CHECK(s.setPackage(r, "R_0603") && s.find(r)->footprintName() == "R_0603");
    CHECK(s.setPackage(c, "CP_Tant_B") && s.setPackage(d, "D_DO41_THT"));
    p.pcb.autoPlace(s, true);
    const auto pads = p.pcb.pads(s);
    for (const auto& pad : pads) {
        if (pad.componentId == r) CHECK(std::fabs(pad.size.x - 0.9) < 1e-9 || std::fabs(pad.size.y - 0.9) < 1e-9);
        if (pad.componentId == d) CHECK(pad.throughHole && pad.drill > 0.9);
    }
    // BOM, export, save / load and the snapshot carry the package.
    Json snap = p.snapshot();
    bool sawOptions = false;
    for (const auto& j : snap.get("components").items()) {
        if (j.get("id").asInt() == c) {
            CHECK(j.get("footprint").asString() == "CP_Tant_B" && j.get("package").asString() == "CP_Tant_B");
            CHECK(j.get("packageOptions").items().size() == 8);
            sawOptions = true;
        }
    }
    CHECK(sawOptions);
    CHECK(exportBomCsv(s).find("Tantalum B") != std::string::npos);
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.schematic.find(r)->footprintName() == "R_0603" && q.schematic.find(d)->footprintName() == "D_DO41_THT");
    // Back to the default clears the variant.
    CHECK(s.setPackage(r, "R_0805") && s.find(r)->package.empty());
    // 3D: the tantalum is a moulded body, the axial diode a through-hole part with leads under the board.
    Mesh m = buildAssemblyMesh(s, p.pcb);
    Rect dr = p.pcb.courtyard(*s.find(d));
    CHECK(anyIn(surfacePoints(m, Surface::Hole), dr, -p.pcb.settings.thickness - 0.1, 0.1));

    // Crystals in the catalog's packages.
    auto reg = [](const char* name) { return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec); };
    CHECK(reg("Crystal_16MHz_3225")->footprint.pads.size() == 4);
    CHECK(reg("Crystal_32.768kHz_3215")->footprint.pads.size() == 2 && !reg("Crystal_32.768kHz_3215")->footprint.pads[0].throughHole);
    CHECK(reg("Crystal_32.768kHz_Cylinder")->footprint.pads[0].throughHole);
    CHECK(reg("Crystal_16MHz")->footprint.pads[0].throughHole);

    // C API.
    SiedaProject* api = sieda_project_new("pkg");
    int32_t rr = sieda_add_component(api, static_cast<int32_t>(ComponentKind::Resistor), "10k", 0, 0, 0, "R1");
    CHECK(sieda_set_component_package(api, rr, "R_Axial_THT") == 1);
    CHECK(sieda_set_component_package(api, rr, "C_0603") == 0);
    sieda_project_free(api);
}

TEST(c_api_smoke) {
    int rc = sieda_c_api_smoke_test();
    if (rc != 0) std::printf("    c api step %d failed\n", rc);
    CHECK(rc == 0);
}

int main() {
    for (const auto& t : registry()) {
        int before = g_failures;
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    }
    std::printf("\n%d checks, %d failures, %zu tests\n", g_checks, g_failures, registry().size());
    return g_failures == 0 ? 0 : 1;
}

TEST(ina333_instrumentation_amplifier) {
    // ECG front end: 3.3 V, VREF = 1.65 V, a floating 1 mV electrode signal through 10 kΩ protection resistors with
    // 1 MΩ bias returns to VREF, RG = 1 kΩ → G = 101.
    Project p;
    auto& s = p.schematic;
    std::string id = p.addCustomPart(findStandardPart("INA333")->spec);
    int vs = s.addComponent(ComponentKind::VoltageSource, "3.3", {0, 0});
    int gnd = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int r3 = s.addComponent(ComponentKind::Resistor, "100k", {100, 0});
    int r4 = s.addComponent(ComponentKind::Resistor, "100k", {100, 100});
    int sig = s.addComponent(ComponentKind::VoltageSource, "0.001", {0, 300});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, 250});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {100, 350});
    int r5 = s.addComponent(ComponentKind::Resistor, "1M", {200, 250});
    int r6 = s.addComponent(ComponentKind::Resistor, "1M", {200, 350});
    int u = s.addCustomComponent(id, "INA333", {300, 300});
    int rg = s.addComponent(ComponentKind::Resistor, "1k", {300, 200});
    int load = s.addComponent(ComponentKind::Resistor, "10k", {400, 300});
    wire(s, vs, "-", gnd, "GND");
    wire(s, vs, "+", r3, "1");
    wire(s, r3, "2", r4, "1");
    wire(s, r4, "2", gnd, "GND");
    wire(s, sig, "+", r1, "1");
    wire(s, r1, "2", u, "+IN");
    wire(s, sig, "-", r2, "1");
    wire(s, r2, "2", u, "-IN");
    wire(s, u, "+IN", r5, "1");
    wire(s, r5, "2", r3, "2");
    wire(s, u, "-IN", r6, "1");
    wire(s, r6, "2", r3, "2");
    wire(s, u, "1", rg, "1");
    wire(s, rg, "2", u, "8");
    wire(s, u, "V+", vs, "+");
    wire(s, u, "V-", gnd, "GND");
    wire(s, u, "REF", r3, "2");
    wire(s, u, "VOUT", load, "1");
    wire(s, load, "2", gnd, "GND");
    p.schematicChanged();

    auto out = [&] {
        DcResult r = Simulator(s).dcOperatingPoint();
        CHECK(r.converged);
        return r.converged ? netV(s, r, u, "VOUT") - netV(s, r, u, "REF") : NAN;
    };
    double vdiff = 0.001 * 2e6 / 2.02e6;  // divided by the protection resistors
    double g101 = out();
    CHECK(std::fabs(g101 - 101 * vdiff) < 1e-3);

    // Two RG resistors in parallel (500 Ω) → G = 201.
    int rg2 = s.addComponent(ComponentKind::Resistor, "1k", {300, 150});
    wire(s, rg2, "1", u, "1");
    wire(s, rg2, "2", u, "8");
    p.schematicChanged();
    CHECK(std::fabs(out() - 201 * vdiff) < 2e-3);

    // RG open → unity gain.
    s.removeComponent(rg);
    s.removeComponent(rg2);
    p.schematicChanged();
    CHECK(std::fabs(out() - vdiff) < 1e-4);

    // A large input saturates just inside the rails instead of following REF + G·Vd.
    int rgBig = s.addComponent(ComponentKind::Resistor, "100", {300, 200});
    wire(s, rgBig, "1", u, "1");
    wire(s, rgBig, "2", u, "8");
    s.setValue(sig, "0.1");
    p.schematicChanged();
    DcResult sat = Simulator(s).dcOperatingPoint();
    CHECK(sat.converged);
    double vo = netV(s, sat, u, "VOUT");
    CHECK(vo > 3.2 && vo < 3.3);
    s.setValue(sig, "-0.1");
    p.schematicChanged();
    DcResult neg = Simulator(s).dcOperatingPoint();
    CHECK(neg.converged && netV(s, neg, u, "VOUT") < 0.1 && netV(s, neg, u, "VOUT") > 0);
}

TEST(project_reset_returns_to_a_blank_project) {
    auto save = [](SiedaProject* p) {
        char* j = sieda_project_save_json(p);
        std::string s = j ? j : "";
        sieda_string_free(j);
        return s;
    };
    SiedaProject* blank = sieda_project_new(nullptr);
    std::string fresh = save(blank);
    sieda_project_free(blank);

    SiedaProject* p = sieda_project_new("Old design");
    char* err = nullptr;
    char* all = sieda_standard_parts_json();
    Json parts = Json::parse(std::string(all));
    sieda_string_free(all);
    std::string specJson;
    for (const auto& item : parts.items())
        if (item.get("spec").get("name").asString() == "NE555") specJson = item.get("spec").dump();
    char* partJson = sieda_custom_part_register(p, specJson.c_str(), &err);
    CHECK(partJson != nullptr);
    sieda_string_free(partJson);
    sieda_add_component(p, static_cast<int32_t>(ComponentKind::Resistor), "1k", 0, 0, 0, "R1");
    sieda_project_set_requirements(p, "a PRD");
    CHECK(sieda_project_set_industry(p, "medical") == 1);
    sieda_pcb_set_board(p, 77, 55, 0.4, 0.3);
    CHECK(sieda_pcb_add_mounting_hole(p, 3, 3, 3.2, 1) >= 0);
    CHECK(save(p) != fresh);

    sieda_project_clear(p);  // keeps the library and board (used when a plan rebuilds the schematic)
    CHECK(save(p).find("NE555") != std::string::npos);
    sieda_project_reset(p);
    CHECK(save(p) == fresh);
    sieda_project_free(p);
}

TEST(microcontroller_library_by_vendor) {
    // Ten microcontrollers per vendor group, each on its real package: lead count, pitch, body and exposed pad.
    std::map<std::string, int> perGroup;
    for (const auto& p : standardParts())
        if (p.category.rfind("Microcontrollers · ", 0) == 0) ++perGroup[p.category];
    CHECK(perGroup["Microcontrollers · Arm"] == 11);  // + the catalog's LPC1769 (CNC / 3D-printer boards)
    // + the catalog's STM32F405 / H743 / F765 and the robotics spares STM32F446 / G474 / H723.
    CHECK(perGroup["Microcontrollers · STMicroelectronics"] == 16);
    CHECK(perGroup["Microcontrollers · Texas Instruments"] == 10);
    // + ATmega328P, ATtiny85, the rad-tolerant ATmegaS128 and the catalog's ATMEGA328P-AU / -PU and SAM D51.
    CHECK(perGroup["Microcontrollers · Microchip"] == 16);
    for (const auto& p : standardParts()) {
        if (p.category.rfind("Microcontrollers · ", 0) != 0) continue;
        bool ok = true;
        try {
            auto part = CustomPartRegistry::instance().registerPart(p.spec);
            ok = part->footprint.pads.size() >= static_cast<size_t>(p.spec.package.pinCount) && !part->def.pins.empty();
        } catch (const std::exception&) {
            ok = false;
        }
        CHECK(ok);
    }

    struct Expect {
        const char* name;
        int leads;
        double pitch;   // centre-to-centre of pads 1 and 2
        bool exposedPad;
        const char* pin1;
    };
    const Expect expect[] = {
        {"RP2040", 56, 0.4, true, "IOVDD"},           {"nRF52832", 48, 0.4, true, "DEC1"},
        {"STM32F103C8T6", 48, 0.5, false, "VBAT"},    {"STM32F030F4P6", 20, 0.65, false, "BOOT0"},
        {"STM32F042K6T6", 32, 0.8, false, "VDD"},     {"STM32L432KCU6", 32, 0.5, true, "VDD"},
        {"ATmega32U4", 44, 0.8, false, "PE6"},        {"ATmega2560", 100, 0.5, false, "PG5"},
        {"ATtiny1614", 14, 1.27, false, "VCC"},       {"PIC16F877A", 40, 2.54, false, "nMCLR/Vpp"},
        {"MSP430G2553", 20, 2.54, false, "DVCC"},     {"MSP432E401Y", 128, 0.4, false, nullptr},
        {"EFM32HG308F64", 24, 0.65, true, nullptr},   {"LPC1768", 100, 0.5, false, nullptr},
    };
    for (const auto& e : expect) {
        const StandardPart* sp = findStandardPart(e.name);
        CHECK(sp != nullptr);
        if (!sp) continue;
        auto part = CustomPartRegistry::instance().registerPart(sp->spec);
        const auto& pads = part->footprint.pads;
        CHECK(static_cast<int>(pads.size()) == e.leads + (e.exposedPad ? 1 : 0));
        double pitch = std::hypot(pads[1].offset.x - pads[0].offset.x, pads[1].offset.y - pads[0].offset.y);
        CHECK(std::fabs(pitch - e.pitch) < 1e-6);
        // Every lead pad is wired to its pin; the exposed pad (if any) to the last listed pin.
        for (int i = 0; i < e.leads; ++i) CHECK(pads[static_cast<size_t>(i)].pinIndex == i);
        if (e.exposedPad) CHECK(pads.back().pinIndex == e.leads && pads.back().size.x > 1.5);
        if (e.pin1) CHECK(sp->spec.pins[0].name == e.pin1);
        // Neighbouring pads never overlap (fine-pitch packages included).
        CHECK(pads[0].size.y < e.pitch || pads[0].size.x < e.pitch);
    }
    // Real body sizes: LQFP-100 14 × 14 mm, QFN-56 7 × 7 mm.
    auto body = [](const char* n) { return CustomPartRegistry::instance().registerPart(findStandardPart(n)->spec)->footprint.body; };
    CHECK(std::fabs(body("STM32F407VGT6").width - 14) < 1e-9);
    CHECK(std::fabs(body("RP2040").width - 7) < 1e-9);
    // Pitch and body survive the JSON round trip, and parts without them keep their ids.
    CustomPartSpec spec = findStandardPart("RP2040")->spec;
    CustomPartSpec back = customPartSpecFromJson(customPartSpecToJson(spec));
    CHECK(back.package.pitch == 0.4 && back.package.bodySize == 7);
    CHECK(customPartSpecToJson(findStandardPart("NE555")->spec).get("package").get("pitch").isNull());
}

TEST(battery_and_ac_sources) {
    // A 9 V battery lighting a 1 kΩ load, and a 12 V rms / 50 Hz AC source into another.
    Project p;
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "", {0, 0});
    int ac = s.addComponent(ComponentKind::ACSource, "", {0, 200});
    int r1 = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {100, 200});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    CHECK(s.find(bt)->value == "9" && s.find(bt)->ref.rfind("BT", 0) == 0);
    CHECK(s.find(ac)->value == "SIN(0 17 50)" && s.find(ac)->ref.rfind("VAC", 0) == 0);
    wire(s, bt, "+", r1, "1");
    wire(s, r1, "2", g, "GND");
    wire(s, bt, "-", g, "GND");
    wire(s, ac, "+", r2, "1");
    wire(s, r2, "2", g, "GND");
    wire(s, ac, "-", g, "GND");
    p.schematicChanged();

    DcResult dc = Simulator(s).dcOperatingPoint();
    CHECK(dc.converged);
    CHECK(std::fabs(netV(s, dc, r1, "1") - 9.0) < 1e-6);
    TransientResult tr = Simulator(s).transient(0.04, 1e-4);  // two 50 Hz cycles
    CHECK(tr.ok);
    const auto& wave = tr.netVoltages[static_cast<size_t>(s.netOf({r2, 0}))];
    double hi = *std::max_element(wave.begin(), wave.end()), lo = *std::min_element(wave.begin(), wave.end());
    CHECK(hi > 16.5 && lo < -16.5);  // ±17 V peak

    // Both export as SPICE voltage sources (named V…), and a shorted battery is caught by ERC.
    std::string spice = exportSpiceNetlist(s, "sources");
    CHECK(spice.find("V" + s.find(bt)->ref + " ") != std::string::npos);
    CHECK(spice.find(s.find(ac)->ref + " ") != std::string::npos && spice.find("SIN(0 17 50)") != std::string::npos);
    int shorted = s.addComponent(ComponentKind::Battery, "3", {300, 0});
    wire(s, shorted, "+", g, "GND");
    wire(s, shorted, "-", g, "GND");
    bool shortFound = false;
    for (const auto& v : s.runERC())
        if (v.code == "ERC_SHORTED_SOURCE") shortFound = true;
    CHECK(shortFound);
}

TEST(wire_junctions) {
    // The schematic a user draws: 9 V battery → 10 kΩ → red LED, then a capacitor added in parallel with the LED by
    // dropping its pins onto the existing wires (T-junctions), not onto other pins.
    Project p;
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "9", {0, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, -60});
    int d1 = s.addComponent(ComponentKind::LED, "Red", {300, 0}, 90);
    int c1 = s.addComponent(ComponentKind::Capacitor, "100n", {200, 0}, 90);
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    wire(s, bt, "+", r1, "1");
    int top = s.connect({r1, 1}, {d1, 0});
    int bottom = s.connect({d1, 1}, {bt, 1});
    wire(s, bt, "-", g, "GND");
    CHECK(top >= 0 && bottom >= 0);

    int jt = s.splitWire(top, {200, -60});
    int jb = s.splitWire(bottom, {200, 60});
    CHECK(jt >= 0 && jb >= 0 && jt != jb);
    CHECK(s.find(jt)->kind == ComponentKind::Junction && s.find(jt)->ref[0] == '#');
    CHECK(s.connect({c1, 0}, {jt, 0}) >= 0);
    CHECK(s.connect({c1, 1}, {jb, 0}) >= 0);
    CHECK(s.wireCount(jt) == 3 && s.wireCount(jb) == 3);
    // Splitting at an existing junction end reuses it.
    CHECK(s.splitWire(s.wires().back().id, {200, 60}) == jb);

    // C1 is in parallel with D1; junctions join nets but are not net members.
    CHECK(s.netOf({c1, 0}) == s.netOf({d1, 0}) && s.netOf({c1, 0}) == s.netOf({r1, 1}));
    CHECK(s.netOf({c1, 1}) == s.netOf({d1, 1}) && s.netOf({c1, 1}) == s.groundNet());
    CHECK(s.netOf({jt, 0}) == s.netOf({c1, 0}));
    for (const auto& n : s.nets())
        for (const auto& pin : n.pins) CHECK(s.find(pin.component)->kind != ComponentKind::Junction);
    for (const auto& v : s.runERC()) {
        CHECK(v.code != "ERC_UNCONNECTED_PIN" && v.code != "ERC_DANGLING_WIRE" && v.code != "ERC_DUPLICATE_REF");
    }
    p.schematicChanged();
    DcResult dc = Simulator(s).dcOperatingPoint();
    CHECK(dc.converged);
    double vled = netV(s, dc, d1, "A") - netV(s, dc, d1, "K");
    CHECK(std::fabs(netV(s, dc, c1, "1") - netV(s, dc, c1, "2") - vled) < 1e-6 && vled > 1.5 && vled < 2.2);
    CHECK(exportSpiceNetlist(s, "j").find(s.find(jt)->ref) == std::string::npos);
    CHECK(exportBomCsv(s).find("Junction") == std::string::npos);

    // Round trip through the project file.
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.schematic.find(jt) && q.schematic.find(jt)->kind == ComponentKind::Junction);
    CHECK(q.schematic.netOf({c1, 0}) == q.schematic.netOf({d1, 0}));

    // A bend point: move it anywhere, delete it and the wire straightens (connection kept).
    int r1d1 = -1;
    for (const auto& w : s.wires())
        if ((w.a.component == jt && w.b.component == d1) || (w.b.component == jt && w.a.component == d1)) r1d1 = w.id;
    int bend = s.splitWire(r1d1, {250, -60});
    CHECK(s.moveComponent(bend, {250, -100}));
    CHECK(s.netOf({bend, 0}) == s.netOf({d1, 0}));
    CHECK(s.removeComponent(bend));
    CHECK(s.netOf({d1, 0}) == s.netOf({c1, 0}));

    // Deleting C1's wire leaves its junction as a bend point; deleting the last wires removes the junction.
    int cw = -1;
    for (const auto& w : s.wires())
        if (w.a.component == c1 && w.a.pin == 0) cw = w.id;
    CHECK(s.removeWire(cw));
    CHECK(s.find(jt) && s.wireCount(jt) == 2);

    // A wire abandoned in empty space is a dangling junction chain: reported, then cleaned up.
    int j1 = s.addComponent(ComponentKind::Junction, "", {200, -150});
    int j2 = s.addComponent(ComponentKind::Junction, "", {260, -150});
    s.connect({c1, 0}, {j1, 0});
    s.connect({j1, 0}, {j2, 0});
    bool dangling = false;
    for (const auto& v : s.runERC())
        if (v.code == "ERC_DANGLING_WIRE") dangling = true;
    CHECK(dangling);
    CHECK(s.netOf({c1, 0}) >= 0 && s.nets()[static_cast<size_t>(s.netOf({c1, 0}))].pins.size() == 1);  // still open
    CHECK(s.removeDanglingJunctions(j2) == 2);
    CHECK(!s.find(j1) && !s.find(j2) && s.wireCount(c1) == 1);
}

TEST(solder_mask_colours) {
    // Green is the default (like most boards); the other fab colours change the 3D board and are saved.
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.settings.solderMask == "green");
    CHECK(solderMaskStyles().size() == 7 && std::string(solderMaskStyles().front().name) == "green");
    auto topColour = [&]() {
        Mesh m = buildAssemblyMesh(p.schematic, p.pcb, {false, false, false});
        // The first box is the board core; find a vertex on its top face (normal +Y).
        for (size_t v = 0; v < m.vertexCount(); ++v)
            if (m.normals[v * 3 + 1] > 0.99f) return Rgba{m.colors[v * 4], m.colors[v * 4 + 1], m.colors[v * 4 + 2], 1};
        return Rgba{};
    };
    Rgba green = topColour();
    CHECK(green.g > green.r * 3 && green.g > green.b);
    for (const char* name : {"black", "blue", "red", "yellow", "white", "purple"}) {
        CHECK(findSolderMask(name) != nullptr);
        p.pcb.settings.solderMask = name;
        Rgba c = topColour();
        CHECK(c.r == findSolderMask(name)->mask.r && c.g == findSolderMask(name)->mask.g);
    }
    CHECK(findSolderMask("yellow")->silk.r < 0.1f && findSolderMask("white")->silk.r < 0.1f);  // black legend
    CHECK(findSolderMask("red")->silk.r > 0.9f);
    CHECK(!findSolderMask("orange"));

    p.pcb.settings.solderMask = "purple";
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.solderMask == "purple");
    Json old = p.toJson();
    old["board"]["solderMask"] = std::string("tartan");
    CHECK(Project::fromJson(old).pcb.settings.solderMask == "green");
}

TEST(assembly_mesh_follows_layer_count) {
    // The 3D board shows the stack-up picked in PCB Layout: a single-sided board has a bare FR-4 underside, two layers
    // have solder mask on both sides, and 4/6-layer boards show their inner copper as bands on the board edge.
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    const Rgba mask = findSolderMask("green")->mask;
    auto count = [](const Mesh& m, auto pred) {
        int n = 0;
        for (size_t v = 0; v < m.vertexCount(); ++v)
            if (pred(Rgba{m.colors[v * 4], m.colors[v * 4 + 1], m.colors[v * 4 + 2], 1}, m.normals[v * 3 + 1],
                     m.positions[v * 3 + 1]))
                ++n;
        return n;
    };
    auto same = [](Rgba a, Rgba b) { return std::fabs(a.r - b.r) < 1e-4 && std::fabs(a.g - b.g) < 1e-4 && std::fabs(a.b - b.b) < 1e-4; };
    const Rgba innerCopper{0.80f, 0.52f, 0.28f, 1};
    const double t = p.pcb.settings.thickness;
    for (int layers : {1, 2, 4, 6}) {
        p.pcb.settings.layerCount = layers;
        Mesh m = buildAssemblyMesh(p.schematic, p.pcb, {false, false, false});
        CHECK(m.normals[1] > 0.99f && same({m.colors[0], m.colors[1], m.colors[2], 1}, mask));  // green top
        int maskUnderside = count(m, [&](Rgba c, float ny, float py) { return ny < -0.99f && std::fabs(py + t) < 1e-3 && same(c, mask); });
        int bareUnderside = count(m, [&](Rgba c, float ny, float py) { return ny < -0.99f && std::fabs(py + t) < 1e-3 && !same(c, mask); });
        int bands = count(m, [&](Rgba c, float ny, float) { return ny > 0.99f && same(c, innerCopper); }) / 4;  // 4 vertices per top quad
        CHECK((layers == 1) == (maskUnderside == 0));
        CHECK((layers == 1) == (bareUnderside > 0));
        CHECK(bands == std::max(0, layers - 2));
    }
}

TEST(fabrication_package_is_complete) {
    Project p = amplifierProject();
    p.name = "Amp: rev A";
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.solderMask = "black";
    p.pcb.settings.holes.push_back({{3, 3}, 3.2, 6.4});
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    // One SMD part on the bottom side.
    for (auto& c : p.schematic.mutableComponents())
        if (c.kind == ComponentKind::Resistor && c.hasFootprint()) {
            c.pcb.bottom = true;
            break;
        }
    auto files = fabricationPackage(p);
    std::map<std::string, std::string> byName;
    for (const auto& f : files) byName[f.path] = f.content;
    auto has = [&](const std::string& path) { return byName.count(path) == 1 && !byName[path].empty(); };
    const std::string g = "gerbers/Amp_rev_A";
    for (const char* layer : {"-F_Cu", "-In1_Cu", "-In2_Cu", "-B_Cu", "-F_Mask", "-B_Mask", "-F_Paste", "-B_Paste",
                              "-F_Silkscreen", "-B_Silkscreen", "-Edge_Cuts"})
        CHECK(has(g + layer + ".gbr"));
    CHECK(has(g + "-PTH.drl") && has(g + "-NPTH.drl") && has(g + "-ipc356.ipc") && has(g + "-job.gbrjob"));
    for (const char* a : {"-bom.csv", "-bom_assembly.csv", "-cpl.csv", "-pick_and_place.csv", "-assembly_top.svg",
                          "-assembly_bottom.svg"})
        CHECK(has("assembly/Amp_rev_A" + std::string(a)));
    CHECK(has("Amp_rev_A-gerbers.zip") && has("fab_notes.txt") && has("Amp_rev_A-netlist.cir") && has("3d/Amp_rev_A.stl"));

    // Gerber X2 attributes, paste and silkscreen content.
    CHECK(byName[g + "-F_Mask.gbr"].find("%TF.FilePolarity,Negative*%") != std::string::npos);
    CHECK(byName[g + "-B_Paste.gbr"].find("%TF.FileFunction,Paste,Bot*%") != std::string::npos);
    CHECK(byName[g + "-B_Silkscreen.gbr"].find("%TF.FileFunction,Legend,Bot*%") != std::string::npos);
    int topSmd = 0;
    for (const auto& pd : p.pcb.pads(p.schematic)) topSmd += !pd.throughHole && pd.smdLayer == kTopLayer;
    size_t flashes = 0;
    const std::string& paste = byName[g + "-F_Paste.gbr"];
    for (size_t at = paste.find("D03*"); at != std::string::npos; at = paste.find("D03*", at + 1)) ++flashes;
    CHECK(static_cast<int>(flashes) == topSmd);
    // Designators are drawn: the top silkscreen has many more strokes than outlines alone.
    auto strokes = silkscreenText("R12", {0, 0}, 1.0, false);
    CHECK(strokes.size() >= 3);
    auto mirrored = silkscreenText("R12", {0, 0}, 1.0, true);
    CHECK(mirrored[0][0].x <= 0 && strokes[0][0].x >= 0);

    // IPC-D-356A: one test point per pad and via, fixed columns, terminated.
    const std::string& ipc = byName[g + "-ipc356.ipc"];
    int records = 0;
    std::istringstream lines(ipc);
    for (std::string line; std::getline(lines, line);)
        if (line.rfind("317", 0) == 0 || line.rfind("327", 0) == 0) {
            ++records;
            CHECK(line[26] == '-' && line[41] == 'X' && line[49] == 'Y');
        }
    CHECK(records == static_cast<int>(p.pcb.pads(p.schematic).size() + p.pcb.vias.size()));
    CHECK(ipc.size() >= 4 && ipc.compare(ipc.size() - 4, 4, "999\n") == 0);

    // Job file: stack-up with 4 copper layers, black mask, white legend.
    Json job = Json::parse(byName[g + "-job.gbrjob"]);
    CHECK(job.get("GeneralSpecs").get("LayerNumber").asInt() == 4);
    int copper = 0;
    bool black = false, white = false;
    for (const auto& l : job.get("MaterialStackup").items()) {
        copper += l.get("Type").asString() == "Copper";
        black |= l.get("Type").asString() == "SolderMask" && l.get("Color").asString() == "Black";
        white |= l.get("Type").asString() == "Legend" && l.get("Color").asString() == "White";
    }
    CHECK(copper == 4 && black && white);
    CHECK(job.get("FilesAttributes").size() == 11);

    // The zip holds the gerbers/ files (flat), each stored intact.
    const std::string& zip = byName["Amp_rev_A-gerbers.zip"];
    CHECK(zip.compare(0, 4, "PK\x03\x04") == 0);
    size_t entries = 0;
    for (size_t at = zip.find("PK\x01\x02"); at != std::string::npos; at = zip.find("PK\x01\x02", at + 1)) ++entries;
    size_t gerberFiles = 0;
    for (const auto& f : files) gerberFiles += f.path.rfind("gerbers/", 0) == 0;
    CHECK(entries == gerberFiles);
    CHECK(zip.find(byName[g + "-F_Cu.gbr"]) != std::string::npos);

    // Order notes carry what the fab asks for.
    const std::string& notes = byName["fab_notes.txt"];
    for (const char* text : {"Layers               4", "Thickness            1.60 mm", "Solder mask          Black",
                             "Silkscreen           White, top and bottom", "HASL lead-free", "Non-plated           1",
                             "IPC-D-356A", "Amp_rev_A-gerbers.zip"})
        CHECK(notes.find(text) != std::string::npos);
    std::string cpl = byName["assembly/Amp_rev_A-cpl.csv"];
    CHECK(cpl.find("Bottom") != std::string::npos && cpl.find("mm,") != std::string::npos);

    // Single-sided: no bottom copper or mask.
    p.pcb.settings.layerCount = 1;
    for (auto& c : p.schematic.mutableComponents()) c.pcb.bottom = false;
    std::set<std::string> single;
    for (const auto& f : fabricationPackage(p, "ss")) single.insert(f.path);
    CHECK(single.count("gerbers/ss-F_Cu.gbr") && !single.count("gerbers/ss-B_Cu.gbr") && !single.count("gerbers/ss-B_Mask.gbr"));

    // Written to disk.
    std::string dir = (std::filesystem::temp_directory_path() / "sieda_fab_test").string();
    std::filesystem::remove_all(dir);
    std::vector<std::string> written;
    std::string error;
    CHECK(writeFabricationPackage(p, dir, &written, &error, "ss"));
    CHECK(error.empty() && written.size() == single.size());
    CHECK(std::filesystem::exists(dir + "/gerbers/ss-F_Cu.gbr") && std::filesystem::exists(dir + "/ss-gerbers.zip"));
    std::filesystem::remove_all(dir);
}

TEST(net_roles_for_copper_colours) {
    Project p;
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "9", {0, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int vneg = s.addComponent(ComponentKind::VoltageSource, "12", {300, 0});   // + at ground, − is a −12 V rail
    int sig = s.addComponent(ComponentKind::VoltageSource, "SIN(0 1 1k)", {400, 0});  // a test signal, not a rail
    int label = s.addComponent(ComponentKind::NetLabel, "3V3", {500, 0});
    int r3 = s.addComponent(ComponentKind::Resistor, "1k", {500, 50});
    wire(s, bt, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", g, "GND");
    wire(s, bt, "-", g, "GND");
    wire(s, vneg, "+", g, "GND");
    int r4 = s.addComponent(ComponentKind::Resistor, "1k", {300, 100});
    wire(s, vneg, "-", r4, "1");
    int r5 = s.addComponent(ComponentKind::Resistor, "1k", {400, 100});
    wire(s, sig, "+", r5, "1");
    wire(s, sig, "-", g, "GND");
    s.connect({label, 0}, {r3, 0});
    auto role = [&](int comp, int pin) { return s.netRole(s.netOf({comp, pin})); };
    CHECK(role(r1, 0) == NetRole::Power);            // battery +
    CHECK(role(r1, 1) == NetRole::Signal);           // divider midpoint
    CHECK(role(r2, 1) == NetRole::Ground);
    CHECK(role(r4, 0) == NetRole::NegativeSupply);   // −12 V rail
    CHECK(role(r5, 0) == NetRole::Signal);           // sine test source
    CHECK(role(r3, 0) == NetRole::Power);            // named 3V3
    CHECK(std::string(netRoleName(NetRole::Ground)) == "ground");
    // A timer IC's VCC / GND power pins mark their nets even without names or symbols.
    std::string id = p.addCustomPart(findStandardPart("NE555")->spec);
    int u = s.addCustomComponent(id, "", {600, 0});
    int r6 = s.addComponent(ComponentKind::Resistor, "1k", {600, 100});
    int r7 = s.addComponent(ComponentKind::Resistor, "1k", {700, 100});
    s.connect({u, s.pinIndex(u, "VCC")}, {r6, 0});
    s.connect({u, s.pinIndex(u, "GND")}, {r7, 0});
    CHECK(role(r6, 0) == NetRole::Power);
    CHECK(role(r7, 0) == NetRole::Ground);
    // In the UI view model.
    Json vm = Json::parse(p.snapshot().dump());
    bool sawPower = false;
    for (const auto& n : vm.get("nets").items()) sawPower |= n.get("role").asString() == "power";
    CHECK(sawPower);
}

TEST(routing_cleanup_chamfers_and_merges) {
    Project p;
    PcbLayout& pcb = p.pcb;
    auto add = [&](Vec2 a, Vec2 b, int net) {
        Track t;
        t.a = a;
        t.b = b;
        t.net = net;
        t.width = 0.25;
        pcb.addTrack(t);
    };
    add({10, 10}, {20, 10}, 1);  // right-angle corner at (20,10) in open board
    add({20, 10}, {20, 20}, 1);
    add({5, 30}, {10, 30}, 2);   // collinear pieces
    add({10, 30}, {15, 30}, 2);
    // A corner hemmed in by another net's track running diagonally across it: no room for a chamfer.
    add({30, 5}, {40, 5}, 3);
    add({40, 5}, {40, 15}, 3);
    add({39.4, 5.2}, {39.8, 5.6}, 4);
    int changes = pcb.cleanupRouting(p.schematic);
    CHECK(changes == 2);
    int diagonals = 0, net2 = 0;
    for (const auto& t : pcb.tracks) {
        if (t.net == 1 && std::fabs(std::fabs(t.b.x - t.a.x) - std::fabs(t.b.y - t.a.y)) < 1e-9) ++diagonals;
        net2 += t.net == 2;
    }
    CHECK(diagonals == 1 && net2 == 1);
    // Still one connected piece of copper from (10,10) to (20,20).
    bool startsAt = false, endsAt = false;
    for (const auto& t : pcb.tracks)
        if (t.net == 1) {
            startsAt |= (t.a - Vec2{10, 10}).length() < 1e-9 || (t.b - Vec2{10, 10}).length() < 1e-9;
            endsAt |= (t.a - Vec2{20, 20}).length() < 1e-9 || (t.b - Vec2{20, 20}).length() < 1e-9;
        }
    CHECK(startsAt && endsAt);
    int net3 = 0;
    for (const auto& t : pcb.tracks) net3 += t.net == 3;
    CHECK(net3 == 2);  // hemmed-in corner untouched
}

TEST(bill_of_materials) {
    Project p;
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "9", {0, 0});
    std::vector<int> r;
    for (const char* v : {"10k", "10k", "4k7", "330", "10k"}) r.push_back(s.addComponent(ComponentKind::Resistor, v, {100, 0}));
    for (int i = 0; i < 6; ++i) s.addComponent(ComponentKind::Resistor, "1M", {100, 0});  // R6…R11: natural order
    int c1 = s.addComponent(ComponentKind::Capacitor, "100n", {200, 0});
    int d1 = s.addComponent(ComponentKind::LED, "Red", {300, 0});
    int q1 = s.addComponent(ComponentKind::NPN, "BC847", {400, 0});
    s.addComponent(ComponentKind::Ground, "", {0, 100});
    s.addComponent(ComponentKind::NetLabel, "VCC", {0, 100});

    auto lines = buildBom(s);
    auto find = [&](const std::string& value) -> const BomLine* {
        for (const auto& l : lines)
            if (l.value == value) return &l;
        return nullptr;
    };
    const BomLine* tenK = find("10k");
    CHECK(tenK && tenK->quantity() == 3 && tenK->refs[0] == "R1" && tenK->refs[2] == "R5");
    CHECK(tenK->suggestedManufacturer == "Yageo" && tenK->suggestedMpn == "RC0805FR-0710KL");
    CHECK(tenK->description == "Resistor 10kΩ ±1% 0805" && tenK->rating == "0.125 W");
    CHECK(find("4k7")->suggestedMpn == "RC0805FR-074K7L");
    CHECK(find("330")->suggestedMpn == "RC0805FR-07330RL");
    const BomLine* meg = find("1M");
    CHECK(meg && meg->refs.size() == 6 && meg->refs[4] == "R10" && meg->refs[5] == "R11");
    CHECK(find("100n")->rating == "≥ 25 V");  // 9 V supply, rated at twice the working voltage
    CHECK(find("BC847")->suggestedMpn == "BC847" && find("Red")->suggestedMpn.empty());
    for (const auto& l : lines) CHECK(l.type != "Ground" && l.type != "Net Label");
    CHECK(lines.front().item == 1 && lines.size() == 8);  // BT1, R×4 values, C, D, Q

    // Sourcing: part numbers, price and DNP flow into the totals and the assembly files.
    for (int id : {r[0], r[1], r[4]}) {
        auto* c = s.find(id);
        c->sourcing.manufacturer = "Yageo";
        c->sourcing.mpn = "RC0805FR-0710KL";
        c->sourcing.supplierPart = "C17414";
        c->sourcing.unitPrice = 0.002;
    }
    s.find(c1)->sourcing.unitPrice = 0.01;
    s.find(d1)->sourcing.dnp = true;
    lines = buildBom(s);
    BomSummary sum = summarizeBom(lines);
    CHECK(sum.dnp == 1 && sum.placements == 14);
    CHECK(std::fabs(sum.costPerBoard - (3 * 0.002 + 0.01)) < 1e-9);
    CHECK(find("10k")->notes.empty());  // part number and price entered
    CHECK(!find("4k7")->notes.empty() && find("4k7")->notes[0].find("suggested: RC0805FR-074K7L") != std::string::npos);
    std::string assembly = exportAssemblyBomCsv(s);
    CHECK(assembly.find("C17414") != std::string::npos && assembly.find("RC0805FR-0710KL") != std::string::npos);
    CHECK(assembly.find(s.find(d1)->ref) == std::string::npos);  // DNP not ordered
    std::string bom = exportBomCsv(s);
    CHECK(bom.rfind("Item,Quantity,References", 0) == 0 && bom.find("DNP") != std::string::npos);
    CHECK(bom.find("Yageo") != std::string::npos && bom.find("0.0060") != std::string::npos);  // line total
    s.find(d1)->pcb.placed = true;
    s.find(q1)->pcb.placed = true;
    std::string cpl = exportCplCsv(s, p.pcb);
    CHECK(cpl.find(s.find(q1)->ref + ",") != std::string::npos && cpl.find(s.find(d1)->ref + ",") == std::string::npos);

    // Saved with the project, together with the build quantity.
    p.buildQuantity = 25;
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.buildQuantity == 25);
    CHECK(q.schematic.find(r[0])->sourcing.supplierPart == "C17414" && q.schematic.find(d1)->sourcing.dnp);
    CHECK(q.schematic.find(bt)->sourcing.empty());
    Json j = bomJson(q.schematic, q.buildQuantity);
    CHECK(std::fabs(j.get("orderCost").asNumber() - 25 * (3 * 0.002 + 0.01)) < 1e-9);
    CHECK(j.get("summary").get("dnp").asInt() == 1);

    // C API: set sourcing field by field.
    SiedaProject* api = sieda_project_new("bom");
    int rid = sieda_add_component(api, static_cast<int32_t>(ComponentKind::Resistor), "10k", 0, 0, 0, "");
    CHECK(sieda_set_component_sourcing(api, rid, "{\"mpn\":\"X1\",\"unitPrice\":0.5}") == 1);
    CHECK(sieda_set_component_sourcing(api, rid, "{\"dnp\":true}") == 1);
    sieda_set_build_quantity(api, 4);
    char* out = sieda_bom_json(api);
    Json api_j = Json::parse(out);
    sieda_string_free(out);
    CHECK(api_j.get("lines")[0].get("mpn").asString() == "X1" && api_j.get("lines")[0].get("dnp").asBool());
    CHECK(api_j.get("buildQuantity").asInt() == 4);
    sieda_project_free(api);
}

TEST(ipc2221b_spacing_table_and_coating) {
    CHECK_NEAR(ipc2221Spacing(230, Ipc2221Column::B2), 1.25, 1e-12);
    CHECK_NEAR(ipc2221Spacing(230, Ipc2221Column::A5), 0.4, 1e-12);
    CHECK_NEAR(ipc2221Spacing(48, Ipc2221Column::B4), 0.13, 1e-12);
    CHECK_NEAR(ipc2221Spacing(12, Ipc2221Column::B1), 0.05, 1e-12);
    CHECK_NEAR(ipc2221Spacing(80, Ipc2221Column::A6), 0.5, 1e-12);
    CHECK_NEAR(ipc2221Spacing(1000, Ipc2221Column::A5), 3.05, 1e-9);
    CHECK_NEAR(ipc2221Clearance(230, false, true), 0.4, 1e-12);   // coated: A5 at any altitude
    CHECK_NEAR(ipc2221Clearance(230, true, false), 6.4, 1e-12);   // B3
    CHECK(std::string(ipc2221ColumnName(externalSpacingColumn(false, true))) == "A5");
    Project p;
    p.pcb.settings.coating = "silicone";
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.coating == "silicone" && q.pcb.settings.coated());
    CHECK(leakageSpacing(q.pcb.settings) == 0.25);
    SiedaProject* api = sieda_project_new("c");
    CHECK(sieda_pcb_set_coating(api, "parylene") == 1 && sieda_pcb_set_coating(api, "glitter") == 0);
    sieda_project_free(api);
}

TEST(reliability_circuit_rules) {
    auto codes = [](const Project& p) {
        std::set<std::string> c;
        for (const auto& v : reliabilityChecks(p)) c.insert(v.code);
        return c;
    };
    // A relay coil / motor winding switched by a MOSFET with no flyback diode.
    Project p;
    p.industry = "automotive";
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "12", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int l1 = s.addComponent(ComponentKind::Inductor, "10m", {100, 0});
    int q1 = s.addComponent(ComponentKind::NMOS, "2N7002", {200, 0});
    int pwm = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 5 1m)", {300, 0});
    wire(s, bt, "+", l1, "1");
    wire(s, bt, "-", g, "GND");
    wire(s, l1, "2", q1, "D");
    wire(s, q1, "S", g, "GND");
    wire(s, pwm, "+", q1, "G");
    wire(s, pwm, "-", g, "GND");
    auto c = codes(p);
    CHECK(c.count("REL_FLYBACK"));
    CHECK(c.count("REL_LOAD_DUMP"));        // automotive supply without TVS
    CHECK(c.count("REL_THERMAL_CYCLING"));  // automotive guidance
    int d1 = s.addComponent(ComponentKind::Diode, "1N4148", {100, 50});
    wire(s, d1, "A", l1, "2");
    wire(s, d1, "K", l1, "1");
    int tvs = s.addComponent(ComponentKind::Diode, "SMBJ24A", {0, 50});
    wire(s, tvs, "K", bt, "+");
    wire(s, tvs, "A", g, "GND");
    c = codes(p);
    CHECK(!c.count("REL_FLYBACK") && !c.count("REL_LOAD_DUMP"));
    // The PWM-driven gate net is a fast net.
    NetClassification cls = classifyNets(p);
    CHECK(cls.fast.count(s.netOf({q1, 0})));

    // A half-bridge of two N-MOSFETs with no gate driver: shoot-through risk.
    int q2 = s.addComponent(ComponentKind::NMOS, "IRF540N", {400, 0});
    wire(s, q2, "D", bt, "+");
    wire(s, q2, "S", q1, "D");
    p.industry = "robotics";
    c = codes(p);
    CHECK(c.count("REL_SHOOT_THROUGH") && c.count("REL_SNUBBER") && c.count("REL_FLEX"));

    // A high-impedance amplifier input (10 MΩ bias resistor) wants a guard ring driven by the other input.
    Project a;
    auto& t = a.schematic;
    int v = t.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int gg = t.addComponent(ComponentKind::Ground, "", {0, 100});
    int u = t.addComponent(ComponentKind::OpAmp, "LM358", {200, 0});
    int rb = t.addComponent(ComponentKind::Resistor, "10M", {100, 0});
    int rf = t.addComponent(ComponentKind::Resistor, "10k", {300, 0});
    wire(t, v, "-", gg, "GND");
    wire(t, rb, "1", u, "IN+");
    wire(t, rb, "2", gg, "GND");
    wire(t, u, "IN-", u, "OUT");  // follower
    wire(t, u, "OUT", rf, "1");
    wire(t, rf, "2", gg, "GND");
    wire(t, v, "+", rf, "1");
    NetClassification hz = classifyNets(a);
    int inPlus = t.netOf({u, 0});
    CHECK(hz.highImpedance.count(inPlus) && hz.highImpedance.at(inPlus) == t.netOf({u, 1}));
    CHECK(codes(a).count("REL_HIGH_IMPEDANCE"));
    // The autorouter keeps other nets the leakage spacing away from the high-impedance input.
    a.pcb.autoPlace(t, true);
    CHECK(a.pcb.autoRoute(t).failed == 0);
    CHECK(!codes(a).count("REL_LEAKAGE"));

    // Space: tin whiskers, outgassing, radiation; tin-lead solder and polyimide in the fab requirements.
    a.industry = "space";
    c = codes(a);
    CHECK(c.count("REL_TIN_WHISKER") && c.count("REL_OUTGASSING") && c.count("REL_RADIATION"));
    FabricationRequirements req = fabricationRequirements(a);
    CHECK(req.solder.find("Sn63Pb37") != std::string::npos && req.material.find("Polyimide") != std::string::npos);
    CHECK(req.ipcClass == 3 && recommendedSurfaceFinish(a).find("SnPb") != std::string::npos);
}

TEST(reliability_layout_rules) {
    auto has = [](const std::vector<RuleViolation>& v, const char* code) {
        return std::any_of(v.begin(), v.end(), [&](const RuleViolation& x) { return x.code == code; });
    };
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    auto clean = reliabilityChecks(p);
    CHECK(!has(clean, "REL_ACID_TRAP") && !has(clean, "REL_SOLDER_BRIDGE") && !has(clean, "REL_VIA_ASPECT"));

    // An acute bend on one net (acid trap) and a 0.1 mm via in a 1.6 mm board (16:1).
    int net = p.pcb.tracks.front().net;
    Track a;
    a.net = net;
    a.layer = 0;
    a.width = 0.25;
    a.a = {2, 2};
    a.b = {6, 2};
    Track b = a;
    b.a = {6, 2};
    b.b = {3, 4};
    p.pcb.addTrack(a);
    p.pcb.addTrack(b);
    Via v;
    v.net = net;
    v.position = {2, 8};
    v.drill = 0.1;
    v.diameter = 0.35;
    p.pcb.addVia(v);
    // Two resistors pushed together: their pads nearly touch (solder bridge).
    Component* r1 = nullptr;
    Component* r2 = nullptr;
    for (auto& c : p.schematic.mutableComponents())
        if (c.kind == ComponentKind::Resistor && c.hasFootprint()) (r1 ? r2 : r1) = r1 ? (r2 ? r2 : &c) : &c;
    CHECK(r1 && r2);
    r1->pcb.rotation = 0;
    r2->pcb.rotation = 180;  // stacked, pads swapped: R2.2 over R1.1 — different nets face each other
    double padH = 0;
    for (const auto& pd : p.pcb.pads(p.schematic))
        if (pd.componentId == r1->id) padH = pd.size.y;
    r2->pcb.position = r1->pcb.position + Vec2{0, padH + 0.1};  // 0.1 mm between the pad rows
    auto found = reliabilityChecks(p);
    CHECK(has(found, "REL_ACID_TRAP"));
    CHECK(has(found, "REL_VIA_ASPECT"));
    CHECK(has(found, "REL_SOLDER_BRIDGE"));

    // RF line width for 50 Ω (IPC-2141 microstrip on FR-4).
    BoardSettings s;
    s.layerCount = 2;
    s.thickness = 1.6;
    double w2 = microstripWidth(50, s);
    CHECK(w2 > 2.6 && w2 < 3.2);
    s.layerCount = 4;
    double w4 = microstripWidth(50, s);
    CHECK(w4 > 0.3 && w4 < 0.45);  // 0.2 mm prepreg under the top layer of a standard 4-layer build

    // Verification has a Design for Reliability stage.
    VerificationReport rep = verifyDesign(p);
    bool stage = false;
    for (const auto& st : rep.stages) stage |= st.id == "reliability";
    CHECK(stage);
}

TEST(stackup_controlled_impedance) {
    // IPC-2141 sanity: wider track → lower impedance; stripline below microstrip for the same geometry.
    CHECK(microstripImpedance(0.3, 0.2, 0.035, 4.4) > microstripImpedance(0.5, 0.2, 0.035, 4.4));
    CHECK(striplineImpedance(0.2, 0.4, 0.035, 4.4) < microstripImpedance(0.2, 0.2, 0.035, 4.4) + 30);
    CHECK(findLaminate("rogers-4350b") && findLaminate("megtron-6") && !findLaminate("unobtainium"));

    BoardSettings s;
    s.layerCount = 6;
    s.thickness = 1.6;
    const double fr4 = widthForImpedance(s, 0, 50);
    CHECK(fr4 > 0);
    CHECK_NEAR(trackImpedance(s, 0, fr4), 50, 1.0);
    CHECK(isStriplineLayer(s, 1) && !isStriplineLayer(s, 0) && !isStriplineLayer(s, 5));
    CHECK(widthForImpedance(s, 2, 50) < fr4);  // stripline: narrower for the same 50 Ω
    BoardSettings four = s;
    four.layerCount = 4;  // standard 4-layer build: 0.2 mm prepreg, thick core
    CHECK(std::fabs(dielectricBelow(four, 0) - 0.2) < 1e-9 && dielectricBelow(four, 1) > 1.0);
    s.material = "rogers-4350b";
    CHECK(widthForImpedance(s, 0, 50) > fr4);  // lower εr → wider line
    auto [dw, gap] = differentialPairGeometry(s, 0, 100);
    CHECK(dw > 0 && gap >= dw - 1e-9);
    CHECK_NEAR(differentialMicrostrip(trackImpedance(s, 0, dw), gap, dielectricBelow(s, 0)), 100, 5);

    // Persistence of the stack-up and the C API.
    Project p = amplifierProject();
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.material = "megtron-6";
    p.pcb.settings.construction = "rigid-flex";
    p.pcb.settings.singleEndedImpedance = 45;
    p.pcb.settings.backdrill = true;
    Project q = Project::fromJson(p.toJson());
    CHECK(q.pcb.settings.material == "megtron-6" && q.pcb.settings.construction == "rigid-flex");
    CHECK_NEAR(q.pcb.settings.singleEndedImpedance, 45, 1e-9);
    CHECK(q.pcb.settings.backdrill);
    Json sj = stackupJson(q.pcb.settings);
    CHECK(sj["materialName"].asString().find("Megtron") != std::string::npos);
    CHECK(sj["layers"].size() == 7);  // 4 copper + 3 dielectrics

    // A 4-layer via on a clock net that only connects the top layer leaves a stub → backdrill file.
    Project c = amplifierProject();
    auto& sch = c.schematic;
    int clk = sch.addComponent(ComponentKind::NetLabel, "CLK", {0, 200});
    int r = sch.addComponent(ComponentKind::Resistor, "33", {60, 200});
    int g = sch.addComponent(ComponentKind::Ground, "", {120, 200});
    wire(sch, clk, "N", r, "1");
    wire(sch, r, "2", g, "GND");
    c.schematicChanged();
    c.pcb.settings.layerCount = 4;
    c.pcb.autoPlace(c.schematic, true);  // footprints on the board: layout rules apply
    int net = sch.netOf({r, 0});
    Via v;
    v.net = net;
    v.position = {5, 5};
    c.pcb.addVia(v);
    Track t;
    t.net = net;
    t.layer = 0;
    t.width = 0.2;
    t.a = {5, 5};
    t.b = {9, 5};
    c.pcb.addTrack(t);
    auto stubs = viaStubs(c);
    CHECK(stubs.size() == 1);
    CHECK(stubs.size() == 1 && stubs[0].bottomStub > 1.0 && stubs[0].topStub == 0);
    auto codes = [&] {
        std::set<std::string> out;
        for (const auto& x : reliabilityChecks(c)) out.insert(x.code);
        return out;
    };
    CHECK(codes().count("REL_VIA_STUB"));
    c.pcb.settings.backdrill = true;
    CHECK(codes().count("REL_BACKDRILL") && !codes().count("REL_VIA_STUB"));
    CHECK(exportBackdrill(c).find("M48") != std::string::npos);

    // Metal-core boards are single/double-sided.
    c.pcb.settings.construction = "metal-core";
    CHECK(codes().count("REL_METAL_CORE_LAYERS"));
}

TEST(length_matching_serpentines) {
    Project p;
    auto& s = p.schematic;
    auto signal = [&](const char* name, double y) {
        int l = s.addComponent(ComponentKind::NetLabel, name, {0, y});
        int r = s.addComponent(ComponentKind::Resistor, "33", {60, y});
        int g = s.addComponent(ComponentKind::Ground, "", {120, y});
        wire(s, l, "N", r, "1");
        wire(s, r, "2", g, "GND");
        return r;
    };
    int dq0 = signal("DQ0", 0), dq1 = signal("DQ1", 100);
    int dp = signal("USB_DP", 200), dn = signal("USB_DN", 300);
    signal("A0", 400);
    signal("A1", 500);  // MCU analog pins — not a bus
    signal("LED1", 600);
    signal("LED2", 700);
    p.schematicChanged();

    auto groups = lengthGroups(s, p.pcb.settings);
    CHECK(groups.size() == 2);
    bool pair = false, bus = false;
    for (const auto& g : groups) {
        pair |= g.kind == "pair" && g.name == "USB_D" && g.nets.size() == 2;
        bus |= g.kind == "bus" && g.name == "DQ" && g.nets.size() == 2;
    }
    CHECK(pair && bus);

    p.pcb.settings.width = 100;
    p.pcb.settings.height = 80;
    int n0 = s.netOf({dq0, 0}), n1 = s.netOf({dq1, 0});
    auto track = [&](int net, Vec2 a, Vec2 b) {
        Track t;
        t.net = net;
        t.width = 0.2;
        t.a = a;
        t.b = b;
        p.pcb.addTrack(t);
    };
    track(n0, {10, 60}, {40, 60});  // 30 mm
    track(n1, {10, 70}, {30, 70});  // 20 mm: 10 mm short
    track(s.netOf({dp, 0}), {10, 20}, {40, 20});
    track(s.netOf({dn, 0}), {10, 22}, {39, 22});  // 1 mm skew, a neighbour 2 mm away
    auto matched = [](const Json& r, size_t i) { return r.get("groups")[i].get("matched").asBool(false); };
    Json before = lengthReportJson(p.pcb, s);
    CHECK(!matched(before, 0) && !matched(before, 1));

    CHECK(tuneLengths(p.pcb, s) == 2);
    CHECK_NEAR(routedNetLength(p.pcb, n1), 30, 1e-6);
    CHECK_NEAR(routedNetLength(p.pcb, s.netOf({dn, 0})), 30, 1e-6);
    Json after = lengthReportJson(p.pcb, s);
    CHECK(after.get("groups").size() == 2 && matched(after, 0) && matched(after, 1));
    // The serpentine keeps the end points (connectivity) and clearance to the neighbouring pair member.
    for (const auto& t : p.pcb.tracks)
        if (t.net == s.netOf({dn, 0}))
            CHECK(segmentSegmentDistance(t.a, t.b, {10, 20}, {40, 20}) - t.width >= p.pcb.settings.clearance - 1e-6);
    CHECK(tuneLengths(p.pcb, s) == 0);  // already matched

    // Persistence.
    p.pcb.settings.pairSkewTolerance = 0.05;
    p.pcb.settings.lengthTuning = false;
    Project q = Project::fromJson(p.toJson());
    CHECK_NEAR(q.pcb.settings.pairSkewTolerance, 0.05, 1e-9);
    CHECK(!q.pcb.settings.lengthTuning);
}

TEST(hdi_blind_buried_microvias) {
    // Via kinds on a 6-layer stack.
    Via v;
    CHECK(std::string(viaKind(v, 6)) == "through" && v.isThrough());
    v.fromLayer = 0;
    v.toLayer = 1;
    v.drill = 0.1;
    CHECK(std::string(viaKind(v, 6)) == "microvia" && v.spans(1) && !v.spans(2));
    v.toLayer = 2;
    v.drill = 0.2;
    CHECK(std::string(viaKind(v, 6)) == "blind");
    v.fromLayer = 2;
    v.toLayer = 3;
    CHECK(std::string(viaKind(v, 6)) == "buried");

    // HDI stack-up: thin laser build-up dielectrics under the outer layers.
    BoardSettings bs;
    bs.layerCount = 6;
    bs.thickness = 1.6;
    const double plain = dielectricBelow(bs, 0);
    bs.hdi = true;
    CHECK(dielectricBelow(bs, 0) < 0.1 && dielectricBelow(bs, 4) < 0.1 && dielectricBelow(bs, 2) > plain);
    CHECK(widthForImpedance(bs, 0, 50) < 0.3);  // thin build-up → narrow 50 Ω microstrip
    Via mv;
    mv.toLayer = 1;
    mv.drill = 0.1;
    CHECK((viaBarrelDepth(bs, mv) - 2 * copperThickness(bs)) / mv.drill <= 1.0);  // laser-drillable dielectric

    // Auto Route on a 4-layer HDI board: vias are cut to the layers they join, connectivity and DRC stay intact.
    Project p = amplifierProject();
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.hdi = true;
    p.pcb.autoPlace(p.schematic, true);
    RouteStats st = p.pcb.autoRoute(p.schematic);
    CHECK(st.failed == 0);
    CHECK(p.pcb.ratsnest(p.schematic).empty());
    int errors = 0;
    for (const auto& e : p.pcb.runDRC(p.schematic)) errors += e.severity == Severity::Error;
    CHECK(errors == 0);
    for (const auto& via : p.pcb.vias) CHECK(via.fromLayer >= 0 && via.lastLayer(4) > via.fromLayer);

    // A hand-placed microvia from the top to layer 2 joining two tracks: own drill file, only on its layers.
    Project q = amplifierProject();
    q.pcb.settings.layerCount = 4;
    q.pcb.settings.hdi = true;
    int net = q.schematic.netOf({q.schematic.components().front().id, 0});
    Track a;
    a.net = net;
    a.layer = 0;
    a.a = {5, 5};
    a.b = {10, 5};
    Track b = a;
    b.layer = 1;
    b.a = {10, 5};
    b.b = {10, 12};
    q.pcb.addTrack(a);
    q.pcb.addTrack(b);
    Via through;
    through.net = net;
    through.position = {10, 5};
    q.pcb.addVia(through);
    CHECK(q.pcb.applyHdiVias(q.schematic) == 1);
    const Via& cut = q.pcb.vias.front();
    CHECK(std::string(viaKind(cut, 4)) == "microvia");
    CHECK_NEAR(cut.drill, 0.1, 1e-9);
    auto spans = viaSpans(q.pcb);
    CHECK(spans.size() == 1 && spans[0].first == 0 && spans[0].second == 1);
    CHECK(exportExcellonSpan(q.pcb, 0, 1).find("Plated,1,2,Blind") != std::string::npos);
    CHECK(exportExcellonDrill(q.schematic, q.pcb, true).find("X10.000") == std::string::npos);  // not in the PTH file
    CHECK(exportCopperGerber(q.schematic, q.pcb, 0).find("X10000000Y") != std::string::npos ||
          exportCopperGerber(q.schematic, q.pcb, 0).find("D03") != std::string::npos);
    // The bottom layer has no copper of this via.
    const std::string bottom = exportCopperGerber(q.schematic, q.pcb, 3);
    CHECK(bottom.find("D03") == std::string::npos);
    // Persistence of the span (the hand-drawn copper is not on a pad, so check the saved document itself).
    const std::string saved = q.toJson().dump();
    CHECK(saved.find("\"toLayer\":1") != std::string::npos && saved.find("\"hdi\":true") != std::string::npos);
    CHECK(Project::fromJson(q.toJson()).pcb.settings.hdi);
    // Fabrication package lists the span drill file and HDI notes.
    bool spanFile = false, notes = false;
    for (const auto& f : fabricationPackage(q)) {
        spanFile |= f.path.find("-L1-L2.drl") != std::string::npos;
        notes |= f.content.find("HDI (IPC-2226)") != std::string::npos;
    }
    CHECK(spanFile && notes);
}

TEST(locked_footprints_survive_auto_place) {
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    Component* first = nullptr;
    for (auto& c : p.schematic.mutableComponents())
        if (c.hasFootprint()) { first = &c; break; }
    CHECK(first);
    first->pcb.position = {7.5, 9.0};
    first->pcb.locked = true;
    const int id = first->id;
    p.pcb.autoPlace(p.schematic, true);
    const Component* again = p.schematic.find(id);
    CHECK(again && again->pcb.placed && again->pcb.locked);
    CHECK_NEAR(again->pcb.position.x, 7.5, 1e-9);
    CHECK_NEAR(again->pcb.position.y, 9.0, 1e-9);
    Project q = Project::fromJson(p.toJson());
    CHECK(q.schematic.find(id)->pcb.locked);
}

TEST(embedded_passives) {
    Project p = amplifierProject();
    p.pcb.settings.layerCount = 4;
    Component* r330 = nullptr;
    Component* cap = nullptr;
    for (auto& c : p.schematic.mutableComponents()) {
        if (c.kind == ComponentKind::Resistor && c.value == "330") r330 = &c;
        if (c.kind == ComponentKind::Capacitor) cap = &c;
    }
    CHECK(r330 && cap);
    const int rid = r330->id, cid = cap->id;
    r330->pcb.embeddedLayer = 1;
    p.pcb.autoPlace(p.schematic, true);

    // 330 Ω on 100 Ω/sq foil: 3.3 squares, 0.5 mm wide.
    auto e = embeddedElement(*p.schematic.find(rid), p.pcb.settings);
    CHECK(e && e->resistor && e->layer == 1 && e->inRange);
    CHECK(e && e->sheet == 100 && std::fabs(e->squares - 3.3) < 1e-9 && std::fabs(e->length - 1.65) < 1e-9);
    int onInner = 0;
    for (const auto& pd : p.pcb.pads(p.schematic))
        if (pd.componentId == rid) onInner += pd.smdLayer == 1 && !pd.throughHole;
    CHECK(onInner == 2);

    // Routes and passes DRC with the part inside the board.
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    CHECK(p.pcb.ratsnest(p.schematic).empty());
    int errors = 0;
    for (const auto& v : p.pcb.runDRC(p.schematic)) errors += v.severity == Severity::Error;
    CHECK(errors == 0);

    // Not assembled: off the CPL and the assembly BOM; listed as embedded in the BOM.
    CHECK(exportCplCsv(p.schematic, p.pcb).find(r330->ref + ",") == std::string::npos);
    CHECK(exportAssemblyBomCsv(p.schematic).find(r330->ref) == std::string::npos);
    bool bomEmbedded = false;
    for (const auto& l : buildBom(p.schematic)) bomEmbedded |= l.embedded && l.refs.front() == r330->ref;
    CHECK(bomEmbedded);
    // Fabrication: resistor element artwork and notes.
    bool artwork = false, notes = false;
    for (const auto& f : fabricationPackage(p)) {
        artwork |= f.path.find("-In1_Resistor.gbr") != std::string::npos && f.content.find("G36") != std::string::npos;
        notes |= f.content.find("EMBEDDED PASSIVES") != std::string::npos;
    }
    CHECK(artwork && notes);
    auto codes = [&] {
        std::set<std::string> out;
        for (const auto& x : reliabilityChecks(p)) out.insert(x.code);
        return out;
    };
    CHECK(codes().count("REL_EMBEDDED_PASSIVES"));

    // 100 nF is far too large for a buried plate pair; a 2-layer board cannot hold embedded parts at all.
    p.schematic.find(cid)->pcb.embeddedLayer = 1;
    auto ce = embeddedElement(*p.schematic.find(cid), p.pcb.settings);
    CHECK(ce && !ce->resistor && ce->layer2 == 2 && !ce->inRange);
    CHECK(codes().count("REL_EMBEDDED_RANGE"));
    p.schematic.find(rid)->value = "330 1%";
    CHECK(codes().count("REL_EMBEDDED_TOLERANCE"));
    Project saved = Project::fromJson(p.toJson());
    CHECK(saved.schematic.find(rid)->pcb.embeddedLayer == 1);
    p.pcb.settings.layerCount = 2;
    CHECK(!embeddedElement(*p.schematic.find(rid), p.pcb.settings));
    CHECK(codes().count("REL_EMBEDDED_STACKUP"));
}

TEST(high_layer_count_and_computing_segments) {
    // Even stack-ups up to 24 layers (servers, mainframes, GPU baseboards).
    CHECK(BoardSettings::normalizeLayerCount(1) == 1 && BoardSettings::normalizeLayerCount(3) == 4);
    CHECK(BoardSettings::normalizeLayerCount(8) == 8 && BoardSettings::normalizeLayerCount(13) == 14);
    CHECK(BoardSettings::normalizeLayerCount(40) == BoardSettings::kMaxLayers);
    Project p = amplifierProject();
    p.pcb.settings.layerCount = 12;
    p.pcb.zones.push_back({"GND", 1, true, 0});
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    CHECK(p.pcb.ratsnest(p.schematic).empty());
    Json st = stackupJson(p.pcb.settings);
    CHECK(st["layers"].size() == 23);  // 12 copper + 11 dielectrics
    Project q = Project::fromJson(p.toJson());
    CHECK(q.pcb.settings.layerCount == 12);

    // Computing segments: profiles, materials and fabrication requirements.
    for (const char* id : {"motherboard", "server", "hpc", "arm", "addin"}) {
        const IndustryProfile* prof = findIndustry(id);
        CHECK(prof && !prof->guidance.empty() && !prof->standards.empty());
        CHECK(prof && std::any_of(designRulePresets().begin(), designRulePresets().end(),
                                  [&](const DesignRulePreset& r) { return r.name == prof->rulePreset; }));
        Project seg = amplifierProject();
        seg.industry = id;
        FabricationRequirements req = fabricationRequirements(seg);
        CHECK(!req.finish.empty());
        CHECK(std::any_of(req.notes.begin(), req.notes.end(),
                          [](const std::string& n) { return n.find("Controlled impedance") != std::string::npos; }));
    }
    Project server = amplifierProject();
    server.industry = "server";
    CHECK(fabricationRequirements(server).ipcClass == 3);
    Project card = amplifierProject();
    card.industry = "addin";
    CHECK(fabricationRequirements(card).finish.find("hard") != std::string::npos);
    CHECK(findLaminate("megtron-7") && findLaminate("tachyon-100g"));
    CHECK(findLaminate("tachyon-100g")->er < findLaminate("megtron-6")->er);
}

TEST(symbol_editor_auto_arrange_rules_geometry_and_board) {
    using P = PinType;
    auto spot = [](const SymbolSpec& sym, const std::string& number) -> const SymbolPin* {
        for (const auto& p : sym.pins)
            if (p.number == number) return &p;
        return nullptr;
    };
    auto side = [&](const SymbolSpec& sym, const std::string& number) { return spot(sym, number) ? spot(sym, number)->side : '?'; };
    // A part exercising every Auto Arrange rule.
    CustomPartSpec mcu;
    mcu.name = "ARRANGE-TEST";
    const std::vector<std::tuple<const char*, P>> pins = {
        {"VDD", P::PowerIn}, {"AVDD", P::PowerIn}, {"VDD", P::PowerIn}, {"GND", P::PowerIn}, {"AGND", P::Passive},
        {"PGND", P::PowerIn}, {"VSS", P::PowerIn}, {"VEE", P::PowerIn}, {"V-", P::PowerIn}, {"0V", P::Passive},
        {"NRST", P::Input}, {"XTAL1", P::Passive}, {"SWDIO", P::Bidirectional}, {"BOOT0", P::Input},
        {"IN_A", P::Input}, {"OUT_A", P::Output}, {"nFAULT", P::OpenCollector}, {"VOUT", P::PowerOut},
        {"PA10", P::Bidirectional}, {"PA9", P::Bidirectional}, {"PA0", P::Bidirectional}, {"P1.3", P::Bidirectional},
        {"P1_2", P::Bidirectional}, {"GPIO12", P::Bidirectional}, {"IO5", P::Bidirectional}, {"RB7", P::Bidirectional},
        {"PA5/ADC1_5", P::Bidirectional}, {"NC", P::NoConnect}, {"NC", P::NoConnect}, {"CAP", P::Passive}, {"FB", P::Passive}};
    for (size_t i = 0; i < pins.size(); ++i)
        mcu.pins.push_back({std::to_string(i + 1), std::get<0>(pins[i]), std::get<1>(pins[i]), ""});
    const SymbolSpec sym = autoArrangeSymbol(mcu);
    CHECK(sym.pins.size() == mcu.pins.size());
    // Supplies on top (repeated ones stacked), grounds and negative supplies at the bottom.
    CHECK(side(sym, "1") == 'T' && side(sym, "2") == 'T' && side(sym, "3") == 'T');
    CHECK(spot(sym, "1")->slot == spot(sym, "3")->slot && spot(sym, "1")->slot != spot(sym, "2")->slot);
    for (const char* g : {"4", "5", "6", "7", "8", "9", "10"}) CHECK(side(sym, g) == 'B');
    // Control pins first on the left, then inputs; outputs (open-drain, regulator outputs too) on the right.
    for (const char* c : {"11", "12", "13", "14"}) CHECK(side(sym, c) == 'L' && spot(sym, c)->slot < 4);
    CHECK(side(sym, "15") == 'L' && spot(sym, "15")->slot > spot(sym, "14")->slot + 1);  // a gap before inputs
    CHECK(side(sym, "16") == 'R' && side(sym, "17") == 'R' && side(sym, "18") == 'R');
    // Ports: PA in bit order (PA0, PA5, PA9, PA10 — not lexical), each port one group on one side.
    CHECK(side(sym, "21") == side(sym, "27") && side(sym, "27") == side(sym, "20") && side(sym, "20") == side(sym, "19"));
    CHECK(spot(sym, "21")->slot < spot(sym, "27")->slot && spot(sym, "27")->slot < spot(sym, "20")->slot &&
          spot(sym, "20")->slot < spot(sym, "19")->slot);
    CHECK(side(sym, "22") == side(sym, "23") && spot(sym, "23")->slot + 1 == spot(sym, "22")->slot);  // P1_2, P1.3
    for (const char* port : {"24", "25", "26"}) CHECK(side(sym, port) == 'L' || side(sym, port) == 'R');
    // No-connect pins last on the right; plain passives on the sides.
    int lastRight = -1;
    for (const auto& p : sym.pins)
        if (p.side == 'R') lastRight = std::max(lastRight, p.slot);
    CHECK(side(sym, "28") == 'R' && side(sym, "29") == 'R' && spot(sym, "29")->slot == lastRight);
    CHECK((side(sym, "30") == 'L' || side(sym, "30") == 'R') && (side(sym, "31") == 'L' || side(sym, "31") == 'R'));
    // Without stacking every pin has its own spot; the result always passes the checks.
    const SymbolSpec flat = autoArrangeSymbol(mcu, false);
    CHECK(spot(flat, "1")->slot != spot(flat, "3")->slot);
    CustomPartSpec checked = mcu;
    checked.symbol = sym;
    for (const auto& issue : checkSymbol(checked)) CHECK(issue.severity == "info");
    checked.symbol = flat;
    CHECK(checkSymbol(checked).empty());
    // A long port is cut into groups of 8 balanced over both sides.
    CustomPartSpec wide;
    wide.name = "PORT-TEST";
    for (int b = 0; b < 24; ++b) wide.pins.push_back({std::to_string(b + 1), "PB" + std::to_string(b), P::Bidirectional, ""});
    const SymbolSpec banks = autoArrangeSymbol(wide);
    int left = 0, right = 0;
    for (const auto& p : banks.pins) (p.side == 'L' ? left : right) += 1;
    CHECK(left == 16 && right == 8);
    CHECK(side(banks, "1") != side(banks, "9"));  // PB0–7 and PB8–15 on different sides

    // Geometry: the generated box equals its explicit layout; long top names make room above the side pins; a
    // requested width below the automatic one is ignored; the body is never narrower than 30.
    CustomPartSpec chip;
    chip.name = "GEO-TEST";
    chip.pins = {{"1", "A", P::Input}, {"2", "B", P::Input}, {"3", "C", P::Output}, {"4", "D", P::Output}};
    auto generated = CustomPartRegistry::instance().registerPart(chip);
    CustomPartSpec explicitBox = chip;
    explicitBox.symbol.pins = {{"1", 'L', 0}, {"2", 'L', 1}, {"3", 'R', 1}, {"4", 'R', 0}};
    auto same = CustomPartRegistry::instance().registerPart(explicitBox);
    CHECK(same->symbolHalfWidth == generated->symbolHalfWidth && same->symbolHalfHeight == generated->symbolHalfHeight);
    for (size_t i = 0; i < 4; ++i)
        CHECK(same->def.pins[i].offset.x == generated->def.pins[i].offset.x && same->def.pins[i].offset.y == generated->def.pins[i].offset.y);
    CHECK(generated->symbolHalfWidth >= 30);
    CustomPartSpec tall = explicitBox;
    tall.pins.push_back({"5", "VERY_LONG_SUPPLY", P::PowerIn, ""});
    tall.symbol.pins.push_back({"5", 'T', 0});
    auto tallPart = CustomPartRegistry::instance().registerPart(tall);
    CHECK(tallPart->symbolHalfHeight > generated->symbolHalfHeight + 40);
    CHECK(tallPart->def.pins[4].offset.x == 0 && tallPart->def.pins[4].offset.y == -(tallPart->symbolHalfHeight + 20));
    CustomPartSpec narrow = explicitBox;
    narrow.symbol.width = 20;
    CHECK(CustomPartRegistry::instance().registerPart(narrow)->symbolHalfWidth == generated->symbolHalfWidth);

    // Stacked pins on a rotated component stay joined and move together.
    CustomPartSpec stacked = chip;
    stacked.pins.push_back({"5", "GND", P::PowerIn, ""});
    stacked.pins.push_back({"6", "GND", P::PowerIn, ""});
    stacked.symbol.pins = {{"1", 'L', 0}, {"2", 'L', 1}, {"3", 'R', 0}, {"4", 'R', 1}, {"5", 'B', 0}, {"6", 'B', 0}};
    Project p;
    auto& sch = p.schematic;
    const std::string id = p.addCustomPart(stacked);
    int u = sch.addCustomComponent(id, "", {300, 0}, 90);
    const Vec2 a = sch.pinPosition({u, sch.pinIndex(u, "5")}), b = sch.pinPosition({u, sch.pinIndex(u, "6")});
    CHECK(a.x == b.x && a.y == b.y);
    CHECK(sch.netOf({u, sch.pinIndex(u, "5")}) == sch.netOf({u, sch.pinIndex(u, "6")}));
    // Wired up and laid out: both GND pads are on the ground net and the router joins them.
    int v = sch.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int r = sch.addComponent(ComponentKind::Resistor, "1k", {120, -80});
    int g = sch.addComponent(ComponentKind::Ground, "", {0, 120});
    sch.connect({v, 1}, {g, 0});
    sch.connect({v, 0}, {r, 0});
    sch.connect({r, 1}, {u, sch.pinIndex(u, "1")});
    sch.connect({u, sch.pinIndex(u, "5")}, {g, 0});
    p.pcb.autoPlace(sch, true);
    CHECK(p.pcb.autoRoute(sch).failed == 0);
    int groundPads = 0;
    const int gnd = sch.netOf({g, 0});
    for (const auto& pad : p.pcb.pads(sch))
        if (pad.componentId == u && pad.net == gnd) ++groundPads;
    CHECK(groundPads == 2);
    // The layout is saved with the project and loads back to the same symbol and nets.
    Project back = Project::fromJson(Json::parse(p.toJson().dump()));
    const Component* restored = back.schematic.find(u);
    CHECK(restored && back.schematic.netOf({u, back.schematic.pinIndex(u, "6")}) == back.schematic.netOf({g, 0}));
    const CustomPart* restoredPart = CustomPartRegistry::instance().find(restored->customPart);
    CHECK(restoredPart && restoredPart->spec.symbol.pins.size() == 6);
    // Swapping in a re-arranged symbol keeps the wires on their pins.
    CustomPartSpec moved = stacked;
    moved.symbol.pins[4].side = 'T';
    moved.symbol.pins[5].side = 'T';
    const std::string movedId = p.addCustomPart(moved);
    CHECK(sch.replaceCustomPart(id, movedId) == 1);
    CHECK(sch.netOf({u, sch.pinIndex(u, "6")}) == sch.netOf({g, 0}));
    CHECK(sch.pinPosition({u, sch.pinIndex(u, "5")}).x != a.x || sch.pinPosition({u, sch.pinIndex(u, "5")}).y != a.y);
}

TEST(symbol_editor_layout_auto_arrange_and_checks) {
    auto reg = [](const CustomPartSpec& spec) { return CustomPartRegistry::instance().registerPart(spec); };
    auto pinAt = [](const std::shared_ptr<const CustomPart>& part, const std::string& number) -> const PinDef* {
        for (const auto& p : part->def.pins)
            if (p.number == number) return &p;
        return nullptr;
    };
    auto codes = [](const std::vector<SymbolIssue>& v) {
        std::multiset<std::string> out;
        for (const auto& i : v) out.insert(i.code);
        return out;
    };

    // Small parts keep the generated datasheet-order box: NE555 pins 1–4 down the left, 5–8 up the right.
    const CustomPartSpec ne555 = findStandardPart("NE555")->spec;
    CHECK(ne555.symbol.empty());
    auto timer = reg(ne555);
    CHECK(pinAt(timer, "1")->side == 'L' && pinAt(timer, "8")->side == 'R');
    CHECK(pinAt(timer, "1")->offset.y < pinAt(timer, "4")->offset.y);
    CHECK(pinAt(timer, "8")->offset.y == pinAt(timer, "1")->offset.y);

    // Large library parts are auto-arranged: supplies on top (repeated ones stacked), grounds below, control pins top left.
    for (const auto& sp : standardParts()) {
        if (sp.spec.pins.size() < 16) continue;
        CHECK(!sp.spec.symbol.empty());
        bool clean = true;
        for (const auto& issue : checkSymbol(sp.spec)) clean &= issue.severity == "info";
        if (!clean) std::printf("    %s: symbol issues\n", sp.spec.name.c_str());
        CHECK(clean);
        auto part = reg(sp.spec);
        bool onGrid = true;
        for (const auto& p : part->def.pins)
            onGrid &= std::fmod(std::fabs(p.offset.x), 10.0) < 1e-9 && std::fmod(std::fabs(p.offset.y), 10.0) < 1e-9;
        CHECK(onGrid);
    }
    auto stm = reg(findStandardPart("STM32F405RGT6")->spec);
    std::set<std::pair<double, double>> vdd;
    for (const auto& p : stm->def.pins)
        if (p.name == "VDD") vdd.insert({p.offset.x, p.offset.y});
    CHECK(vdd.size() == 1);  // four VDD pins, one stacked pin
    CHECK(pinAt(stm, "19")->name == "VDD" && pinAt(stm, "19")->side == 'T');
    CHECK(pinAt(stm, "19")->offset.y == -(stm->symbolHalfHeight + 20));
    const PinDef* vss = nullptr;
    for (const auto& p : stm->def.pins)
        if (p.name == "VSS") vss = &p;
    CHECK(vss && vss->side == 'B' && vss->offset.y == stm->symbolHalfHeight + 20);
    const PinDef* nrst = nullptr;
    for (const auto& p : stm->def.pins)
        if (p.name == "NRST") nrst = &p;
    double topLeft = 1e9;  // NRST is the first pin of the left side (reset / boot / debug group)
    for (const auto& p : stm->def.pins)
        if (p.side == 'L') topLeft = std::min(topLeft, p.offset.y);
    CHECK(nrst && nrst->side == 'L' && nrst->offset.y == topLeft);
    // Port pins in bit order, one slot apart, within a group of 8.
    auto y = [&](const char* name) {
        for (const auto& p : stm->def.pins)
            if (p.name == name) return p.offset.y;
        return 1e9;
    };
    CHECK(y("PA1") - y("PA0") == 20 && y("PA7") - y("PA0") == 140);

    // A hand-made layout on all four sides.
    CustomPartSpec chip;
    chip.name = "SYM-TEST";
    chip.pins = {{"1", "VCC", PinType::PowerIn, ""}, {"2", "IN", PinType::Input, ""}, {"3", "OUT", PinType::Output, ""},
                 {"4", "GND", PinType::PowerIn, ""}, {"5", "GND", PinType::PowerIn, ""}, {"6", "EN", PinType::Input, ""}};
    chip.symbol.pins = {{"1", 'T', 0}, {"2", 'L', 0}, {"6", 'L', 2}, {"3", 'R', 1}, {"4", 'B', 0}, {"5", 'B', 0}};
    CHECK(codes(checkSymbol(chip)) == std::multiset<std::string>({"SYM_STACK"}));
    auto c = reg(chip);
    CHECK(pinAt(c, "1")->offset.x == 0 && pinAt(c, "1")->offset.y == -(c->symbolHalfHeight + 20));
    CHECK(pinAt(c, "4")->offset.x == pinAt(c, "5")->offset.x && pinAt(c, "4")->offset.y == pinAt(c, "5")->offset.y);
    CHECK(pinAt(c, "6")->offset.y - pinAt(c, "2")->offset.y == 40);  // slot 2 leaves a gap
    CHECK(pinAt(c, "2")->offset.x == -(c->symbolHalfWidth + 20) && pinAt(c, "3")->offset.x == c->symbolHalfWidth + 20);
    // The body widens for a requested width and for a row of top pins.
    CustomPartSpec wide = chip;
    wide.symbol.width = 200;
    CHECK(reg(wide)->symbolHalfWidth == 100);
    CustomPartSpec row = chip;
    for (int k = 0; k < 12; ++k) {
        row.pins.push_back({std::to_string(7 + k), "D" + std::to_string(k), PinType::Bidirectional, ""});
        row.symbol.pins.push_back({std::to_string(7 + k), 'T', 1 + k});
    }
    auto rowPart = reg(row);
    CHECK(rowPart->symbolHalfWidth >= 13 * 10 + 10);
    double minX = 1e9, maxX = -1e9;
    for (const auto& p : rowPart->def.pins)
        if (p.side == 'T') {
            minX = std::min(minX, p.offset.x);
            maxX = std::max(maxX, p.offset.x);
        }
    CHECK(minX == -maxX && maxX < rowPart->symbolHalfWidth);

    // Checks: missing, unknown, duplicate, overlap, stacked signal pins; errors refuse registration.
    CustomPartSpec bad = chip;
    bad.symbol.pins.pop_back();  // pin 5 missing
    CHECK(codes(checkSymbol(bad)).count("SYM_MISSING") == 1);
    bad.symbol.pins.push_back({"9", 'R', 5});
    CHECK(codes(checkSymbol(bad)).count("SYM_UNKNOWN") == 1);
    bad = chip;
    bad.symbol.pins.push_back({"2", 'R', 6});
    CHECK(codes(checkSymbol(bad)).count("SYM_DUPLICATE") == 1);
    bad = chip;
    bad.symbol.pins[2] = {"6", 'L', 0};  // EN on top of IN
    CHECK(codes(checkSymbol(bad)).count("SYM_OVERLAP") == 1);
    bool threw = false;
    try {
        reg(bad);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
    CustomPartSpec sig = chip;
    sig.pins.push_back({"7", "IN", PinType::Input, ""});
    sig.symbol.pins.push_back({"7", 'L', 0});
    CHECK(codes(checkSymbol(sig)).count("SYM_STACK_SIGNAL") == 1);

    // JSON: the layout round-trips, changes the part id, and is validated.
    Json j = customPartSpecToJson(chip);
    CHECK(j.get("symbolLayout").get("pins").size() == 6);
    CustomPartSpec back = customPartSpecFromJson(j);
    CHECK(back.symbol.pins.size() == 6 && back.symbol.pins[0].side == 'T' && back.symbol.pins[2].slot == 2);
    CHECK(reg(back)->id == c->id);
    CustomPartSpec plain = chip;
    plain.symbol = {};
    CHECK(reg(plain)->id != c->id);
    CHECK(customPartSpecToJson(plain).get("symbolLayout").isNull());  // generated symbols keep their old ids
    for (const char* badSym : {R"({"pins":[{"number":"1","side":"X","slot":0}]})", R"({"pins":[{"number":"1","side":"L","slot":-1}]})",
                               R"({"width":900,"pins":[]})", R"({"pins":[{"side":"L","slot":0}]})"}) {
        Json bj = customPartSpecToJson(chip);
        bj["symbolLayout"] = Json::parse(badSym);
        threw = false;
        try {
            customPartSpecFromJson(bj);
        } catch (const JsonError&) {
            threw = true;
        }
        CHECK(threw);
    }

    // Stacked pins are one node: wiring one GND pin connects both, and the board must join both pads.
    Project p;
    auto& sch = p.schematic;
    const std::string id = p.addCustomPart(chip);
    int u = sch.addCustomComponent(id, "", {200, 0});
    int v = sch.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = sch.addComponent(ComponentKind::Ground, "", {0, 100});
    sch.connect({v, 0}, {u, sch.pinIndex(u, "1")});
    sch.connect({v, 1}, {g, 0});
    sch.connect({u, sch.pinIndex(u, "4")}, {g, 0});
    CHECK(sch.netOf({u, sch.pinIndex(u, "5")}) == sch.netOf({u, sch.pinIndex(u, "4")}));
    CHECK(sch.netOf({u, sch.pinIndex(u, "5")}) == sch.netOf({g, 0}));
    CHECK(sch.netOf({u, sch.pinIndex(u, "2")}) != sch.netOf({u, sch.pinIndex(u, "6")}));  // the gap joins nothing
    CHECK(sch.isPinConnected({u, sch.pinIndex(u, "5")}));
    // A stack with no wire is still open: ERC reports the floating ground pins.
    int u2 = sch.addCustomComponent(id, "", {400, 0});
    CHECK(sch.netOf({u2, sch.pinIndex(u2, "4")}) == sch.netOf({u2, sch.pinIndex(u2, "5")}));
    CHECK(!sch.isPinConnected({u2, sch.pinIndex(u2, "4")}) && !sch.isPinConnected({u2, sch.pinIndex(u2, "5")}));
    bool groundOpen = false;
    for (const auto& v : sch.runERC())
        groundOpen |= v.code == "ERC_FLOATING_COMPONENT" && !v.components.empty() && v.components[0] == u2;
    CHECK(groundOpen);

    // C API: auto-arrange and check.
    const std::string specJson = customPartSpecToJson(findStandardPart("CH340G")->spec).dump();
    char* error = nullptr;
    char* arranged = sieda_symbol_auto_arrange(specJson.c_str(), 1, &error);
    CHECK(arranged && !error);
    if (arranged) {
        Json aj = Json::parse(arranged);
        CHECK(aj.get("symbolLayout").get("pins").size() == 16);
        char* issues = sieda_check_symbol(arranged);
        CHECK(issues && Json::parse(issues).size() == 0);
        sieda_string_free(issues);
        sieda_string_free(arranged);
    }
    CHECK(sieda_symbol_auto_arrange(R"({"name":"X","pins":[]})", 1, &error) == nullptr && error);
    sieda_string_free(error);
    Json broken = customPartSpecToJson(chip);
    broken["symbolLayout"] = Json::parse(R"({"pins":[{"number":"1","side":"Q","slot":0}]})");
    char* invalid = sieda_check_symbol(broken.dump().c_str());
    CHECK(Json::parse(invalid).items().front().get("code").asString("") == "SYM_INVALID");
    sieda_string_free(invalid);
}

TEST(footprint_editor_checks_geometry_and_c_api) {
    // A two-pin part whose land pattern each case rewrites.
    CustomPartSpec two;
    two.name = "FP-EDGE";
    two.package.type = "CUSTOM";
    two.pins = {{"1", "A", PinType::Passive, ""}, {"2", "B", PinType::Passive, ""}};
    auto with = [&](std::vector<PackageSpec::Land> lands) {
        CustomPartSpec s = two;
        s.package.lands = std::move(lands);
        return s;
    };
    auto codes = [](const std::vector<LandIssue>& v) {
        std::multiset<std::string> out;
        for (const auto& i : v) out.insert(i.code);
        return out;
    };
    using L = PackageSpec::Land;
    // Circles: the gap is centre distance minus the radii (not the bounding boxes).
    CHECK(checkLandPattern(with({L(0, 0, 1, 1, 0, true), L(1.2, 0, 1, 1, 0, true)})).empty());
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1, 0, true), L(1.05, 0, 1, 1, 0, true)}))).count("LAND_GAP") == 1);
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1, 0, true), L(0.9, 0, 1, 1, 0, true)}))).count("LAND_OVERLAP") == 1);
    // Diagonal neighbours: corner-to-corner distance, 0.05 mm on each axis → 0.0707 mm.
    auto diag = checkLandPattern(with({L(0, 0, 1, 1), L(1.05, 1.05, 1, 1)}));
    CHECK(diag.size() == 1 && diag[0].code == "LAND_GAP" && diag[0].message.find("0.071") != std::string::npos);
    CHECK(diag[0].pads == std::vector<int>({1, 2}));
    // Touching edges are a zero gap, not an overlap; the minimum gap is configurable.
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1), L(1, 0, 1, 1)}))).count("LAND_GAP") == 1);
    CHECK(checkLandPattern(with({L(0, 0, 1, 1), L(1.15, 0, 1, 1)}), 0.1).empty());
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1), L(1.15, 0, 1, 1)}), 0.2)).count("LAND_GAP") == 1);
    // Two pads of one pin may overlap (a split exposed pad); two mechanical pads may not.
    CHECK(checkLandPattern(with({L(0, 0, 1, 1), L(3, 0, 1, 1), L(0.5, 0, 1, 1, 0, false, "1")})).empty());
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1), L(3, 0, 1, 1), L(6, 0, 2, 2, 1, true, "-"),
                                        L(6.5, 0, 2, 2, 1, true, "-")}))).count("LAND_OVERLAP") == 1);
    // Pin references are case-insensitive ("ep" is pin "EP").
    CustomPartSpec ep = two;
    ep.pins.push_back({"EP", "GND", PinType::PowerIn, ""});
    ep.package.lands = {L(-2, 0, 1, 1), L(2, 0, 1, 1), L(0, 0, 2, 2, 0, false, "ep")};
    CHECK(checkLandPattern(ep).empty());
    auto epPart = CustomPartRegistry::instance().registerPart(ep);
    CHECK(epPart->def.pins[static_cast<size_t>(epPart->footprint.pads[2].pinIndex)].number == "EP");
    // The pin reference survives JSON ([x, y, w, h, drill, round, pin]); "custom" normalises to CUSTOM.
    Json j = customPartSpecToJson(ep);
    CHECK(j.get("package").get("lands")[2].size() == 7);
    j["package"]["type"] = "custom";
    CustomPartSpec back = customPartSpecFromJson(j);
    CHECK(back.package.type == "CUSTOM" && back.package.lands[2].pin == "ep");
    CHECK(CustomPartRegistry::instance().registerPart(back)->footprint.pads.size() == 3);
    // A BGA converts with its ball names as pad pins: every ball keeps its pin.
    const CustomPartSpec bga = landPatternFromFootprint(findStandardPart("XC7A35T-1CSG324I")->spec);
    CHECK(bga.package.lands.size() == 324 && bga.package.lands[0].pin == "A1");
    CHECK(checkLandPattern(bga).empty());

    // C API: convert, check, and the error paths.
    const std::string ne555 = customPartSpecToJson(findStandardPart("NE555")->spec).dump();
    char* error = nullptr;
    char* converted = sieda_custom_part_land_pattern(ne555.c_str(), &error);
    CHECK(converted != nullptr && error == nullptr);
    if (converted) {
        Json cj = Json::parse(converted);
        CHECK(cj.get("package").get("type").asString("") == "CUSTOM" && cj.get("package").get("lands").size() == 8);
        char* issues = sieda_check_land_pattern(converted, 0.1);
        CHECK(issues && std::string(issues) == "[]");
        sieda_string_free(issues);
        sieda_string_free(converted);
    }
    CHECK(sieda_custom_part_land_pattern("{not json", &error) == nullptr && error != nullptr);
    sieda_string_free(error);
    char* invalid = sieda_check_land_pattern(R"({"name":"X","package":{"type":"CUSTOM","lands":[[0,0,1,1,2,1]]},"pins":[]})", 0.1);
    Json ij = Json::parse(invalid);
    CHECK(ij.size() == 1 && ij.items().front().get("code").asString("") == "LAND_INVALID");
    sieda_string_free(invalid);
    char* overlap = sieda_check_land_pattern(customPartSpecToJson(with({L(0, 0, 1, 1), L(0.5, 0, 1, 1)})).dump().c_str(), 0);
    Json oj = Json::parse(overlap);
    CHECK(oj.size() == 1 && oj.items().front().get("severity").asString("") == "error" && oj.items().front().get("pads").size() == 2);
    sieda_string_free(overlap);
}

TEST(footprint_editor_land_patterns) {
    auto spec = [](const char* name) { return findStandardPart(name)->spec; };
    auto padOf = [](const std::shared_ptr<const CustomPart>& part, const std::string& number) -> const PadDef* {
        for (const auto& pad : part->footprint.pads)
            if (pad.pinIndex >= 0 && part->def.pins[static_cast<size_t>(pad.pinIndex)].number == number) return &pad;
        return nullptr;
    };
    // Any generated footprint converts to an editable land pattern that draws the same pads on the same pins.
    for (const char* name : {"NE555", "LM7805", "STM32F405RGT6", "DRV8833PWPR", "LM2596S-5.0", "XC7A35T-1CSG324I",
                             "BMI088", "TMC2209-LA", "LM358DR", "AMS1117-3.3", "BSS138", "ESP32-WROOM-32E",
                             "Crystal_16MHz_3225", "BSC028N06LS3G"}) {
        const CustomPartSpec original = spec(name);
        const CustomPartSpec editable = landPatternFromFootprint(original);
        CHECK(usesLandPattern(editable.package.type));
        auto a = CustomPartRegistry::instance().registerPart(original);
        auto b = CustomPartRegistry::instance().registerPart(editable);
        CHECK(a->footprint.pads.size() == b->footprint.pads.size());
        bool same = true;
        for (size_t i = 0; i < a->footprint.pads.size(); ++i) {
            const PadDef &pa = a->footprint.pads[i], &pb = b->footprint.pads[i];
            same &= std::fabs(pa.offset.x - pb.offset.x) < 1e-9 && std::fabs(pa.offset.y - pb.offset.y) < 1e-9 &&
                    std::fabs(pa.size.x - pb.size.x) < 1e-9 && pa.throughHole == pb.throughHole && pa.round == pb.round &&
                    pa.pinIndex == pb.pinIndex;
        }
        if (!same) std::printf("    %s: converted pads differ\n", name);
        CHECK(same);
        CHECK(std::fabs(a->footprint.body.width - b->footprint.body.width) < 1e-9);
        // The pins are untouched (the app compares them field by field).
        CHECK(customPartSpecToJson(editable).get("pins").dump() == customPartSpecToJson(original).get("pins").dump());
        // Survives JSON (saved in the project library) and checks clean.
        CHECK(customPartSpecToJson(customPartSpecFromJson(customPartSpecToJson(editable))).dump() ==
              customPartSpecToJson(editable).dump());
        for (const auto& issue : checkLandPattern(editable))
            if (issue.severity == "error") std::printf("    %s: %s\n", name, issue.message.c_str());
    }
    // DIP keeps its plated through-holes; the TO-263 tab stays on the middle lead's pin; the QFN's exposed pad on EP.
    const CustomPartSpec dip = landPatternFromFootprint(spec("NE555"));
    CHECK(dip.package.lands[0].drill > 0 && dip.package.lands[1].round);
    const CustomPartSpec d2pak = landPatternFromFootprint(spec("LM2596S-5.0"));
    CHECK(d2pak.package.lands.size() == 6 && !d2pak.package.lands[5].pin.empty());
    const CustomPartSpec qfn = landPatternFromFootprint(spec("TMC2209-LA"));
    bool epOk = false;
    for (const auto& l : qfn.package.lands) epOk |= l.pin == "EP" || l.pin == "29";
    CHECK(epOk);
    CHECK(landPatternFromFootprint(spec("BMI088")).package.type == "LGA");  // already a land pattern
    bool emptyThrew = false;  // nothing to convert: no pins, no package
    try {
        landPatternFromFootprint(CustomPartSpec{});
    } catch (const JsonError&) {
        emptyThrew = true;
    }
    CHECK(emptyThrew);

    // Edit: move pad 2 onto pad 1 → overlap; nudge it close → gap warning; a pad with no pin; a pin with no pad.
    CustomPartSpec e = landPatternFromFootprint(spec("NE555"));
    CHECK(checkLandPattern(e).empty());
    auto codes = [](const std::vector<LandIssue>& v) {
        std::set<std::string> out;
        for (const auto& i : v) out.insert(i.code);
        return out;
    };
    CustomPartSpec moved = e;
    moved.package.lands[1].x = moved.package.lands[0].x;
    moved.package.lands[1].y = moved.package.lands[0].y + 0.5;
    CHECK(codes(checkLandPattern(moved)).count("LAND_OVERLAP"));
    moved.package.lands[1].y = moved.package.lands[0].y + moved.package.lands[0].h + 0.05;
    auto gapIssues = checkLandPattern(moved);
    CHECK(codes(gapIssues).count("LAND_GAP") && !codes(gapIssues).count("LAND_OVERLAP"));
    CustomPartSpec extra = e;
    extra.package.lands.push_back(PackageSpec::Land(0, 0, 1.0, 1.0, 0.8, true));  // 0.1 mm ring is fine
    CHECK(codes(checkLandPattern(extra)).count("LAND_NO_PIN"));
    extra.package.lands.back().drill = 0.9;
    CHECK(codes(checkLandPattern(extra)).count("LAND_ANNULAR"));
    extra.package.lands.back().pin = "-";  // mechanical
    CHECK(!codes(checkLandPattern(extra)).count("LAND_NO_PIN"));
    auto mech = CustomPartRegistry::instance().registerPart(extra);
    CHECK(mech->footprint.pads.back().pinIndex == -1 && mech->footprint.pads.back().throughHole);
    CustomPartSpec missing = e;
    missing.package.lands.pop_back();
    CHECK(codes(checkLandPattern(missing)).count("LAND_NO_PAD"));
    bool threw = false;
    try {
        CustomPartRegistry::instance().registerPart(missing);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
    // Two pads on one pin (a split ground) both connect to it and may touch.
    CustomPartSpec split = e;
    split.package.lands.push_back(PackageSpec::Land(0, 0, 1.0, 1.0, 0, false, "1"));
    auto sp = CustomPartRegistry::instance().registerPart(split);
    CHECK(sp->footprint.pads.back().pinIndex == padOf(sp, "1")->pinIndex);
    // Drill larger than the pad is refused on load.
    Json bad = customPartSpecToJson(e);
    bad["package"]["lands"] = Json::parse("[[0, 0, 1, 1, 1.2, 1]]");
    threw = false;
    try {
        customPartSpecFromJson(bad);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
}

/// The robot segments JSON lists every platform with its parts kit (the app reads the kits from it).
static bool json_roundtrip_platform_kits() {
    Project p;
    Json j = Json::parse(robotSegmentsJson(p).dump());
    for (const auto& plat : j.get("platforms").items()) {
        const auto& kit = robotPartKit(plat.get("id").asString(""));
        if (plat.get("kit").size() != kit.size() || kit.empty()) return false;
        if (plat.get("kit")[0].get("parts").size() != kit[0].parts.size()) return false;
    }
    return j.get("platforms").size() == 7;
}

TEST(robotics_part_kits_place_on_a_board) {
    // Every robot platform (rover, drone, arm, quadruped, humanoid, 3D printer, CNC) has a production parts kit; every
    // part in it is in the standard library on its real package, registers, and places and routes on a board.
    const char* platforms[] = {"rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"};
    std::set<std::string> all;
    for (const char* id : platforms) {
        CHECK(findRobotPlatform(id) != nullptr);
        const auto& kit = robotPartKit(id);
        CHECK(kit.size() >= 7);
        size_t count = 0;
        for (const auto& group : kit) {
            CHECK(!group.subsystem.empty() && !group.parts.empty());
            for (const auto& name : group.parts) {
                ++count;
                all.insert(name);
                const StandardPart* sp = findStandardPart(name);
                if (!sp) std::printf("    %s kit: missing %s\n", id, name.c_str());
                CHECK(sp != nullptr);
            }
        }
        CHECK(count >= 25);
    }
    CHECK(robotPartKit("toaster").empty());
    CHECK(all.size() >= 110);
    for (const auto& name : all) {
        const StandardPart* sp = findStandardPart(name);
        if (!sp) continue;
        bool ok = true;
        try {
            auto part = CustomPartRegistry::instance().registerPart(sp->spec);
            size_t mapped = 0;
            for (const auto& pad : part->footprint.pads) mapped += pad.pinIndex >= 0 ? 1 : 0;
            // Every pin has a pad (pins that share a pad — stacked supplies — are fine) and the courtyard holds them.
            ok = !part->def.pins.empty() && mapped >= 2 && part->footprint.courtyardW > 0 && part->footprint.courtyardH > 0;
        } catch (const std::exception& e) {
            std::printf("    %s: %s\n", name.c_str(), e.what());
            ok = false;
        }
        CHECK(ok);
    }
    CHECK(json_roundtrip_platform_kits());

    // A drone kit's flight-controller core placed and routed: STM32F405 + BMI088 + MS5611 + W25Q128 on one board.
    Project p;
    auto& sch = p.schematic;
    int x = 0;
    std::map<std::string, int> u;
    for (const char* name : {"STM32F405RGT6", "BMI088", "MS5611-01BA03", "W25Q128JVSIQ", "BSC028N06LS3G"}) {
        u[name] = sch.addCustomComponent(p.addCustomPart(findStandardPart(name)->spec), "", {static_cast<double>(x), 0});
        x += 300;
    }
    auto pin = [&](const char* part, const char* pinName) { return sch.pinIndex(u[part], pinName); };
    CHECK(pin("BMI088", "SDO1") >= 0 && pin("MS5611-01BA03", "SDO") >= 0);
    sch.connect({u["STM32F405RGT6"], pin("STM32F405RGT6", "PA6")}, {u["BMI088"], pin("BMI088", "SDO1")});
    sch.connect({u["STM32F405RGT6"], pin("STM32F405RGT6", "PA5")}, {u["MS5611-01BA03"], pin("MS5611-01BA03", "SCLK")});
    sch.connect({u["STM32F405RGT6"], pin("STM32F405RGT6", "PB3")}, {u["W25Q128JVSIQ"], pin("W25Q128JVSIQ", "CLK")});
    sch.connect({u["STM32F405RGT6"], pin("STM32F405RGT6", "PB0")}, {u["BSC028N06LS3G"], pin("BSC028N06LS3G", "G")});
    p.pcb.settings.width = 70;
    p.pcb.settings.height = 50;
    p.pcb.autoPlace(sch, true);
    const auto route = p.pcb.autoRoute(sch);
    CHECK(route.failed == 0);
}

TEST(exact_land_patterns_for_irregular_packages) {
    auto reg = [](const char* name) { return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec); };
    auto padOf = [](const std::shared_ptr<const CustomPart>& part, const char* number) -> const PadDef* {
        for (const auto& pad : part->footprint.pads)
            if (pad.pinIndex >= 0 && part->def.pins[static_cast<size_t>(pad.pinIndex)].number == number) return &pad;
        return nullptr;
    };
    // BMP280: 2 × 2.5 mm, clockwise pad numbering — pads 1…4 across the top, 5…8 back along the bottom.
    auto bmp = reg("BMP280");
    CHECK(bmp->footprint.pads.size() == 8);
    CHECK(bmp->footprint.label == "LGA-8");
    CHECK_NEAR(padOf(bmp, "1")->offset.x, -0.975, 1e-9);
    CHECK_NEAR(padOf(bmp, "1")->offset.y, -0.8, 1e-9);
    CHECK_NEAR(padOf(bmp, "4")->offset.x, 0.975, 1e-9);
    CHECK_NEAR(padOf(bmp, "5")->offset.y, 0.8, 1e-9);
    CHECK_NEAR(bmp->footprint.body.width, 2.0, 1e-9);
    CHECK_NEAR(bmp->footprint.body.depth, 2.5, 1e-9);
    // BMI088: 4.5 × 3 mm, seven pads per long side and one on each short side (pad 8 right, 16 left).
    auto bmi = reg("BMI088");
    CHECK(padOf(bmi, "8")->offset.x > 1.9 && std::fabs(padOf(bmi, "8")->offset.y) < 1e-9);
    CHECK(padOf(bmi, "16")->offset.x < -1.9);
    CHECK(padOf(bmi, "8")->size.x > padOf(bmi, "8")->size.y);  // short-side pad turned 90°
    // SuperSO8 MOSFET: the drain is one large pad, not four small ones.
    auto fet = reg("BSC028N06LS3G");
    CHECK(fet->footprint.pads.size() == 5);
    const PadDef* drain = padOf(fet, "5");
    CHECK(drain && drain->size.x * drain->size.y > 15.0 && fet->def.pins[static_cast<size_t>(drain->pinIndex)].name == "D");
    // SC-70-6 load switch on its 0.65 mm pitch.
    auto sw = reg("TPS22919DCKR");
    CHECK_NEAR(padOf(sw, "2")->offset.y - padOf(sw, "1")->offset.y, 0.65, 1e-9);
    // The land pattern survives JSON (it is what the app saves into the project library) and is validated.
    CustomPartSpec back = customPartSpecFromJson(customPartSpecToJson(findStandardPart("BMI088")->spec));
    CHECK(back.package.type == "LGA" && back.package.lands.size() == 16);
    CHECK_NEAR(back.package.lands[7].x, findStandardPart("BMI088")->spec.package.lands[7].x, 1e-12);
    CHECK_NEAR(back.package.bodyDepth, 3.0, 1e-12);
    CHECK(CustomPartRegistry::instance().registerPart(back)->id == bmi->id);
    CustomPartSpec bare = back;
    bare.package.lands.clear();
    bool threw = false;
    try {
        CustomPartRegistry::instance().registerPart(bare);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
    Json bad = customPartSpecToJson(back);
    bad["package"]["lands"] = Json::parse("[[0, 0, 0, 1]]");
    threw = false;
    try {
        customPartSpecFromJson(bad);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(robotics_segments_and_rules) {
    CHECK(robotPlatforms().size() == 7);  // + 3D printer and CNC machine
    CHECK(findRobotPlatform("quadruped") && findRobotPlatform("rover") && !findRobotPlatform("toaster"));
    auto codes = [](const Project& p) {
        std::set<std::string> out;
        for (const auto& v : roboticsChecks(p)) out.insert(v.code);
        return out;
    };
    // A 24 V motor drive: battery, low-side MOSFET switching an inductive load.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "24", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int bus = s.addComponent(ComponentKind::NetLabel, "VBUS", {60, -40});
    int load = s.addComponent(ComponentKind::Inductor, "1m", {120, 0});
    int q = s.addComponent(ComponentKind::NMOS, "AO3400", {200, 40});
    wire(s, v, "-", g, "GND");
    wire(s, bus, "N", load, "1");
    wire(s, load, "2", q, "D");
    wire(s, q, "S", g, "GND");
    int direct = s.connect({v, 0}, {bus, 0});
    p.schematicChanged();
    CHECK(roboticsChecks(p).empty());  // not a robot project yet
    p.robotPlatform = "arm";
    auto c1 = codes(p);
    CHECK(c1.count("REL_REVERSE_POLARITY") && c1.count("REL_NO_FUSE"));
    // Series Schottky + fuse on the input clears both.
    s.removeWire(direct);
    int d = s.addComponent(ComponentKind::Diode, "SS34", {20, -40});
    int f = s.addComponent(ComponentKind::Fuse, "5", {40, -40});
    wire(s, v, "+", d, "A");
    wire(s, d, "K", f, "1");
    wire(s, f, "2", bus, "N");
    p.schematicChanged();
    auto c2 = codes(p);
    CHECK(!c2.count("REL_REVERSE_POLARITY") && !c2.count("REL_NO_FUSE"));

    // CAN bus: termination required between CANH and CANL.
    int h = s.addComponent(ComponentKind::NetLabel, "CANH", {300, 0});
    int l = s.addComponent(ComponentKind::NetLabel, "CANL", {300, 60});
    int rt = s.addComponent(ComponentKind::Resistor, "1k", {340, 30});
    wire(s, h, "N", rt, "1");
    wire(s, l, "N", rt, "2");
    p.schematicChanged();
    CHECK(codes(p).count("REL_BUS_TERMINATION"));
    s.find(rt)->value = "120";
    CHECK(!codes(p).count("REL_BUS_TERMINATION"));
    bool canPair = false;
    for (auto [a, b] : differentialPairs(s)) canPair |= s.nets()[static_cast<size_t>(a)].name == "CANH";
    CHECK(canPair);

    // Analog ground: one star tie, not zero and not two.
    int agnd = s.addComponent(ComponentKind::NetLabel, "AGND", {400, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {440, 0});
    wire(s, agnd, "N", r1, "1");
    wire(s, r1, "2", bus, "N");
    p.schematicChanged();
    CHECK(codes(p).count("REL_STAR_GROUND"));  // floating
    int fb = s.addComponent(ComponentKind::Inductor, "1u", {440, 60});
    wire(s, agnd, "N", fb, "1");
    wire(s, fb, "2", g, "GND");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_STAR_GROUND"));
    int fb2 = s.addComponent(ComponentKind::Inductor, "1u", {480, 60});
    wire(s, agnd, "N", fb2, "1");
    wire(s, fb2, "2", g, "GND");
    p.schematicChanged();
    CHECK(codes(p).count("REL_STAR_GROUND"));  // ground loop

    // Segment report: seven segments, power complete only once a TVS joins the fuse and reverse protection.
    auto segs = robotSegments(p);
    CHECK(segs.size() == 7);
    CHECK(segs[0].id == "power" && segs[0].status == "partial");
    CHECK(robotSegmentsJson(p)["segments"].size() == 7);
    Project saved = Project::fromJson(p.toJson());
    CHECK(saved.robotPlatform == "arm");

    // Thermal vias at an SMD power FET on a laid-out board.
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.width = 60;
    p.pcb.settings.height = 40;
    p.pcb.autoPlace(s, true);
    CHECK(p.pcb.autoRoute(s).failed == 0);
    const size_t before = p.pcb.vias.size();
    const int added = addThermalVias(p, q, 6);
    CHECK(added > 0 && p.pcb.vias.size() == before + static_cast<size_t>(added));
    int drainNet = s.netOf({q, 1});
    for (size_t i = before; i < p.pcb.vias.size(); ++i) CHECK(p.pcb.vias[i].net == drainNet);
    CHECK(p.pcb.ratsnest(s).empty());
    int errors = 0;
    for (const auto& e : p.pcb.runDRC(s)) errors += e.severity == Severity::Error;
    CHECK(errors == 0);
}

TEST(automotive_ecu_segments_and_rules) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    CHECK(ecuTypes().size() == 6);
    CHECK(findEcuType("bcm") && findEcuType("gateway") && !findEcuType("toaster"));
    auto codes = [](const Project& p) {
        std::set<std::string> out;
        for (const auto& v : automotiveChecks(p))
            if (v.severity != Severity::Info) out.insert(v.code);
        return out;
    };
    // Battery straight into an LM7805 that powers an ATmega328P.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "13.5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int reg = s.addCustomComponent(partId("LM7805"), "", {100, 0});
    int mcu = s.addCustomComponent(partId("ATmega328P"), "", {300, 0});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", reg, "1");
    wire(s, reg, "2", g, "GND");
    wire(s, reg, "3", mcu, "7");
    wire(s, mcu, "8", g, "GND");
    p.schematicChanged();
    CHECK(automotiveChecks(p).empty());  // not an ECU project yet
    p.ecuType = "bcm";
    auto c1 = codes(p);
    CHECK(c1.count("REL_REVERSE_POLARITY") && c1.count("REL_NO_FUSE") && c1.count("REL_EMI_FILTER"));
    CHECK(c1.count("REL_COLD_CRANK"));  // 7805: 5 V + 2 V dropout > 6 V crank
    CHECK(c1.count("REL_WATCHDOG"));

    // Watchdog supervisor: RESET to the MCU reset and WDI kicked by a GPIO clears the rule.
    int wd = s.addCustomComponent(partId("TPS3823-33-Q1"), "", {300, 200});
    wire(s, wd, "VDD", mcu, "7");
    wire(s, wd, "GND", g, "GND");
    wire(s, wd, "RESET", mcu, "1");
    p.schematicChanged();
    CHECK(codes(p).count("REL_WATCHDOG"));  // WDI not kicked yet
    wire(s, wd, "WDI", mcu, "5");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_WATCHDOG"));

    // CAN bus lines: termination and bus ESD.
    int h = s.addComponent(ComponentKind::NetLabel, "CANH", {500, 0});
    int l = s.addComponent(ComponentKind::NetLabel, "CANL", {500, 60});
    int rt = s.addComponent(ComponentKind::Resistor, "120", {540, 30});
    wire(s, h, "N", rt, "1");
    wire(s, l, "N", rt, "2");
    p.schematicChanged();
    auto c2 = codes(p);
    CHECK(!c2.count("REL_BUS_TERMINATION") && c2.count("REL_BUS_ESD") && c2.count("REL_CLOCK"));
    int e1 = s.addComponent(ComponentKind::Diode, "PESD1CAN", {560, 0});
    int e2 = s.addComponent(ComponentKind::Diode, "PESD1CAN", {560, 60});
    wire(s, e1, "K", h, "N");
    wire(s, e1, "A", g, "GND");
    wire(s, e2, "K", l, "N");
    wire(s, e2, "A", g, "GND");
    int y = s.addCustomComponent(partId("Crystal_8MHz"), "", {300, -100});
    wire(s, y, "1", mcu, "9");
    wire(s, y, "2", mcu, "10");
    p.schematicChanged();
    auto c3 = codes(p);
    CHECK(!c3.count("REL_BUS_ESD") && !c3.count("REL_CLOCK"));

    // Six segments; a body ECU without a LIN transceiver is incomplete on the network segment.
    auto segs = ecuSegments(p);
    CHECK(segs.size() == 6);
    CHECK(segs[0].id == "shield" && segs[0].status == "partial");
    CHECK(segs[3].id == "network");
    bool linMissing = false;
    for (const auto& i : segs[3].items) linMissing |= i.label == "LIN node" && !i.ok;
    CHECK(linMissing);
    p.ecuType = "powertrain";
    const auto powertrain = ecuSegments(p);
    for (const auto& i : powertrain[3].items)
        if (i.label == "LIN node") CHECK(i.ok);
    Json j = ecuSegmentsJson(p);
    CHECK(j["segments"].size() == 6 && j["platforms"].size() == 6);
    Project saved = Project::fromJson(p.toJson());
    CHECK(saved.ecuType == "powertrain");
    // The automotive industry profile alone turns the checks on.
    p.ecuType.clear();
    CHECK(automotiveChecks(p).empty());
    p.industry = "automotive";
    CHECK(!automotiveChecks(p).empty());
}

TEST(aerospace_segments_rules_and_isolated_power) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    CHECK(aerospaceMissions().size() == 5);
    CHECK(findAerospaceMission("leo") && findAerospaceMission("commercial") && !findAerospaceMission("mars"));
    auto codes = [](const Project& p) {
        std::set<std::string> out;
        for (const auto& v : aerospaceChecks(p))
            if (v.severity != Severity::Info) out.insert(v.code);
        return out;
    };
    // A 28 V bus into an isolated DC-DC: the secondary regulates and the primary draws P_out / efficiency.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "28", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 200});
    int rtn = s.addComponent(ComponentKind::NetLabel, "RTN", {0, 100});
    int dc = s.addCustomComponent(partId("ISO-DCDC-2805S"), "", {200, 0});
    int load = s.addComponent(ComponentKind::Resistor, "10", {400, 0});
    int bleed = s.addComponent(ComponentKind::Resistor, "10meg", {100, 150});
    wire(s, v, "+", dc, "+VIN");
    wire(s, v, "-", rtn, "N");
    wire(s, dc, "-VIN", rtn, "N");
    wire(s, dc, "+VOUT", load, "1");
    wire(s, load, "2", g, "GND");
    wire(s, dc, "-VOUT", g, "GND");
    wire(s, bleed, "1", rtn, "N");
    wire(s, bleed, "2", g, "GND");
    p.schematicChanged();
    {
        DcResult dcr = Simulator(s).dcOperatingPoint();
        CHECK(dcr.converged);
        CHECK_NEAR(netV(s, dcr, load, "1"), 5.0, 0.05);
        double supply = 0;
        for (const auto& d : dcr.devices)
            if (d.componentId == v) supply = d.current;
        // 2.5 W out at 82 % from 28 V ≈ 109 mA (+ 2 mA quiescent).
        CHECK_NEAR(supply, 2.5 / 0.82 / 28.0 + 0.002, 0.003);
        CHECK(std::fabs(netV(s, dcr, rtn, "N")) < 0.01);  // the bleeder carries no load current
    }
    CHECK(aerospaceChecks(p).empty());  // not an aerospace project yet
    p.industry = "space";
    CHECK(!aerospaceChecks(p).empty() && codes(p).empty());  // industry alone: advisory only
    p.aerospaceMission = "leo";
    auto c1 = codes(p);
    CHECK(!c1.count("REL_ISOLATED_POWER") && c1.count("REL_LIGHTNING_TVS") && c1.count("REL_SSPC"));

    // Three commercial MCUs straight on the rail: rad-hard, MRAM, latch-up and TMR voter findings.
    std::vector<int> lanes;
    for (int i = 0; i < 3; ++i) {
        int u = s.addCustomComponent(partId("ATmega328P"), "", {600.0 + 200 * i, 0});
        wire(s, u, "7", load, "1");
        wire(s, u, "8", g, "GND");
        lanes.push_back(u);
    }
    p.schematicChanged();
    auto c2 = codes(p);
    CHECK(c2.count("REL_RAD_HARD") && c2.count("REL_MRAM") && c2.count("REL_SEL_PROTECTION") && c2.count("REL_SEL_WATCHDOG"));
    CHECK(c2.count("REL_TMR_VOTER"));
    int voter = s.addCustomComponent(partId("74HC10"), "", {600, 300});
    wire(s, voter, "VCC", load, "1");
    wire(s, voter, "GND", g, "GND");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_TMR_VOTER"));

    // LVDS receiver: 100 Ω across each used pair.
    int rx = s.addCustomComponent(partId("UT54LVDS032"), "", {600, 500});
    int dp = s.addComponent(ComponentKind::NetLabel, "SPW_DIN_P", {700, 500});
    int dn = s.addComponent(ComponentKind::NetLabel, "SPW_DIN_N", {700, 560});
    wire(s, rx, "RIN1+", dp, "N");
    wire(s, rx, "RIN1-", dn, "N");
    p.schematicChanged();
    CHECK(codes(p).count("REL_LVDS_TERMINATION"));
    int rt = s.addComponent(ComponentKind::Resistor, "100", {740, 530});
    wire(s, rt, "1", dp, "N");
    wire(s, rt, "2", dn, "N");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_LVDS_TERMINATION"));

    // Five segments; MRAM missing, SpaceWire present for a LEO mission.
    auto segs = aerospaceSegments(p);
    CHECK(segs.size() == 5);
    CHECK(segs[0].id == "compute" && segs[0].status != "complete");
    CHECK(segs[3].id == "avionics");
    for (const auto& i : segs[3].items)
        if (i.label == "SpaceWire / LVDS") CHECK(i.ok);
    Json j = aerospaceSegmentsJson(p);
    CHECK(j["segments"].size() == 5 && j["platforms"].size() == 5);
    Project saved = Project::fromJson(p.toJson());
    CHECK(saved.aerospaceMission == "leo");
    // The isolated model survives a save / load of the part spec.
    CustomPartSpec spec = customPartSpecFromJson(customPartSpecToJson(findStandardPart("ISO-DCDC-2805S")->spec));
    CHECK(spec.model.regulator.isolated() && spec.model.regulator.inReturn == "2");
    CHECK_NEAR(spec.model.regulator.efficiency, 0.82, 1e-9);
}

TEST(naval_segments_and_rules) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    CHECK(navalPlatforms().size() == 5 && findNavalPlatform("submarine") && !findNavalPlatform("yacht"));
    auto codes = [](const Project& p) {
        std::set<std::string> out;
        for (const auto& v : navalChecks(p))
            if (v.severity != Severity::Info) out.insert(v.code);
        return out;
    };
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "28", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int u = s.addCustomComponent(partId("ATmega328P"), "", {200, 0});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", u, "7");
    wire(s, u, "8", g, "GND");
    p.schematicChanged();
    CHECK(navalChecks(p).empty());
    p.industry = "marine";
    CHECK(!navalChecks(p).empty() && codes(p).empty());  // advisory only
    p.navalPlatform = "combatant";
    auto c1 = codes(p);
    for (const char* code : {"REL_SHIP_ISOLATION", "REL_SURGE_FRONT_END", "REL_SALT_FOG_COATING", "REL_HERMETIC",
                             "REL_SHOCK_UNDERFILL", "REL_SHOCK_MOUNTING", "REL_SHOCK_SUBSTRATE", "REL_FIBER_LINK"})
        CHECK(c1.count(code));
    // Board build: coating, underfill, thick polyimide, close mounting holes.
    p.pcb.settings.coating = "parylene";
    p.pcb.settings.underfill = true;
    p.pcb.settings.thickness = 2.4;
    p.pcb.settings.layerCount = 8;
    p.pcb.settings.material = "polyimide";
    p.pcb.settings.clearance = 0.25;
    for (double x : {10.0, 70.0})
        for (double y : {10.0, 70.0}) p.pcb.settings.holes.push_back({{x, y}, 3.2, 6.4});
    auto c2 = codes(p);
    for (const char* code : {"REL_SALT_FOG_COATING", "REL_SHOCK_UNDERFILL", "REL_SHOCK_MOUNTING", "REL_SHOCK_SUBSTRATE",
                             "REL_ECM_SPACING"})
        CHECK(!c2.count(code));
    // A radar LNA needs a PIN limiter on its input.
    int lna = s.addCustomComponent(partId("LNA-MMIC"), "", {400, 0});
    int in = s.addComponent(ComponentKind::NetLabel, "LNA_IN", {360, 0});
    wire(s, lna, "RFIN", in, "N");
    wire(s, lna, "GND", g, "GND");
    p.schematicChanged();
    CHECK(codes(p).count("REL_LNA_LIMITER"));
    int d = s.addComponent(ComponentKind::Diode, "BAP64-02", {380, 60});
    wire(s, d, "A", in, "N");
    wire(s, d, "K", g, "GND");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_LNA_LIMITER"));
    auto segs = navalSegments(p);
    CHECK(segs.size() == 5 && segs[2].id == "mechanical" && segs[2].status == "complete");
    CHECK(navalSegmentsJson(p)["segments"].size() == 5);
    CHECK(Project::fromJson(p.toJson()).navalPlatform == "combatant");
    CHECK(Project::fromJson(p.toJson()).pcb.settings.underfill);
}

TEST(galvanic_domains_and_isolation_barrier) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    // System side: 5 V → isolated converter → patient side with a resistor load; an isolator crosses too.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int dc = s.addCustomComponent(partId("ISO-DCDC-MED"), "", {200, 0});
    int pg = s.addComponent(ComponentKind::NetLabel, "PGND", {300, 100});
    int load = s.addComponent(ComponentKind::Resistor, "1k", {400, 0});
    int ecg = s.addComponent(ComponentKind::NetLabel, "ECG_RA", {500, 0});
    int rin = s.addComponent(ComponentKind::Resistor, "10k", {500, 60});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", dc, "+VIN");
    wire(s, dc, "-VIN", g, "GND");
    wire(s, dc, "+VOUT", load, "1");
    wire(s, dc, "-VOUT", pg, "N");
    wire(s, load, "2", pg, "N");
    wire(s, ecg, "N", rin, "1");
    wire(s, rin, "2", pg, "N");
    p.schematicChanged();
    const GalvanicDomains doms = galvanicDomains(s);
    const int sys = doms.domainOfNet(s.netOf({v, 0})), pat = doms.domainOfNet(s.netOf({load, 0}));
    CHECK(sys >= 0 && pat >= 0 && sys != pat);
    CHECK(doms.domainOfNet(s.netOf({rin, 0})) == pat);
    CHECK(doms.domainOfComponent(s, *s.find(dc)) == -1 && doms.domainOfComponent(s, *s.find(load)) == pat);
    {
        DcResult dcr = Simulator(s).dcOperatingPoint();  // 3.0–5.5 V in, isolated 5 V out
        CHECK(dcr.converged);
        CHECK_NEAR(netV(s, dcr, load, "1") - netV(s, dcr, load, "2"), 5.0, 0.05);
    }
    // Medical rules: patient barrier present; 2 × MOPP creepage asked of the board.
    p.medicalClass = "cf";
    std::set<std::string> c1;
    for (const auto& x : medicalChecks(p))
        if (x.severity != Severity::Info) c1.insert(x.code);
    CHECK(!c1.count("REL_PATIENT_ISOLATION") && c1.count("REL_MOPP_CREEPAGE") && c1.count("REL_DEFIB_PROTECTION"));
    // A Y-capacitor across the barrier merges the domains: the patient is no longer isolated.
    int ycap = s.addComponent(ComponentKind::Capacitor, "4.7n", {300, 200});
    wire(s, ycap, "1", pg, "N");
    wire(s, ycap, "2", g, "GND");
    p.schematicChanged();
    std::set<std::string> c2;
    for (const auto& x : medicalChecks(p))
        if (x.severity != Severity::Info) c2.insert(x.code);
    CHECK(c2.count("REL_PATIENT_ISOLATION"));
    s.removeComponent(ycap);
    p.schematicChanged();

    // Layout: with an 8 mm barrier the domains are placed, routed and poured 8 mm apart and DRC proves it.
    p.pcb.settings.isolationGap = 8;
    p.pcb.settings.layerCount = 2;
    p.pcb.settings.width = 60;
    p.pcb.settings.height = 40;
    p.pcb.zones.push_back({"GND", 1, false, 0});
    p.pcb.autoPlace(s, true);
    CHECK(p.pcb.autoRoute(s).failed == 0);
    int gapErrors = 0;
    for (const auto& x : p.pcb.runDRC(s)) gapErrors += x.code == "DRC_ISOLATION_GAP";
    CHECK(gapErrors == 0);
    // A patient-side part moved next to the system side is caught.
    const Vec2 next = s.find(v)->pcb.position + Vec2{3, 0};
    for (auto& c : s.mutableComponents())
        if (c.id == load) c.pcb.position = next;
    p.pcb.clearRouting();
    int after = 0;
    for (const auto& x : p.pcb.runDRC(s)) after += x.code == "DRC_ISOLATION_GAP";
    CHECK(after > 0);
    CHECK(Project::fromJson(p.toJson()).pcb.settings.isolationGap == 8);
    CHECK(Project::fromJson(p.toJson()).medicalClass == "cf");
}

TEST(tamper_mesh_over_secure_element) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "3.3", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int se = s.addCustomComponent(partId("SECURE-MCU"), "", {200, 0});
    int c1 = s.addComponent(ComponentKind::Capacitor, "100n", {300, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {300, 100});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {300, 200});
    int ma = s.addComponent(ComponentKind::NetLabel, "TAMPER_MESH_A", {400, 0});
    int mb = s.addComponent(ComponentKind::NetLabel, "TAMPER_MESH_B", {400, 100});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", se, "VDD");
    wire(s, se, "GND", g, "GND");
    wire(s, c1, "1", se, "VDD");
    wire(s, c1, "2", g, "GND");
    wire(s, r1, "1", se, "NRST");
    wire(s, r1, "2", se, "VDD");
    wire(s, r2, "1", se, "TAMPER1");
    wire(s, r2, "2", g, "GND");
    wire(s, se, "MESH_A_DRV", ma, "N");
    wire(s, se, "MESH_A_SNS", ma, "N");
    wire(s, se, "MESH_B_DRV", mb, "N");
    wire(s, se, "MESH_B_SNS", mb, "N");
    p.schematicChanged();
    const std::string seRef = s.find(se)->ref;

    // Retail rules: a countertop terminal's secure element without a mesh, backup cell or tamper switches.
    p.retailDevice = "countertop";
    auto retailCodes = [&] {
        std::set<std::string> c;
        for (const auto& x : retailChecks(p))
            if (x.severity != Severity::Info) c.insert(x.code);
        return c;
    };
    {
        const auto c = retailCodes();
        CHECK(!c.count("REL_SECURE_ELEMENT") && c.count("REL_TAMPER_MESH") && c.count("REL_KEY_BATTERY"));
        CHECK(!c.count("REL_TAMPER_SWITCHES"));  // TAMPER1 is wired (pull-down R2)
        CHECK(c.count("REL_RETAIL_COATING") == 0);  // countertop: coating advice is Info
    }
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.width = 50;
    p.pcb.settings.height = 45;
    p.pcb.tamperMeshes.push_back({seRef, "TAMPER_MESH_A", "TAMPER_MESH_B", 1, 2, 2.0});
    CHECK(!retailCodes().count("REL_TAMPER_MESH"));
    {
        const auto segs = retailSegments(p);
        CHECK(segs.size() == 4 && segs[0].id == "security");
        CHECK(segs[0].items[0].ok && segs[0].items[1].ok && !segs[0].items[2].ok);  // no backup cell yet
    }
    p.pcb.autoPlace(s, true);
    const RouteStats st = p.pcb.autoRoute(s);
    CHECK(st.failed == 0);
    const auto geo = p.pcb.tamperMeshGeometry(s, p.pcb.pads(s));
    CHECK(geo.size() == 1 && geo[0].error.empty());
    // The secure element sits under the mesh; both serpentines are laid on their inner layers.
    const Rect area = geo[0].region;
    CHECK(area.contains(s.find(se)->pcb.position));
    int stripesA = 0, stripesB = 0;
    for (const auto& t : p.pcb.tracks) {
        stripesA += t.net == geo[0].netA && t.layer == 1;
        stripesB += t.net == geo[0].netB && t.layer == 2;
        // Nothing else crosses the mesh layers inside the secure area.
        if ((t.layer == 1 || t.layer == 2) && t.net != geo[0].netA && t.net != geo[0].netB)
            CHECK(segmentRectDistance(t.a, t.b, area) > 0);
    }
    CHECK(stripesA > 10 && stripesB > 10);
    for (const auto& via : p.pcb.vias) CHECK(!area.contains(via.position) || via.net == geo[0].netA || via.net == geo[0].netB);
    std::set<std::string> codes;
    for (const auto& x : p.pcb.runDRC(s)) codes.insert(x.code);
    CHECK(!codes.count("DRC_TAMPER_MESH") && !codes.count("DRC_TAMPER_MESH_BREACH") && !codes.count("DRC_UNROUTED"));
    CHECK(!codes.count("DRC_CLEARANCE"));
    // A via drilled through the secure area is a breach.
    Via drill;
    drill.net = s.netOf({g, 0});
    drill.position = s.find(se)->pcb.position;
    p.pcb.vias.push_back(drill);
    std::set<std::string> breach;
    for (const auto& x : p.pcb.runDRC(s)) breach.insert(x.code);
    CHECK(breach.count("DRC_TAMPER_MESH_BREACH"));
    // Persisted with the project; a 2-layer board cannot carry a mesh.
    const Project q = Project::fromJson(p.toJson());
    CHECK(q.pcb.tamperMeshes.size() == 1 && q.pcb.tamperMeshes[0].netB == "TAMPER_MESH_B");
    p.pcb.settings.layerCount = 2;
    std::set<std::string> two;
    for (const auto& x : p.pcb.runDRC(s)) two.insert(x.code);
    CHECK(two.count("DRC_TAMPER_MESH"));
}

TEST(retail_printer_and_peripheral_rules) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    Project p;
    auto& s = p.schematic;
    p.industry = "retail";
    p.retailDevice = "printer";
    int v = s.addComponent(ComponentKind::VoltageSource, "24", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int head = s.addCustomComponent(partId("TPH-80MM"), "", {200, 0});
    int q = s.addComponent(ComponentKind::NMOS, "AO3400", {300, 100});
    int sol = s.addComponent(ComponentKind::NetLabel, "CUTTER_SOL", {400, 100});
    int j = s.addComponent(ComponentKind::Connector, "CUTTER", {500, 100});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", head, "1");
    wire(s, head, "3", g, "GND");
    wire(s, q, "D", sol, "N");
    wire(s, q, "S", g, "GND");
    wire(s, j, "2", sol, "N");
    wire(s, j, "1", v, "+");
    p.schematicChanged();
    auto codes = [&] {
        std::set<std::string> c;
        for (const auto& x : retailChecks(p))
            if (x.severity != Severity::Info) c.insert(x.code);
        return c;
    };
    {
        const auto c = codes();
        CHECK(c.count("REL_TPH_BULK") && c.count("REL_PRINTER_MOTOR") && c.count("REL_SOLENOID_FLYBACK"));
        CHECK(!c.count("REL_SECURE_ELEMENT"));  // a printer carries no payment keys
    }
    int bulk = s.addComponent(ComponentKind::Capacitor, "1000u", {200, 200});
    int fly = s.addComponent(ComponentKind::Diode, "1N4007", {400, 200});
    wire(s, bulk, "1", v, "+");
    wire(s, bulk, "2", g, "GND");
    wire(s, fly, "A", sol, "N");
    wire(s, fly, "K", v, "+");
    p.schematicChanged();
    {
        const auto c = codes();
        CHECK(!c.count("REL_TPH_BULK") && !c.count("REL_SOLENOID_FLYBACK") && c.count("REL_PRINTER_MOTOR"));
    }
    CHECK(Project::fromJson(p.toJson()).retailDevice == "printer");
    CHECK(findIndustry("retail") && findIndustry("appliance"));
    // A buck converter with a shared ground is simulated by efficiency and is not a galvanic barrier.
    Project b;
    auto& t = b.schematic;
    int vin = t.addComponent(ComponentKind::VoltageSource, "24", {0, 0});
    int gg = t.addComponent(ComponentKind::Ground, "", {0, 100});
    int buck = t.addCustomComponent(partId("LM2596-5.0"), "", {200, 0});
    int l = t.addComponent(ComponentKind::Inductor, "33u", {300, 0});
    int load = t.addComponent(ComponentKind::Resistor, "10", {400, 0});
    wire(t, vin, "-", gg, "GND");
    wire(t, vin, "+", buck, "VIN");
    wire(t, buck, "GND", gg, "GND");
    wire(t, buck, "ON_OFF", gg, "GND");
    wire(t, buck, "OUTPUT", l, "1");
    wire(t, l, "2", load, "1");
    wire(t, buck, "FEEDBACK", load, "1");
    wire(t, load, "2", gg, "GND");
    b.schematicChanged();
    DcResult dc = Simulator(t).dcOperatingPoint();
    CHECK(dc.converged);
    CHECK_NEAR(netV(t, dc, load, "1"), 5.0, 0.05);
    // 0.5 A out at 5 V and 85 %: ≈ 0.12 A from 24 V instead of 0.5 A through a linear pass element.
    CHECK(reading(dc, vin) && std::fabs(reading(dc, vin)->current) < 0.2);
    CHECK(galvanicDomains(t).domainOfNet(t.netOf({vin, 0})) == galvanicDomains(t).domainOfNet(t.netOf({load, 0})));
}

TEST(appliance_mains_rules_and_voltage_spacing) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    // 230 VAC through a fuse and a bridge into a bulk capacitor and a resistive load; a 3.3 V logic island beside it.
    Project p;
    auto& s = p.schematic;
    p.industry = "appliance";
    p.applianceType = "kitchen";
    int ac = s.addComponent(ComponentKind::ACSource, "SIN(0 325 50)", {0, 0});
    int f = s.addComponent(ComponentKind::Fuse, "2", {100, -100});
    int lbl = s.addComponent(ComponentKind::NetLabel, "AC_L", {50, -100});
    int d1 = s.addComponent(ComponentKind::Diode, "1N4007", {200, -100});
    int d2 = s.addComponent(ComponentKind::Diode, "1N4007", {200, 0});
    int d3 = s.addComponent(ComponentKind::Diode, "1N4007", {200, 100});
    int d4 = s.addComponent(ComponentKind::Diode, "1N4007", {200, 200});
    int g = s.addComponent(ComponentKind::Ground, "", {300, 300});
    int bulk = s.addComponent(ComponentKind::Capacitor, "10u", {300, 0});
    int load = s.addComponent(ComponentKind::Resistor, "100k", {400, 0});
    int v = s.addComponent(ComponentKind::VoltageSource, "3.3", {600, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {700, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {700, 100});
    wire(s, ac, "+", lbl, "N");
    wire(s, lbl, "N", f, "1");
    wire(s, f, "2", d1, "A");
    wire(s, f, "2", d3, "K");
    wire(s, ac, "-", d2, "A");
    wire(s, ac, "-", d4, "K");
    wire(s, d1, "K", bulk, "1");
    wire(s, d2, "K", bulk, "1");
    wire(s, d3, "A", g, "GND");
    wire(s, d4, "A", g, "GND");
    wire(s, bulk, "2", g, "GND");
    wire(s, load, "1", bulk, "1");
    wire(s, load, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", g, "GND");
    p.schematicChanged();

    // The transient puts L, N and the bus at mains potential (not their DC point of 0 V); logic stays low.
    const auto ranges = netVoltageRanges(s);
    const int bus = s.netOf({bulk, 0}), logic = s.netOf({r1, 1});
    CHECK(ranges.at(bus).second > 250 && ranges.at(s.netOf({f, 1})).second > 250);
    CHECK(std::fabs(ranges.at(logic).second) < 5);
    {
        std::set<std::string> c;
        for (const auto& x : applianceChecks(p))
            if (x.severity != Severity::Info) c.insert(x.code);
        CHECK(!c.count("REL_MAINS_FUSE") && c.count("REL_MAINS_MOV") && c.count("REL_X_CAP") && c.count("REL_OFFLINE_SUPPLY"));
    }
    int mov = s.addCustomComponent(partId("S10K275"), "", {150, -50});
    int x2 = s.addCustomComponent(partId("X2-100N-275VAC"), "", {150, 50});
    wire(s, mov, "1", f, "2");
    wire(s, mov, "2", ac, "-");
    wire(s, x2, "1", f, "2");
    wire(s, x2, "2", ac, "-");
    p.schematicChanged();
    {
        std::set<std::string> c;
        for (const auto& x : applianceChecks(p))
            if (x.severity != Severity::Info) c.insert(x.code);
        CHECK(!c.count("REL_MAINS_MOV") && !c.count("REL_X_CAP"));
        const auto segs = applianceSegments(p);
        CHECK(segs.size() == 4 && segs[0].id == "mains" && segs[0].items[0].ok && segs[0].items[1].ok == false);
    }

    // Mains spacing: coated board → 0.8 mm for 325 V, applied by fencing the mains nets, not the whole board.
    p.pcb.settings.layerCount = 2;
    p.pcb.settings.width = 70;
    p.pcb.settings.height = 50;
    p.pcb.settings.coating = "acrylic";
    const SpacingDomains sd = spacingDomains(s, p.pcb.settings);
    CHECK_NEAR(sd.hvGap, 0.8, 1e-9);
    CHECK(sd.highVoltage[static_cast<size_t>(bus)] && !sd.highVoltage[static_cast<size_t>(logic)]);
    CHECK(sd.domains.domainOfNet(bus) != sd.domains.domainOfNet(logic));
    p.pcb.autoPlace(s, true);
    CHECK(p.pcb.autoRoute(s).failed == 0);
    CHECK(p.pcb.settings.clearance < 0.5);  // the logic keeps its design-rule clearance
    int hv = 0;
    for (const auto& x : p.pcb.runDRC(s)) hv += x.code == "DRC_HV_CLEARANCE" || x.code == "DRC_SHORT";
    CHECK(hv == 0);
    CHECK(Project::fromJson(p.toJson()).applianceType == "kitchen");
}

TEST(memory_design_segments_and_checks) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    auto codes = [](const Project& p, bool warningsOnly = true) {
        std::set<std::string> c;
        for (const auto& x : memoryChecks(p))
            if (!warningsOnly || x.severity != Severity::Info) c.insert(x.code);
        return c;
    };
    CHECK(memoryDesignTypes().size() == 5);
    for (const char* id : {"sdram", "ddr", "lpddr", "dimm", "rdimm"}) CHECK(findMemoryDesignType(id) != nullptr);
    CHECK(findMemoryDesignType("ddr6") == nullptr);
    CHECK(findIndustry("memory") && findIndustry("memory")->rulePreset == "HDI / Fine-Pitch BGA (IPC-2226)");

    // The DRAM parts: SDR SDRAM in TSOP-II, DDR3L / DDR4 in a depopulated 9 × 16 FBGA laid out by ball name.
    const auto sdr = CustomPartRegistry::instance().registerPart(findStandardPart("MT48LC16M16A2TG-6A")->spec);
    CHECK(sdr->footprint.pads.size() == 54 && sdr->footprint.label == "TSSOP-54 0.8mm pitch");
    for (const char* name : {"MT41K256M16HA-125", "AS4C256M16D3-12BCN", "MT40A512M16LY-062E"}) {
        const auto ddr = CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec);
        CHECK(ddr->footprint.pads.size() == 96);
        for (const auto& pad : ddr->footprint.pads) CHECK(pad.pinIndex >= 0);
        const PadDef* a1 = nullptr;
        const PadDef* t9 = nullptr;
        for (const auto& pad : ddr->footprint.pads) {
            const std::string& n = ddr->def.pins[static_cast<size_t>(pad.pinIndex)].number;
            if (n == "A1") a1 = &pad;
            if (n == "T9") t9 = &pad;
        }
        // A1 top-left and T9 bottom-right of a 9-column, 16-row grid at 0.8 mm.
        CHECK(a1 && t9);
        if (a1 && t9) {
            CHECK_NEAR(a1->offset.x, -3.2, 1e-9);
            CHECK_NEAR(a1->offset.y, -6.0, 1e-9);
            CHECK_NEAR(t9->offset.x, 3.2, 1e-9);
            CHECK_NEAR(t9->offset.y, 6.0, 1e-9);
        }
        CHECK(ddr->footprint.body.depth > ddr->footprint.body.width);
    }
    // A full square grid keeps its row-major layout (the FPGA is unchanged).
    const auto fpga = CustomPartRegistry::instance().registerPart(findStandardPart("XC7A35T-1CSG324I")->spec);
    CHECK(fpga->footprint.pads.size() == 324 && fpga->footprint.body.width == fpga->footprint.body.depth);

    // SDR SDRAM beside a controller, wired data-only at first.
    Project p;
    auto& s = p.schematic;
    CHECK(memoryChecks(p).empty() && !isMemoryProject(p));
    p.industry = "memory";
    int mcu = s.addCustomComponent(partId("STM32H743IIT6"), "", {0, 0});
    int ram = s.addCustomComponent(partId("MT48LC16M16A2TG-6A"), "", {600, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {300, 400});
    const char* data[][2] = {{"DQ0", "PD14"}, {"DQ1", "PD15"}, {"DQ2", "PD0"}, {"DQ3", "PD1"}, {"DQ4", "PE7"}, {"DQ5", "PE8"},
                             {"DQ6", "PE9"}, {"DQ7", "PE10"}};
    for (auto& d : data) wire(s, ram, d[0], mcu, d[1]);
    wire(s, ram, "CLK", mcu, "PG8");
    wire(s, ram, "VSS", g, "GND");
    p.schematicChanged();
    for (const auto& v : memoryChecks(p)) CHECK(v.severity == Severity::Info);  // industry only: advice
    p.memoryDesign = "sdram";
    {
        const auto c = codes(p);
        CHECK(c.count("REL_DRAM_DECOUPLING") && c.count("REL_DRAM_LAYERS"));
        CHECK(!c.count("REL_DRAM_CONTROLLER"));  // the STM32 is found on the data lines
        CHECK(!c.count("REL_DDR_ZQ") && !c.count("REL_DDR_VTT"));  // no DDR rules on SDR
        const auto info = codes(p, false);
        CHECK(info.count("REL_DRAM_CLOCK_SERIES") && info.count("REL_DRAM_LENGTH_MATCH"));
    }
    // Decoupling, a named data bus and a 4-layer board with a ground plane clear them.
    int vdd = s.addComponent(ComponentKind::NetLabel, "+3V3", {300, -300});
    wire(s, ram, "VDD", vdd, "N");
    wire(s, ram, "VDDQ", vdd, "N");
    for (int i = 0; i < 4; ++i) {
        int c = s.addComponent(ComponentKind::Capacitor, i == 0 ? "10u" : "100n", {400.0 + 40 * i, -200});
        wire(s, c, "1", vdd, "N");
        wire(s, c, "2", g, "GND");
    }
    for (int i = 0; i < 8; ++i) {
        int l = s.addComponent(ComponentKind::NetLabel, ("SD_DQ" + std::to_string(i)).c_str(), {500, 50.0 * i});
        wire(s, l, "N", ram, data[i][0]);
    }
    p.pcb.settings.layerCount = 4;
    p.pcb.zones.push_back({"GND", 1, true, 0});
    p.schematicChanged();
    {
        const auto c = codes(p, false);
        CHECK(!c.count("REL_DRAM_DECOUPLING") && !c.count("REL_DRAM_BULK") && !c.count("REL_DRAM_LAYERS"));
        CHECK(!c.count("REL_DRAM_LENGTH_MATCH") && !c.count("REL_DRAM_REFERENCE_PLANE"));
        const auto segs = memorySegments(p);
        CHECK(segs.size() == 5 && segs[0].id == "power" && segs[1].id == "clock" && segs[2].id == "data" &&
              segs[3].id == "config" && segs[4].id == "layout");
        CHECK(segs[0].status == "complete");
        CHECK(segs[2].items[0].ok);  // data lanes matched
        CHECK(!segs[1].items[1].ok);  // address lines still open
    }
    CHECK(Project::fromJson(p.toJson()).memoryDesign == "sdram");
    Json bad = p.toJson();
    bad["memoryDesign"] = "ddr9";
    CHECK(Project::fromJson(bad).memoryDesign.empty());

    // DDR3L memory-down: ZQ, VREF, RESET_n, VTT and the CK pair are checked.
    Project q;
    auto& d = q.schematic;
    q.memoryDesign = "ddr";
    int dram = d.addCustomComponent(partId("MT41K256M16HA-125"), "", {0, 0});
    int gq = d.addComponent(ComponentKind::Ground, "", {0, 400});
    wire(d, dram, "V_{SS}", gq, "GND");
    q.schematicChanged();
    {
        const auto c = codes(q);
        CHECK(c.count("REL_DDR_ZQ") && c.count("REL_DDR_VREF") && c.count("REL_DDR_RESET") && c.count("REL_DDR_VTT"));
        CHECK(c.count("REL_DRAM_CONTROLLER"));
    }
    int zq = d.addComponent(ComponentKind::Resistor, "100", {100, 100});
    wire(d, dram, "ZQ", zq, "1");
    wire(d, zq, "2", gq, "GND");
    q.schematicChanged();
    bool wrongValue = false;
    for (const auto& v : memoryChecks(q)) wrongValue |= v.code == "REL_DDR_ZQ" && v.message.find("240") != std::string::npos;
    CHECK(wrongValue);
    d.find(zq)->value = "240 1%";
    int reset = d.addComponent(ComponentKind::Resistor, "10k", {100, 200});
    wire(d, dram, "nRESET", reset, "1");
    wire(d, reset, "2", gq, "GND");
    int vtt = d.addComponent(ComponentKind::NetLabel, "VTT", {200, 0});
    int rt = d.addComponent(ComponentKind::Resistor, "39", {200, 100});
    wire(d, dram, "A0", rt, "1");
    wire(d, rt, "2", vtt, "N");
    int ck = d.addComponent(ComponentKind::Resistor, "100", {300, 100});
    wire(d, dram, "CK", ck, "1");
    wire(d, dram, "nCK", ck, "2");
    int vref = d.addComponent(ComponentKind::NetLabel, "VREF", {300, 200});
    int ra = d.addComponent(ComponentKind::Resistor, "1k", {300, 250});
    int rb = d.addComponent(ComponentKind::Resistor, "1k", {300, 300});
    int cv = d.addComponent(ComponentKind::Capacitor, "100n", {350, 250});
    wire(d, dram, "V_{REFCA}", vref, "N");
    wire(d, dram, "V_{REFDQ}", vref, "N");
    wire(d, ra, "1", vref, "N");
    wire(d, rb, "1", vref, "N");
    wire(d, rb, "2", gq, "GND");
    wire(d, cv, "1", vref, "N");
    wire(d, cv, "2", gq, "GND");
    q.schematicChanged();
    {
        const auto c = codes(q);
        CHECK(!c.count("REL_DDR_ZQ") && !c.count("REL_DDR_RESET") && !c.count("REL_DDR_VTT") && !c.count("REL_DDR_CK_TERM"));
        CHECK(!c.count("REL_DDR_VREF"));
        const auto segs = memorySegments(q);
        CHECK(segs[2].items[2].ok);  // ZQ calibration resistor
    }

    // A DDR5 module: SPD EEPROM with pull-ups and a 1.2–1.27 mm board.
    Project m;
    m.memoryDesign = "dimm";
    m.schematic.addCustomComponent(partId("MT40A512M16LY-062E"), "", {0, 0});
    m.schematicChanged();
    {
        const auto c = codes(m);
        CHECK(c.count("REL_SPD") && c.count("REL_DIMM_THICKNESS"));
    }
    m.pcb.settings.thickness = 1.27;
    m.schematic.addCustomComponent(partId("AT24CS02-SSHM-T"), "", {300, 0});
    m.schematicChanged();
    {
        const auto c = codes(m);
        CHECK(!c.count("REL_SPD") && c.count("REL_SPD_PULLUPS") && !c.count("REL_DIMM_THICKNESS"));
    }

    // The C API: set / reject a type and read the segment report.
    SiedaProject* api = sieda_project_new("mem");
    CHECK(sieda_set_memory_design(api, "rdimm") == 1);
    CHECK(sieda_set_memory_design(api, "nope") == 0);
    char* json = sieda_memory_segments_json(api);
    const Json report = Json::parse(json);
    CHECK(report.get("platform").asString("") == "rdimm" && report.get("applies").asBool());
    CHECK(report.get("platforms").size() == 5 && report.get("segments").size() == 5);
    sieda_string_free(json);
    CHECK(sieda_set_memory_design(api, "") == 1);
    sieda_project_free(api);
}

namespace {
/// DDR3L / FPGA / generic routing test boards: HDI rules, the given planes, auto-placed and fitted.
int routeErrors(Project& p, int layers, std::initializer_list<std::pair<const char*, int>> planes, RouteStats& st) {
    p.pcb.settings.applyPreset("HDI / Fine-Pitch BGA (IPC-2226)");
    p.pcb.settings.width = 70;
    p.pcb.settings.height = 50;
    p.pcb.settings.layerCount = layers;
    for (const auto& [net, layer] : planes) p.pcb.zones.push_back({net, layer, true, 0});
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.fitBoardToComponents(p.schematic, 2.5);
    st = p.pcb.autoRoute(p.schematic);
    int errors = 0;
    for (const auto& v : p.pcb.runDRC(p.schematic)) errors += v.severity == Severity::Error;
    return errors;
}
}  // namespace

TEST(bga_fanout_fpga_and_ddr3_memory_down) {
    // An Artix-7 (324-ball, 0.8 mm) talking to a DDR3L x16 (96-ball) over 50 lines, on 8 layers with GND / 1.0 V /
    // 1.35 V planes: every ball fans out (dogbone) and the bus routes completely with no DRC error.
    auto norm = [](std::string s) {
        std::string o;
        for (char c : s)
            if (c != '{' && c != '}') o += c;
        if (o.size() > 2 && o[0] == 'V' && o[1] == '_') o.erase(1, 1);
        return o;
    };
    Project p;
    auto& sch = p.schematic;
    int f = sch.addCustomComponent(p.addCustomPart(findStandardPart("XC7A35T-1CSG324I")->spec), "", {0, 0});
    int m = sch.addCustomComponent(p.addCustomPart(findStandardPart("MT41K256M16HA-125")->spec), "", {600, 0});
    int g = sch.addComponent(ComponentKind::Ground, "", {0, 400});
    int v33 = sch.addComponent(ComponentKind::NetLabel, "+3V3", {0, -400});
    int v1 = sch.addComponent(ComponentKind::NetLabel, "+1V0", {100, -400});
    int v135 = sch.addComponent(ComponentKind::NetLabel, "+1V35", {200, -400});
    const auto& fpins = sch.find(f)->def().pins;
    const auto& mpins = sch.find(m)->def().pins;
    std::vector<int> io;
    for (int i = 0; i < static_cast<int>(fpins.size()); ++i) {
        const std::string& n = fpins[static_cast<size_t>(i)].name;
        if (n.rfind("IO_", 0) == 0) io.push_back(i);
        else if (n == "GND" || n == "GNDADC_0") sch.connect({f, i}, {g, 0});
        else if (n == "VCCO_34" || n == "VCCO_35") sch.connect({f, i}, {v135, 0});
        else if (n.rfind("VCCO", 0) == 0 || n == "VCCADC_0" || n == "VCCBATT_0" || n == "VCCAUX") sch.connect({f, i}, {v33, 0});
        else if (n == "VCCINT" || n == "VCCBRAM") sch.connect({f, i}, {v1, 0});
    }
    int k = 0;
    for (int i = 0; i < static_cast<int>(mpins.size()); ++i) {
        const std::string n = norm(mpins[static_cast<size_t>(i)].name);
        if (n == "VSS" || n == "VSSQ") sch.connect({m, i}, {g, 0});
        else if (n == "VDD" || n == "VDDQ") sch.connect({m, i}, {v135, 0});
        else if (n != "NC" && n.rfind("VREF", 0) != 0) sch.connect({m, i}, {f, io[static_cast<size_t>(k++)]});
    }
    CHECK(k == 50);
    RouteStats st;
    CHECK(routeErrors(p, 8, {{"GND", 1}, {"+1V0", 5}, {"+1V35", 6}}, st) == 0);
    CHECK(st.failed == 0 && st.routed == st.connections && st.connections > 150);
    // Every signal ball has its fan-out via in the gap between four balls (not in the pad: VIPPO is off).
    int fanned = 0;
    for (const auto& pad : p.pcb.pads(sch)) {
        if (pad.componentId != m || pad.net < 0) continue;
        for (const auto& v : p.pcb.vias)
            if (v.net == pad.net && std::fabs((v.position - pad.position).length() - 0.8 / std::sqrt(2.0)) < 0.01) {
                ++fanned;
                break;
            }
    }
    CHECK(fanned > 60);
    CHECK(p.pcb.settings.routingGrid == 0.25);  // the finer BGA grid is only used while routing
}

TEST(every_package_type_autoroutes) {
    // One library part of every package type (DIP, SOIC, TSSOP, QFN, LQFP, SON, SOT-23, SOT-223, TO-220, TO-263,
    // Multiwatt, LGA, BGA, modules, headers, crystals, discs): its pins wired to resistors, auto-routed on 4 layers
    // with a GND plane — every connection routes and DRC is clean.
    std::map<std::string, const StandardPart*> byType;
    for (const auto& sp : standardParts()) byType.emplace(sp.spec.package.type, &sp);
    CHECK(byType.size() >= 20);
    for (const auto& [type, sp] : byType) {
        Project p;
        auto& sch = p.schematic;
        int u = sch.addCustomComponent(p.addCustomPart(sp->spec), "", {0, 0});
        int g = sch.addComponent(ComponentKind::Ground, "", {0, 400});
        const auto& pins = sch.find(u)->def().pins;
        int sig = 0;
        for (int i = 0; i < static_cast<int>(pins.size()) && sig < 16; ++i) {
            if (pins[static_cast<size_t>(i)].type == static_cast<int>(PinType::NoConnect)) continue;
            int r = sch.addComponent(ComponentKind::Resistor, "1k", {300.0 + 60 * (sig % 8), 60.0 * (sig / 8)});
            sch.connect({u, i}, {r, 0});
            sch.connect({r, 1}, {g, 0});
            ++sig;
        }
        RouteStats st;
        const int errors = routeErrors(p, 4, {{"GND", 1}}, st);
        if (errors != 0 || st.failed != 0)
            std::printf("    %s (%s): %d/%d routed, %d DRC errors\n", type.c_str(), sp->spec.name.c_str(), st.routed,
                        st.connections, errors);
        CHECK(errors == 0 && st.failed == 0);
    }
}

TEST(library_footprints_keep_pads_apart) {
    // No footprint puts copper of two different pins closer than 0.1 mm (an exposed pad or a corner pad touching a
    // pin pad would short them on the board).
    for (const auto& sp : standardParts()) {
        auto part = CustomPartRegistry::instance().registerPart(sp.spec);
        const auto& pads = part->footprint.pads;
        double worst = 1e9;
        for (size_t i = 0; i < pads.size(); ++i)
            for (size_t j = i + 1; j < pads.size(); ++j) {
                if (pads[i].pinIndex >= 0 && pads[i].pinIndex == pads[j].pinIndex) continue;
                const Rect a = Rect::centered(pads[i].offset, pads[i].size.x, pads[i].size.y);
                const Rect b = Rect::centered(pads[j].offset, pads[j].size.x, pads[j].size.y);
                double gap = std::hypot(std::max({0.0, b.x0 - a.x1, a.x0 - b.x1}), std::max({0.0, b.y0 - a.y1, a.y0 - b.y1}));
                if (pads[i].round && pads[j].round)
                    gap = std::max(0.0, (pads[i].offset - pads[j].offset).length() - pads[i].size.x / 2 - pads[j].size.x / 2);
                worst = std::min(worst, gap);
            }
        if (worst < 0.099) std::printf("    %s: pads %.3f mm apart\n", sp.spec.name.c_str(), worst);
        CHECK(worst >= 0.099);
    }
}

TEST(collinear_segments_are_not_a_crossing) {
    // Two dogbone stubs on one 45° diagonal, 0.57 mm apart: with fused multiply-add the orientation products come out
    // as tiny values of either sign, which used to read as a crossing (a false DRC short on Apple silicon).
    const Vec2 a{16.75, 16.45}, b{17.15, 16.05}, c{15.95, 17.25}, d{16.35, 16.85};
    CHECK(!segmentsIntersect(a, b, c, d));
    CHECK(std::fabs(segmentSegmentDistance(a, b, c, d) - (d - a).length()) < 1e-9);
    CHECK(segmentSegmentDistance(a, b, {16.95, 16.25}, {17.35, 15.85}) < 1e-9);  // overlapping collinear: touching
    CHECK(segmentsIntersect({0, 0}, {1, 1}, {0, 1}, {1, 0}));                     // a real crossing still is one
}
