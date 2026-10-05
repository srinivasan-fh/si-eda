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

    bool isNoConnect(int pin) const;

    const ComponentDef& def() const;
    bool hasFootprint() const { return !def().footprint.empty(); }
    /// The footprint the part is placed with: its package variant, else the kind's default.
    const std::string& footprintName() const;
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

private:
    void invalidate() { netsDirty_ = true; }
    bool hasInstances() const;
    /// After an edit: keeps repeated sheets' instances in line (nothing to do in a design without them).
    void edited() {
        if (hasInstances()) syncInstances();
    }
    int masterWireOf(int wireId) const;
    int copyWireOn(int masterWire, int sheet) const;
    /// Designator of a block part (by its logical designator) on one of the block's sheets.
    std::string channelRef(const std::string& logical, int sheet, int step) const;
    /// Instance-aware parts of annotate(): numbers the blocks' logical designators.
    void annotateBlocks(const AnnotateOptions& options);
    void rebuildNets() const;
    /// Multi-sheet checks: ports, sheet entries, labels split across sheets, wires between sheets, bus labels.
    void hierarchyERC(std::vector<RuleViolation>& out) const;

    std::vector<Component> components_;
    std::vector<Wire> wires_;
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
