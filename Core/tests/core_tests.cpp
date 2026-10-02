// SiEDA core unit tests — dependency-free; run via `ctest` or directly.
#include <map>
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
#include "sieda/Export.hpp"
#include "sieda/Fabrication.hpp"
#include "sieda/Firmware.hpp"
#include "sieda/Industry.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/Project.hpp"
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
    CHECK(empty.stages.size() == 7);
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
    CHECK(j["stages"].size() == 7);

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
    CHECK(industryProfiles().size() == 13);
    for (const char* id : {"general", "robotics", "uav", "power", "automotive", "rf", "space", "marine", "industrial",
                           "medical", "defence", "networking", "vlsi"}) {
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
    CHECK(perGroup["Microcontrollers · Arm"] == 10);
    CHECK(perGroup["Microcontrollers · STMicroelectronics"] == 10);
    CHECK(perGroup["Microcontrollers · Texas Instruments"] == 10);
    CHECK(perGroup["Microcontrollers · Microchip"] == 12);  // + ATmega328P and ATtiny85
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
