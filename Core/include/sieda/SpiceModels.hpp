// SiEDA Core — SPICE model import.
//
// Reads vendor model text (.lib / .mod / .cir / .sub): `.model` cards (D, NPN, PNP, NMOS, PMOS, NJF, PJF, SW), `.subckt`
// definitions, `.param` and `.func`, with PSpice / LTspice / ngspice syntax (continuation lines, `;` and `$` comments,
// `{expressions}`, `PARAMS:`, `POLY(n)`, `VALUE={…}`, `TABLE`, AKO models). A model or subcircuit is flattened into
// simulator primitives (R C L K V I E F G H B D Q M J); a part carries the model text, the model name and a pin map
// (Component::spice), and the simulator replaces the part's built-in model with it. The parser never throws: bad
// input gives diagnostics with line numbers. Guide: docs/SIMULATION.md.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sieda {

struct SpiceDiagnostic {
    enum class Level { Info = 0, Warning = 1, Error = 2 };
    Level level = Level::Warning;
    int line = 0;  // 1-based line in the text (0: not tied to a line)
    std::string message;
};
const char* spiceLevelName(SpiceDiagnostic::Level level);  // "info", "warning", "error"

/// Parses a SPICE number: "1.5k", "10MEG", "2u2"-free SPICE scale factors (T G MEG K M MIL U N P F, case-insensitive)
/// followed by optional unit letters ("100pF", "1kohm"). False for anything else (no exceptions).
bool parseSpiceNumber(const std::string& text, double& value);

// ------------------------------------------------------------------ expressions

/// Expression tree of a `{…}` value, a B-source or a VALUE= controlled source. Leaves are numbers, parameters,
/// node voltages V(a) / V(a,b), source currents I(Vx) and `time`.
struct SpiceExpr {
    enum class Op {
        Number, Param, Voltage, Current, Time, Neg, Not, Add, Sub, Mul, Div, Pow, Lt, Gt, Le, Ge, Eq, Ne, And, Or,
        Cond, Call
    };
    Op op = Op::Number;
    double value = 0;
    std::string name;   // parameter / function / node / source name (upper case)
    std::string name2;  // second node of V(a,b)
    std::vector<std::shared_ptr<SpiceExpr>> args;
    int slot = -1, slot2 = -1;  // simulator control slots of a voltage / current leaf
};
using SpiceExprPtr = std::shared_ptr<SpiceExpr>;

/// Parses an expression (without the surrounding braces). nullptr with `error` on a syntax error.
SpiceExprPtr parseSpiceExpression(const std::string& text, std::string& error);
/// Evaluates an expression whose voltage / current leaves have slots: `controls[slot]`. Parameters must have been
/// substituted (flattening does that); an unknown name evaluates to 0.
double evalSpiceExpression(const SpiceExpr& e, const double* controls, double time);

// ------------------------------------------------------------------ library

struct SpiceModelCard {
    std::string name;         // upper case
    std::string displayName;  // as written
    std::string type;         // "D", "NPN", "PNP", "NMOS", "PMOS", "NJF", "PJF", "SW"
    std::map<std::string, std::string> params;  // upper-case name → value text (number or {expression})
    std::string ako;          // AKO base model
    int line = 0;
    int scope = -1;           // index of the enclosing subckt, -1 = global
    std::string text;         // the card as written (continuations joined)
};

struct SpiceCard {
    int line = 0;
    std::string text;  // continuation lines joined, comments removed
};

struct SpiceSubckt {
    std::string name;         // upper case
    std::string displayName;
    std::vector<std::string> ports;                            // upper case, in order
    std::vector<std::pair<std::string, std::string>> params;   // PARAMS: defaults (name → expression text)
    std::vector<SpiceCard> body;                               // element, .param cards (not nested definitions)
    std::vector<std::string> blockText;                        // the definition as written, .subckt … .ends
    int line = 0;
    int scope = -1;           // enclosing subckt (nested definition), -1 = global
    bool closed = false;      // has its .ends
};

struct SpiceFunc {
    std::string name;
    std::vector<std::string> args;
    std::string body;
    int line = 0;
    std::string text;
};

struct SpiceLibrary {
    std::vector<SpiceModelCard> models;
    std::vector<SpiceSubckt> subckts;
    std::vector<std::pair<std::string, std::string>> params;  // global .param (name → expression text), in order
    std::vector<std::string> paramText;                        // the global .param cards as written
    std::vector<SpiceFunc> funcs;
    std::vector<SpiceDiagnostic> diagnostics;

    /// Model / subckt visible from `scope` (a subckt index, -1 global): the innermost definition wins.
    const SpiceModelCard* findModel(const std::string& name, int scope = -1) const;
    int findSubckt(const std::string& name, int scope = -1) const;  // index, -1 when absent
};

/// Parses model text. Text over 8 MB or 200 000 lines is refused with an error diagnostic.
SpiceLibrary parseSpiceLibrary(const std::string& text);

// ------------------------------------------------------------------ flattening

/// Prefix of the flattened names of a model's ports (followed by the port index): a control character, which no
/// parsed node name can contain.
constexpr const char* kSpicePortTag = "\x01";

struct SpicePrimitive {
    char type = 'R';                 // R C L K V I E F G H B D Q M J
    std::string name;                // hierarchical: "Q3", "X1.Q3"
    std::vector<std::string> nodes;  // flattened node names: "0" ground, kSpicePortTag + index for the model's ports
    double value = 0;                // R, C, L; V / I DC value; K coupling factor; E/F/G/H linear gain
    std::string waveform;            // V / I: "PULSE(v1 v2 td tr tf pw per)", "SIN(…)", "PWL(…)", "EXP(…)" (numbers)
    // Controlled sources: controlling voltages as node pairs (E, G) or controlling V sources (F, H, POLY F/H; for K
    // the two inductors). `coeffs` is the polynomial (SPICE POLY order: constant, linear, then products).
    std::vector<std::pair<std::string, std::string>> controlNodes;
    std::vector<std::string> controlSources;
    std::vector<double> coeffs;
    bool poly = false;
    SpiceExprPtr expr;               // B source, VALUE=, TABLE input, switch
    bool voltageOutput = false;      // E/H/B V=: a voltage; G/F/B I=: a current
    std::vector<std::pair<double, double>> table;  // TABLE points, ascending input
    // Semiconductors: the model card (parameters as numbers, evaluated at flattening) and instance parameters.
    std::string modelType;           // "D", "NPN", …
    std::string modelName;
    std::map<std::string, double> model;
    int level = 1;
    double area = 1, multiplier = 1, w = 0, l = 0, ad = 0, as = 0, pd = 0, ps = 0;
};

struct SpiceFlatCircuit {
    bool ok = false;
    std::string kind;                // "model" or "subckt"
    std::string type;                // model type, or "SUBCKT"
    std::string name;                // display name
    std::vector<std::string> ports;  // port names in order (a .model: its terminals, e.g. C B E)
    std::vector<SpicePrimitive> prims;
    std::vector<SpiceDiagnostic> diagnostics;
};

/// Flattens the model or subckt `name` (case-insensitive). A `.model` gives one device whose ports are its terminals
/// in SPICE order (D: A K, BJT: C B E, MOSFET: D G S B, JFET: D G S); a subckt gives its primitives with its ports.
SpiceFlatCircuit flattenSpiceModel(const SpiceLibrary& library, const std::string& name);

/// Entries a library offers: models and subckts with their ports, for a model browser.
struct SpiceLibraryEntry {
    std::string name, kind, type;     // kind "model" / "subckt"; type "D", "NPN", …, "SUBCKT"
    std::vector<std::string> ports;
    int line = 0;
};
std::vector<SpiceLibraryEntry> spiceLibraryEntries(const SpiceLibrary& library);

/// The text needed to simulate `name`: global .param / .func cards, the definition and every model and subckt it
/// uses, as written. Empty when `name` is not defined.
std::string extractSpiceModel(const SpiceLibrary& library, const std::string& name);

/// Default port → pin assignment for a model on a part, as pin-map text (see SpiceModelRef::pins): port names that
/// match pin names (C/B/E, D/G/S, A/K, IN+/IN-/OUT, V+/V-…), else the part's pins in order when the counts agree.
/// `pins` are the part's pins in order as (number, name); the number is empty for built-in symbols, whose pins the map
/// names. An op-amp symbol (IN+, IN-, OUT) with a five-port macromodel gets ideal ±15 V rails ("dc:15 dc:-15").
/// Empty when no mapping is evident.
std::string defaultSpicePinMap(const SpiceFlatCircuit& flat, const std::vector<std::pair<std::string, std::string>>& pins,
                               bool isOpAmpSymbol);

/// Monomials of a SPICE POLY(dims) source in coefficient order (constant, x1…xn, x1², x1·x2, …, then cubic …): each
/// term lists the indices of the controlling values it multiplies. `count` terms.
std::vector<std::vector<int>> spicePolyTerms(int dims, size_t count);

/// Shared parse / flatten cache (keyed by text and name): Monte Carlo and sweeps rebuild the circuit many times.
std::shared_ptr<const SpiceFlatCircuit> cachedSpiceFlatten(const std::string& text, const std::string& name);

/// Ready-made models of common parts (typical datasheet parameters, generic): name → model text.
struct SpiceBuiltinModel {
    std::string name, description, text;
};
const std::vector<SpiceBuiltinModel>& builtinSpiceModels();

}  // namespace sieda
