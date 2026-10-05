// SiEDA Core — IBIS (I/O Buffer Information Specification, ANSI/EIA-656-B; IBIS 4.x / 5.x keywords) import and the
// driver / receiver models signal-integrity analysis runs with.
//
// An IBIS file describes each I/O buffer behaviourally: its die capacitance (C_comp), output I-V curves ([Pullup],
// [Pulldown], clamps), edge rate ([Ramp] dV/dt, or [Rising Waveform] / [Falling Waveform] tables), receiver thresholds
// and the package parasitics of every pin. SiEDA reduces a buffer to the linear model a transmission-line solve needs:
// a Thévenin source with a linear 10–90 % ramp, an output resistance taken from the I-V slope near the rails, C_comp at
// the die and the package R / L / C. Typ / min / max columns are kept so any corner can be selected (IBIS "min" is the
// weak / slow corner, "max" the strong / fast one).
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "sieda/Json.hpp"

namespace sieda {

/// A typ / min / max value; missing columns ("NA") are NaN.
struct IbisTriple {
    double typ, min, max;
    IbisTriple();
    /// Column 0 = typ, 1 = min, 2 = max; falls back to typ, then any present column. NaN when none is present.
    double at(int corner) const;
    bool valid() const;
};

struct IbisIvPoint {
    double v = 0;
    IbisTriple i;
};

struct IbisWaveform {
    double rFixture = 50, vFixture = 0;
    std::vector<std::pair<double, IbisTriple>> points;  // time (s), voltage (V)
};

struct IbisModel {
    std::string name;
    std::string type;  // Model_type: "Input", "Output", "I/O", "3-state", "Open_drain", "Terminator", …
    IbisTriple cComp;
    IbisTriple voltageRange, pullupReference, pulldownReference;
    double vinl, vinh;  // receiver thresholds (V), NaN when not given
    IbisTriple rampRiseDv, rampRiseDt, rampFallDv, rampFallDt;  // [Ramp]: 20–80 % swing and its time
    double rLoad = 50;
    std::vector<IbisIvPoint> pullup, pulldown, gndClamp, powerClamp;
    std::vector<IbisWaveform> rising, falling;
    IbisModel();
    /// Output-capable buffer (Output, I/O, 3-state, open drain / sink / source, ECL outputs).
    bool canDrive() const;
};

struct IbisPin {
    std::string pin, signal, model;
    double rPin, lPin, cPin;  // NaN when not given
    IbisPin();
};

struct IbisComponent {
    std::string name, manufacturer;
    IbisTriple rPkg, lPkg, cPkg;
    std::vector<IbisPin> pins;
};

struct IbisFile {
    std::string version;   // [IBIS Ver]
    std::string fileName;  // [File Name]
    std::vector<IbisComponent> components;
    std::vector<IbisModel> models;
    std::map<std::string, std::vector<std::string>> selectors;  // [Model Selector] name → model names
    std::vector<std::string> warnings;
    /// Model by name (case-insensitive); a [Model Selector] name resolves to its first model.
    const IbisModel* findModel(const std::string& name) const;
};

struct IbisError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Parses an .ibs file. Keywords are case-insensitive, '_' and ' ' are equivalent, the comment character is '|'
/// unless [Comment Char] changes it, numbers take IBIS scale suffixes (T G M k m u n p f) with units ignored and "NA"
/// for missing columns. Unknown keywords (IBIS 5.x [Composite Current], [ISSO PU], [Driver Schedule], …) are skipped.
/// Throws IbisError when the text has no [Model].
IbisFile parseIbis(const std::string& text);

/// The linear driver / receiver model the transmission-line solve uses.
struct DriverModel {
    std::string id;      // "lvcmos33" (logic family) or "ibis:<model name>"
    std::string name;    // "LVCMOS 3.3 V", "DQ_34_1600 (MT41K256M16)"
    std::string source;  // "family" or "ibis"
    std::string type;    // "output", "io", "input"
    double vHigh = 3.3;      // output high level / supply (V); the low level is 0 V
    double riseTime = 1e-9;  // 10–90 % (s)
    double fallTime = 1e-9;  // 90–10 % (s)
    double rOut = 30;        // output resistance (Ω)
    double cComp = 3e-12;    // die capacitance at the pad (F)
    double cIn = 3e-12;      // input capacitance as a receiver (F)
    double vih = 2.0, vil = 0.8;  // receiver thresholds (V)
    double rTerm = 0;   // on-die termination as a receiver (Ω to vTerm); 0 = none
    double vTerm = 0;   // termination voltage (V)
    double rPkg = 0, lPkg = 0, cPkg = 0;  // package parasitics of the pin (Ω, H, F)
    std::string note;
    bool canDrive() const { return type != "input"; }
};

/// Built-in defaults when no IBIS model is assigned: 5 V CMOS, LVCMOS 3.3 / 2.5 / 1.8 / 1.2 V, SSTL-15 (DDR3),
/// SSTL-135 (DDR3L), POD-12 (DDR4) and USB 2.0 high-speed. Typical datasheet values (JESD8 family standards).
const std::vector<DriverModel>& logicFamilies();
const DriverModel* findLogicFamily(const std::string& id);

/// Reduces an IBIS [Model] to a DriverModel at `corner` ("typ", "min" = weak/slow, "max" = strong/fast):
///  • edge: [Ramp] gives the 20–80 % time; a linear edge's 10–90 % time is 4/3 of it. Without [Ramp] the
///    [Rising/Falling Waveform] table's 20 % and 80 % crossings are used;
///  • rOut: least-squares slope of the [Pulldown] curve (V vs I) between 0 and 30 % of the supply, averaged with the
///    [Pullup] curve over the same range (its voltage column is referenced to Vcc, IBIS §6);
///  • C_comp, Vinl / Vinh (default 30 % / 70 % of the supply), [Voltage Range];
///  • package R / L / C from the pin's R_pin / L_pin / C_pin, else the component's [Package] typ values.
DriverModel driverFromIbis(const IbisModel& model, const std::string& corner = "typ", const IbisComponent* component = nullptr,
                           const IbisPin* pin = nullptr);

Json driverModelToJson(const DriverModel& m);
DriverModel driverModelFromJson(const Json& j);

/// A parsed file as JSON for the import preview: version, components with pins, models with their derived driver.
Json ibisFileJson(const IbisFile& f, const std::string& corner = "typ");

}  // namespace sieda
