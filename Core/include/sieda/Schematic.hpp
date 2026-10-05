// SiEDA Core — schematic data model: components, wires, nets and electrical rule check.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Library.hpp"

namespace sieda {

struct PinRef {
    int component = -1;
    int pin = -1;
    bool operator==(const PinRef& o) const { return component == o.component && pin == o.pin; }
    bool operator<(const PinRef& o) const { return component < o.component || (component == o.component && pin < o.pin); }
};

struct PcbPlacement {
    Vec2 position;      // mm, board coordinates (y down)
    int rotation = 0;   // degrees, multiple of 90
    bool bottom = false;
    bool placed = false;
    /// Locked by the designer (connectors, fixed mechanical parts, matched-length bus parts): Auto Place keeps it.
    bool locked = false;
    /// Embedded passive: the inner copper layer it is formed on (1 … layers-2); 0 = an ordinary surface part.
    int embeddedLayer = 0;
    bool embedded() const { return embeddedLayer > 0; }
};

/// How a part is bought and fitted (BOM): who makes it, the orderable part number, the distributor / assembly-house
/// part number, the unit price, and "do not populate" (on the board but not fitted).
struct Sourcing {
    std::string manufacturer;
    std::string mpn;           // manufacturer part number
    std::string supplierPart;  // e.g. LCSC "C17414", Digi-Key, Mouser
    double unitPrice = 0;      // per part, in the project currency (0 = not priced)
    bool dnp = false;
    bool empty() const { return manufacturer.empty() && mpn.empty() && supplierPart.empty() && unitPrice == 0 && !dnp; }
};

/// How far a net label reaches in a multi-sheet design.
enum class LabelScope {
    Global = 0,      // joins every label of the same name on every sheet (the default; what every older design uses)
    Local = 1,       // joins labels of the same name on its own sheet only
    Port = 2,        // hierarchical port: sheet-local, and also joins the matching entry on the parent's sheet symbol
    SheetEntry = 3,  // entry of a sheet symbol on a parent sheet: joins the ports of the same name on `targetSheet`
};
const char* labelScopeName(LabelScope s);  // "global", "local", "port", "entry"
/// Parses a scope name; false for an unknown name.
bool labelScopeFromName(const std::string& name, LabelScope* out);

/// Designators of a repeated sheet's channels, made from each part's designator inside the block ("R1").
enum class InstanceRefs {
    SheetNumber = 0,  // the n-th sheet numbers from n·100 + 1: R1 → R201 on sheet 2, R301 on sheet 3 (·1000 for big blocks)
    Suffix = 1,       // the channel label is appended: R1 → R1_A, R1_B, …
};
const char* instanceRefsName(InstanceRefs r);  // "sheet", "suffix"
bool instanceRefsFromName(const std::string& name, InstanceRefs* out);

/// A schematic sheet (page). Components belong to exactly one sheet; wires never cross sheets — nets reach other
/// sheets through global labels, ground symbols and hierarchical ports / sheet entries.
struct Sheet {
    Sheet() = default;
    Sheet(int id_, std::string name_, int parent_) : id(id_), name(std::move(name_)), parent(parent_) {}
    int id = 1;
    std::string name;
    /// The sheet whose sheet symbol stands for this one (hierarchical design); 0 = a top-level sheet.
    int parent = 0;
    /// Repeated (multi-instance) sheet: the definition sheet this instance mirrors; 0 = an ordinary sheet or a
    /// definition. The components and wires of an instance are kept as copies of its definition's (see
    /// Schematic::repeatSheet), each with its own id, designator, nets, footprint placement and variant settings.
    int instanceOf = 0;
    /// Channel label of a repeated sheet ("A", "B", …): on the definition and on each of its instances.
    std::string channel;
    /// Definition of a repeated sheet: how its channels' designators are made from the block's own designators.
    InstanceRefs refs = InstanceRefs::SheetNumber;
};

/// Imported SPICE model attached to a part (docs/SIMULATION.md, sieda/SpiceModels.hpp): the simulator uses it instead
/// of the part's built-in model.
struct SpiceModelRef {
    std::string text;   // the definition and everything it uses (.model / .subckt / .param / .func cards)
    std::string model;  // the .model or .subckt name
    /// One entry per model port, in port order: a pin name or number, "0" (ground), "net:NAME", "dc:15" (an ideal
    /// supply to ground) or "nc"; ";" separates instances (both halves of a dual op-amp). Empty: the default mapping.
    std::string pins;
    bool empty() const { return text.empty(); }
};

struct Component {
    int id = -1;
    ComponentKind kind = ComponentKind::Resistor;
    std::string ref;
    std::string value;
    Vec2 position;  // schematic units
    int rotation = 0;
    PcbPlacement pcb;
    std::string customPart;  // CustomPartRegistry id when kind == ComponentKind::Custom
    std::vector<int> noConnect;  // pin indices deliberately left open (no-connect flag): ERC does not flag them
    // Microcontroller firmware (Intel HEX) run by the simulator; empty for other parts.
    std::string firmware;
    std::string firmwareName;  // file or example name shown in the editor
    double clockHz = 0;        // CPU clock; 0 = the model's default (16 MHz ATmega328P, 8 MHz ATtiny85)
    SpiceModelRef spice;       // imported SPICE model (empty: the built-in model)
    Sourcing sourcing;         // BOM: manufacturer, part numbers, price, do-not-populate
    /// Package variant this part is fitted in (Library::packageVariants: "R_0603", "CP_Tant_B", "D_DO41_THT"…);
    /// empty = the kind's default footprint.
    std::string package;
    /// Sheet the component is drawn on (Sheet::id).
    int sheet = 1;
    /// Net labels only: how far the label reaches; for a sheet entry, the child sheet it connects into.
    LabelScope scope = LabelScope::Global;
    int targetSheet = 0;
    /// On an instance of a repeated sheet: the component of the definition sheet this one copies (0 = none).
    int instanceOf = 0;
    /// On the definition of a repeated sheet: the part's designator inside the block ("R1"); `ref` is its designator
    /// in the definition's own channel (R201, R1_A). Empty everywhere else.
    std::string logicalRef;
    /// Net labels: the bus this label is an entry of (Bus::id; 0 = an ordinary label).
    int bus = 0;
    /// Multi-unit parts. A unit placed on its own (kind PartUnit): `unitOf` is its package and `unit` the 1-based unit
    /// index into CustomPart::units. The package is a Custom component with every pin and the footprint, marked
    /// `packageOnly`: it is not drawn on the schematic (its units are) but is the part the netlist, BOM and PCB see.
    int unitOf = 0;
    int unit = 0;
    bool packageOnly = false;
    /// On a copy in a repeated sheet's channel: the parameters this channel sets itself instead of taking the block's
    /// (bits of ChannelOverride). 0 everywhere else.
    int channelOverrides = 0;
    /// Signal harnesses (net labels only). A harness label (`harnessType` set) stands for a bundle named by its value
    /// ("USB1"): through its scope — port, sheet entry or global — it carries every member net "USB1.DP", "USB1.DN" …
    /// of its type across sheets; its own pin joins nothing. A harness entry (`harnessOf` = a harness label on the
    /// same sheet) is a label named by one entry of the type ("DP") that joins the member net "USB1.DP" on its sheet:
    /// the entries of a harness connector.
    std::string harnessType;
    int harnessOf = 0;

    bool isNoConnect(int pin) const;

    const ComponentDef& def() const;
    bool hasFootprint() const { return !def().footprint.empty(); }
    /// The footprint the part is placed with: its package variant, else the kind's default.
    const std::string& footprintName() const;
};

/// A signal harness type (Altium's harness definition): a named bundle of signals, e.g. USB = {DP, DN, VBUS, GND}.
struct HarnessType {
    std::string name;
    std::vector<std::string> entries;
};

/// A net class defined on the schematic (Altium's net class directive): the design rules the board takes for its
/// nets. 0 = the board's default.
struct NetClassDef {
    std::string name;
    double trackWidth = 0;  // mm
    double clearance = 0;   // mm, to other nets' copper
};

/// A schematic directive on a net (Altium's parameter set / differential pair / net class directives), attached to a
/// component pin: the net that pin is on — in every channel of a repeated sheet — gets it.
struct NetDirective {
    int id = -1;
    int component = -1;
    int pin = 0;
    std::string netClass;   // "" = none
    bool diffPair = false;  // a differential pair member: paired with the net of the opposite suffix (X_P / X_N …)
    double trackWidth = 0;  // parameter set: overrides the class (mm; 0 = none)
    double clearance = 0;
};

/// The rules a net gets from the schematic's directives.
struct NetRule {
    int net = -1;
    std::string netName;
    std::string netClass;
    double trackWidth = 0;
    double clearance = 0;
    bool diffPair = false;
    int partner = -1;  // the other net of its differential pair (-1 = none)
};

/// Per-channel parameters of a repeated sheet's part (Component::channelOverrides).
enum ChannelOverride : int {
    kOverrideValue = 1,    // the channel's own value (R1 = 10k in channel A, 12k in channel B)
    kOverridePackage = 2,  // the channel's own package variant
};

/// A graphical bus on a sheet: a named polyline ("D[0..7]", see expandBus). Its members leave it through bus entries —
/// net labels attached to it (Component::bus) — and join nets by name like any label; the bus itself carries no
/// connection.
struct Bus {
    int id = -1;
    int sheet = 1;
    std::string name;
    std::vector<Vec2> points;  // schematic units, at least two
    /// On an instance of a repeated sheet: the bus of the definition sheet this one copies (0 = none).
    int instanceOf = 0;
};

struct Wire {
    int id = -1;
    PinRef a, b;
    /// On an instance of a repeated sheet: the wire of the definition sheet this one copies (0 = none).
    int instanceOf = 0;
};

struct Net {
    int index = -1;
    std::string name;
    std::vector<PinRef> pins;
    bool isGround = false;
};

/// What a net carries, for colouring copper the way designers expect (power red, ground blue) and for net classes.
enum class NetRole { Signal = 0, Power, Ground, NegativeSupply };
const char* netRoleName(NetRole r);  // "signal", "power", "ground", "negative"

enum class Severity { Info = 0, Warning = 1, Error = 2 };

struct RuleViolation {
    Severity severity = Severity::Warning;
    std::string code;     // stable machine code, e.g. "ERC_UNCONNECTED_PIN"
    std::string message;  // human readable
    std::vector<int> components;
    Vec2 location;  // schematic units (ERC) or millimetres (DRC)
    bool hasLocation = false;
    int sheet = 0;  // ERC: the sheet `location` is on (0 = not a schematic location)
};

/// Designator re-numbering (Tools ▸ Annotate).
struct AnnotateOptions {
    /// Rows: sheet by sheet, top to bottom then left to right; columns: left to right then top to bottom.
    bool byColumns = false;
    /// Keep every unique designator and number only the duplicates and unnumbered ones ("R?").
    bool keepExisting = false;
    /// Sheet-based numbers: parts on the n-th sheet are numbered from n·100 + 1 (R101, R102 … R201 …), or from
    /// n·1000 + 1 when a sheet holds 100 or more parts of one prefix.
    bool sheetNumbering = false;
    /// Multi-unit parts: re-assign interchangeable units (the gates of a quad op-amp) to packages in placement order —
    /// A, B, C, D of the first package, then the next — before numbering, so no package is left half used.
    bool packUnits = false;
};

struct RefChange {
    int component = -1;
    std::string from, to;
};

/// Bus notation: "D[0..7]" → D0 … D7, "A[15..12]" → A15 … A12, comma lists "D[0..3],WR,RD"; a range may carry a
/// suffix ("D[0..1]_N" → D0_N, D1_N). Returns the member names, or an empty list when `name` is not a bus or has
/// more than 1024 members.
std::vector<std::string> expandBus(const std::string& name);

class Schematic {
public:
    int addComponent(ComponentKind kind, const std::string& value, Vec2 position, int rotation = 0,
                     const std::string& ref = "");
    /// Adds an instance of a registered custom part. Returns -1 if the part id is unknown.
    int addCustomComponent(const std::string& partId, const std::string& value, Vec2 position, int rotation = 0,
                           const std::string& ref = "");
    /// Switches every instance of `oldId` to `newId`, re-mapping wires by pin number then name. Returns count.
    int replaceCustomPart(const std::string& oldId, const std::string& newId);
    /// Removes a component and its wires. Removing a junction that joins exactly two wires keeps them connected.
    bool removeComponent(int id);
    bool moveComponent(int id, Vec2 position);
    bool rotateComponent(int id, int deltaDeg = 90);
    bool setValue(int id, const std::string& value);
    /// Fits the part in another package variant ("" = default); false if the kind has no such variant.
    bool setPackage(int id, const std::string& package);
    bool setRef(int id, const std::string& ref);
    /// Marks a pin as intentionally unconnected (KiCad/Altium "no-connect" flag) or clears the mark.
    bool setPinNoConnect(int componentId, int pin, bool noConnect);
    /// Attaches firmware to a microcontroller (empty `hex` removes it). Returns false for an unknown id.
    bool setFirmware(int id, const std::string& hex, const std::string& name, double clockHz);
    bool setSpiceModel(int id, const SpiceModelRef& model);

    int connect(PinRef a, PinRef b);  // returns wire id, or -1 if invalid / duplicate
    /// Removes a wire; a junction left without wires is removed with it.
    bool removeWire(int id);
    /// Splits a wire at `position` with a new junction (T-junction or bend point) and returns the junction's id:
    /// the two halves keep the connection and further wires can end on the junction. -1 for an unknown wire.
    int splitWire(int wireId, Vec2 position);
    /// Removes a chain of dangling junctions starting at `junctionId` (a wire abandoned half-way). Returns count.
    int removeDanglingJunctions(int junctionId);
    /// Number of wire ends on the component's pins.
    int wireCount(int componentId) const;
    void clear();

    const std::vector<Component>& components() const { return components_; }
    std::vector<Component>& mutableComponents() { invalidate(); return components_; }
    const std::vector<Wire>& wires() const { return wires_; }
    const Component* find(int id) const;
    Component* find(int id);
    const Component* findByRef(const std::string& ref) const;
    int pinIndex(int componentId, const std::string& pinName) const;

    Vec2 pinPosition(PinRef pin) const;  // schematic units, world space

    /// Connectivity (cached). Nets are merged across wires and identically named net labels.
    const std::vector<Net>& nets() const;
    int netOf(PinRef pin) const;  // -1 if pin has no net (unconnected)
    /// True when the pin's net reaches anything besides the pin itself and the pins stacked with it on its symbol
    /// (a stack of repeated GND pins with no wire is still open).
    bool isPinConnected(PinRef pin) const;
    int groundNet() const;        // -1 if no ground net
    /// Ground nets; supply rails (a source's + terminal, a regulator's power output, an IC's power input, or a name
    /// such as VCC, VDD, VBAT, +5V, 3V3); negative rails (a source's − terminal above a grounded +, or -12V, VEE);
    /// everything else is a signal.
    NetRole netRole(int net) const;

    std::vector<RuleViolation> runERC() const;

    /// Simulating an assembly (a design variant): parts not fitted (Sourcing::dnp) are left out of the simulated
    /// circuit — their nets stay, only their elements go. Off by default (the design as drawn).
    void setOmitUnfitted(bool on) { omitUnfitted_ = on; }
    bool omitsFromSimulation(const Component& c) const {
        return omitUnfitted_ && c.sourcing.dnp && !isNetSymbolKind(c.kind);
    }

    // ---- sheets (multi-sheet / hierarchical design). There is always at least one sheet. ----
    const std::vector<Sheet>& sheets() const { return sheets_; }
    const Sheet* findSheet(int id) const;
    /// Position of the sheet in the sheet order (tabs, annotation), or -1.
    int sheetIndex(int id) const;
    /// Nesting depth: 0 for a top-level sheet.
    int sheetDepth(int id) const;
    /// Adds a sheet (name trimmed, unique, non-empty) under `parent` (0 = top level). Returns its id or -1.
    int addSheet(const std::string& name, int parent = 0);
    bool renameSheet(int id, const std::string& name);
    /// Re-parents a sheet (0 = top level); refuses cycles.
    bool setSheetParent(int id, int parent);
    /// Moves a sheet to position `index` in the sheet order.
    bool reorderSheet(int id, int index);
    /// Removes a sheet. A sheet that still holds components is removed only with `deleteContents` (its components
    /// and their wires go with it). Sheet entries pointing into it are removed; its child sheets move up to its
    /// parent. The last sheet cannot be removed.
    bool removeSheet(int id, bool deleteContents);
    /// The sheet new components are placed on.
    int activeSheet() const { return activeSheet_; }
    bool setActiveSheet(int id);
    /// Moves components to another sheet. Junctions that only join moved components go with them; wires that would
    /// cross sheets are removed. Returns the number of components moved (junctions included).
    int moveToSheet(const std::vector<int>& ids, int sheet);
    /// Net labels only. A sheet entry needs an existing `targetSheet` other than the label's own sheet.
    bool setLabelScope(int id, LabelScope scope, int targetSheet = 0);
    /// Distinct hierarchical port names on a sheet, sorted.
    std::vector<std::string> sheetPorts(int sheet) const;
    /// Sheet symbol of `child` on its parent sheet: adds one entry (wired to nothing yet) for every port of `child`
    /// that has none, stacked downward from `origin` (or below the existing entries). Returns the entries added; -1
    /// when `child` is unknown or top-level.
    int placeSheetEntries(int child, Vec2 origin);

    /// Re-numbers reference designators (net symbols keep theirs). Returns what changed.
    std::vector<RefChange> annotate(const AnnotateOptions& options);
    /// Bus labels: one net label per member of `bus` (see expandBus) on the given pins of `componentId`, in order,
    /// each placed just outside its pin and wired to it. Returns the labels added, or -1 when the pins and members
    /// differ in number or a pin is invalid.
    int addBusLabels(int componentId, const std::vector<int>& pins, const std::string& bus,
                     LabelScope scope = LabelScope::Global);

    // ---- repeated (multi-instance) sheets: one definition sheet used several times (channels). Each instance is a
    // sheet of its own holding copies of the definition's components and wires, with their own ids, designators,
    // nets (local labels and ports are per sheet), footprints and variant settings; the stored design stays flat.
    // Edits to a copy go to the definition and every instance follows. ----
    /// Uses `sheet` `count` times in all (itself first): adds or removes instance sheets ("<name> [B]" …, under the
    /// same parent, after it in the sheet order). Only a sheet without child sheets or sheet entries can be repeated,
    /// and not an instance; 1 ends the repetition. Returns the number of instances, or -1.
    int repeatSheet(int sheet, int count);
    /// The definition sheet of an instance; any other sheet is its own.
    int definitionSheet(int sheet) const;
    /// The definition followed by its instances in sheet order (just `sheet` when it is not repeated).
    std::vector<int> sheetInstances(int sheet) const;
    /// True for the definition of a repeated sheet and for its instances.
    bool isRepeated(int sheet) const;
    /// How the channels' designators are made (definition or any instance of it).
    bool setInstanceRefs(int sheet, InstanceRefs refs);
    /// Channel label (non-empty, unique within the block, letters, digits, '_' or '-').
    bool setSheetChannel(int sheet, const std::string& channel);
    /// Brings every instance in line with its definition (components, wires, designators) and repairs stale links.
    /// Every edit through this class does it; call it after changing components through mutableComponents().
    void syncInstances();
    /// Per-channel parameters. On a part of a repeated sheet, sets the value of this channel only (a copy keeps it
    /// while the block's value changes; on the block's own sheet the other channels keep the old value). Setting the
    /// block's value clears the override. On any other part it is setValue. False for an unknown id or a net symbol.
    bool setChannelValue(int id, const std::string& value);
    /// Package variant of this channel only (see setChannelValue); "" = the kind's default footprint.
    bool setChannelPackage(int id, const std::string& package);
    /// The channel takes the block's value and package again.
    bool clearChannelOverrides(int id);
    /// The value every channel of a repeated part takes unless it sets its own (the part's value elsewhere).
    std::string blockValue(int id) const;
    /// Channel path of a sheet in a (nested) repeated hierarchy, outermost first: {"B", "A"} for channel A of a
    /// sub-block inside channel B of a block. Only repeated levels count; empty for an ordinary sheet.
    std::vector<std::string> channelPath(int sheet) const;
    /// Occurrences of definition sheet `def` whose parent is `parent`: the definition first when it is there, then
    /// its instances in sheet order.
    std::vector<int> occurrencesUnder(int def, int parent) const;
    /// Number of channels of a repeated sheet's block under one parent (its repeat count); 1 for an ordinary sheet.
    int channelCount(int sheet) const;
    /// For an id on an instance sheet, the definition component it copies; otherwise `id`.
    int masterOf(int id) const;
    /// The copy of definition component `masterId` on `sheet` (`masterId` itself on the definition), or -1.
    int copyOn(int masterId, int sheet) const;

    std::string nextRef(ComponentKind kind) const;
    std::string nextRef(const std::string& prefix) const;

    // Persistence helpers used by Project
    void restoreComponent(const Component& c);
    void restoreWire(const Wire& w);
    /// Replaces the sheet list (invalid or duplicate entries are dropped; an empty list leaves one default sheet)
    /// and the active sheet. Components on unknown sheets move to the first sheet.
    void restoreSheets(const std::vector<Sheet>& sheets, int active);
    /// Adds a bus read from a file (call after the components; entries of unknown buses become plain labels).
    void restoreBus(const Bus& bus);

    // ---- multi-unit parts (one symbol per gate, one footprint) ----
    /// Places unit A of a multi-unit custom part: a hidden package (every pin, the footprint) and the unit's symbol.
    /// Returns the unit's id, or -1 for an unknown part or one without units.
    int addCustomUnits(const std::string& partId, const std::string& value, Vec2 position, int rotation = 0,
                       const std::string& ref = "");
    /// Places unit `unit` (1-based) of the package of `componentId` (the package or any of its units) on the active
    /// sheet. Returns its id, or -1 (unknown part, unit out of range, or already placed).
    int addPartUnit(int componentId, int unit, Vec2 position, int rotation = 0);
    /// Places the first unit of the package that is not placed yet. -1 when every unit is placed.
    int placeNextUnit(int componentId, Vec2 position);
    /// The package of a unit (or the package itself); -1 for anything else.
    int unitPackage(int componentId) const;
    /// The units placed of a package, in order of their unit index.
    std::vector<int> placedUnits(int packageId) const;
    /// "A", "B", "P"… for a placed unit; "" otherwise.
    std::string unitName(const Component& c) const;
    /// Designator with the unit ("U1A") for a unit, the designator otherwise.
    std::string displayRef(const Component& c) const;
    /// Gate swap: two placed units of interchangeable gates (UnitSpec::swapGroup) of the same part and value, on one
    /// sheet, exchange their gates (package and unit); the symbols and wires stay. False when not allowed.
    bool swapUnits(int unitA, int unitB);
    /// Pin swap: two pins of a placed unit in one of its pin-swap groups exchange their wires. False when not allowed.
    bool swapPins(int componentId, int pinA, int pinB);

    // ---- signal harnesses (structured buses) ----
    const std::vector<HarnessType>& harnessTypes() const { return harnessTypes_; }
    const HarnessType* findHarnessType(const std::string& name) const;
    /// Defines or replaces a harness type: a name (letters, digits, '_' or '-', 1…32) and 1…256 unique entry names
    /// (no '.' or spaces). False when invalid.
    bool setHarnessType(const std::string& name, const std::vector<std::string>& entries);
    /// Removes a type; its harness labels stay (ERC reports them until the type is defined again).
    bool removeHarnessType(const std::string& name);
    /// Makes a net label a harness label of `type` ("" = an ordinary label again; its entries become plain labels).
    bool setLabelHarness(int labelId, const std::string& type);
    /// Harness connector on the active sheet: a local harness label named `name` with one entry per member of
    /// `type`, stacked below `position`. Returns the harness label's id, or -1 (unknown type, invalid name).
    int addHarnessConnector(const std::string& type, const std::string& name, Vec2 position);
    /// Adds an entry for every member of a harness label's type that has none. Returns the entries added, or -1.
    int placeHarnessEntries(int harnessLabel);
    /// The net a label joins by name: "USB1.DP" for a harness entry, the label's value otherwise.
    std::string labelNetName(const Component& c) const;
    /// True for a harness label (a bundle, not a net).
    static bool isHarnessLabel(const Component& c) {
        return c.kind == ComponentKind::NetLabel && !c.harnessType.empty() && c.harnessOf == 0;
    }
    /// Detaches harness entries whose harness is gone or on another sheet (true when it changed anything).
    bool repairHarnessLinks();
    /// Replaces the harness types (persistence; invalid entries dropped).
    void restoreHarnessTypes(const std::vector<HarnessType>& types);

    // ---- schematic directives: net classes, differential pairs, parameter sets (the source of the PCB rules) ----
    const std::vector<NetClassDef>& netClassDefs() const { return netClassDefs_; }
    const NetClassDef* findNetClassDef(const std::string& name) const;
    /// Defines or replaces a net class (name: letters, digits, '_' or '-', 1…32; width / clearance 0 or 0.05…10 mm).
    bool setNetClassDef(const NetClassDef& def);
    bool removeNetClassDef(const std::string& name);
    const std::vector<NetDirective>& directives() const { return directives_; }
    const NetDirective* findDirective(int id) const;
    /// Adds a directive on the net of (component, pin); a copy in a repeated sheet's channel anchors it on its block.
    /// Returns its id, or -1 (unknown pin, invalid values).
    int addDirective(const NetDirective& d);
    bool updateDirective(int id, const NetDirective& d);
    bool removeDirective(int id);
    /// Persistence: adds a directive read from a file (invalid ones dropped).
    void restoreDirective(const NetDirective& d);
    /// The rules every net with a directive gets: class values, overridden by the directive's own; differential pair
    /// members with their partner nets. Sorted by net.
    std::vector<NetRule> netRules() const;
    /// Differential pairs marked by directives: (positive, negative) nets.
    std::vector<std::pair<int, int>> directiveDiffPairs() const;

    // ---- graphical buses ----
    const std::vector<Bus>& buses() const { return buses_; }
    const Bus* findBus(int id) const;
    /// Draws a bus on the active sheet (on a repeated sheet's instance: on its definition). `name` must be bus
    /// notation; 2 … 256 points. Returns its id or -1.
    int addBus(const std::string& name, const std::vector<Vec2>& points);
    /// Removes a bus with its entries (and their wires).
    bool removeBus(int id);
    /// Renames a bus (entries whose names are no longer members are reported by ERC).
    bool renameBus(int id, const std::string& name);
    /// Makes a net label an entry of a bus on its sheet (0 = an ordinary label again). False for anything else.
    bool setLabelBus(int labelId, int busId);
    /// Moves a bus and its entries by `delta`.
    bool moveBus(int id, Vec2 delta);
    bool setBusPoints(int id, const std::vector<Vec2>& points);
    /// Members of a bus in order (expandBus of its name).
    std::vector<std::string> busMembers(int id) const;
    /// Point of the bus nearest to `p`.
    Vec2 nearestBusPoint(int id, Vec2 p) const;
    /// Rips entries out of a bus for `members` (all members when empty) that have none yet, spaced along it.
    /// Returns the entries added, or -1 for an unknown bus or a name that is not a member.
    int ripBusEntries(int id, const std::vector<std::string>& members, LabelScope scope = LabelScope::Local);
    /// Connects bus members to a part's pins: each member that names a pin of the part (D0 → pin "D0" or "PB0/D0")
    /// gets an entry on the bus, near the pin, wired to it; with no names in common, the members go to the part's
    /// unconnected pins in order. Returns the connections made, or -1 for an unknown bus or part.
    int connectBusToPart(int id, int componentId, LabelScope scope = LabelScope::Local);

private:
    void invalidate() { netsDirty_ = true; }
    bool hasInstances() const;
    /// After an edit: keeps repeated sheets' instances in line (nothing to do in a design without them).
    void edited() {
        repairHarnessLinks();
        if (!directives_.empty()) repairDirectives();
        if (hasInstances()) syncInstances();
        else syncUnits();
    }
    /// Units follow their package (designator, value, part) and the package its first unit (sheet, position);
    /// units without a valid package and packages without units go.
    bool syncUnits();  // true when it changed anything
    /// Annotation helper: see AnnotateOptions::packUnits.
    void packUnits();
    /// ERC of multi-unit parts: units not placed whose pins are left open.
    void unitERC(std::vector<RuleViolation>& out) const;
    int masterWireOf(int wireId) const;
    int copyWireOn(int masterWire, int sheet) const;
    /// Designator of a block part (by its logical designator) on one of the block's sheets; `path` is the sheet's
    /// channel path joined by '_' (the suffix scheme).
    std::string channelRef(const std::string& logical, int sheet, int step, const std::string& path) const;
    /// Nested repeated sheets: every occurrence of a block's parent holds the same channels of it as the parent's
    /// definition does (created / removed / relabelled here). True when it changed anything.
    bool syncNestedSheets();
    /// Removes a sheet with every sheet below it, their components, wires and the sheet entries leading into them.
    void eraseSheetTree(int id);
    /// A sheet entry copied from `defSheet` onto its instance `instSheet`: the matching occurrence of its target.
    int mapEntryTarget(int target, int defSheet, int instSheet) const;
    /// Unique name for a new instance sheet ("Amp [B]", nested: "Sub [B/A]").
    std::string instanceSheetName(int sheet) const;
    bool setChannelField(int id, const std::string& text, int bit);
    /// Instance-aware parts of annotate(): numbers the blocks' logical designators.
    void annotateBlocks(const AnnotateOptions& options);
    /// Drops buses on missing sheets and detaches entries from buses that are gone or on another sheet.
    bool repairBusLinks();  // true when it changed anything
    /// One pass of syncInstances(); true when it changed anything (a repair can enable another).
    bool syncInstancesOnce();
    int masterBusOf(int id) const;
    /// Bus checks: entries that are not members, members that reach one pin only, buses without entries.
    void busERC(std::vector<RuleViolation>& out) const;
    void rebuildNets() const;
    /// Multi-sheet checks: ports, sheet entries, labels split across sheets, wires between sheets, bus labels.
    void hierarchyERC(std::vector<RuleViolation>& out) const;
    /// Harness checks: unknown types, entries that are not members, mismatched types through the hierarchy, members
    /// that reach nothing.
    void harnessERC(std::vector<RuleViolation>& out) const;
    std::vector<HarnessType> harnessTypes_;
    /// Directive checks: unknown classes, directives on no net, conflicting classes, unpaired differential pairs.
    void directiveERC(std::vector<RuleViolation>& out) const;
    /// Drops directives whose anchor is gone (true when it changed anything).
    bool repairDirectives();
    std::vector<NetClassDef> netClassDefs_;
    std::vector<NetDirective> directives_;
    int nextDirectiveId_ = 1;

    std::vector<Component> components_;
    std::vector<Wire> wires_;
    std::vector<Bus> buses_;
    int nextBusId_ = 1;
    bool omitUnfitted_ = false;
    std::vector<Sheet> sheets_{Sheet{1, "Main", 0}};
    int activeSheet_ = 1;
    int nextSheetId_ = 2;
    int nextComponentId_ = 1;
    int nextWireId_ = 1;

    mutable bool netsDirty_ = true;
    mutable std::vector<Net> nets_;
    mutable std::map<PinRef, int> pinToNet_;
};

const char* severityName(Severity s);

}  // namespace sieda
