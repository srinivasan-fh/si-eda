/*
 * SiEDA Core — stable C ABI.
 *
 * This is the only header the Swift app imports (via the bridging header). The C++ engine sits
 * behind an opaque handle; rich results are returned as UTF-8 JSON strings that the caller must
 * release with sieda_string_free(). Mesh data is exposed as raw float/uint32 arrays for SceneKit.
 *
 * Coordinates: schematic in grid units (10 = one grid step, y down); PCB in millimetres (y down).
 * Functions returning int32_t return 1/0 for success/failure, or an id / -1 where documented.
 * A handle must not be used from two threads at once.
 */
#ifndef SIEDA_C_H
#define SIEDA_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SiedaProject SiedaProject;
typedef struct SiedaMesh SiedaMesh;
typedef struct SiedaLiveSim SiedaLiveSim;

/* ---- lifecycle & persistence ------------------------------------------------------------ */
const char* sieda_version(void);
SiedaProject* sieda_project_new(const char* name);
void sieda_project_free(SiedaProject* project);
/* Returns NULL on failure; *error_out (if non-NULL) receives a message to free with sieda_string_free. */
SiedaProject* sieda_project_load_json(const char* json, char** error_out);
char* sieda_project_save_json(const SiedaProject* project);
void sieda_project_set_name(SiedaProject* project, const char* name);
void sieda_project_set_requirements(SiedaProject* project, const char* text);
void sieda_project_clear(SiedaProject* project);
/* A blank project: empty schematic and library, default board, rules, net classes, pours, holes and outline. */
void sieda_project_reset(SiedaProject* project);
/* Industry profile ("general", "robotics", "power", "automotive", "rf", "space", "marine", "industrial", "medical", "defence", "networking", "vlsi"): applies its
   design-rule preset, altitude class and derating. Returns 1 on success, 0 for an unknown id. */
int32_t sieda_project_set_industry(SiedaProject* project, const char* industry_id);
/* [{"id","name","description","standards","rulePreset","powerDerating","currentDerating","highAltitude",
     "minAmbientC","maxAmbientC","guidance":[...]}] */
char* sieda_industry_profiles_json(void);
/* Full UI view model (components with world pin positions, wires, nets, pads, tracks, vias, ratsnest). */
char* sieda_project_snapshot(const SiedaProject* project);
/* Built-in component catalogue. */
char* sieda_library_json(void);
void sieda_string_free(char* s);

/* ---- schematic editing --------------------------------------------------------------------- */
/* ref may be NULL/empty for automatic designators. Returns the new component id or -1. */
int32_t sieda_add_component(SiedaProject* project, int32_t kind, const char* value, double x, double y,
                            int32_t rotation, const char* ref);
int32_t sieda_remove_component(SiedaProject* project, int32_t component_id);
int32_t sieda_move_component(SiedaProject* project, int32_t component_id, double x, double y);
int32_t sieda_rotate_component(SiedaProject* project, int32_t component_id, int32_t delta_degrees);
int32_t sieda_set_component_value(SiedaProject* project, int32_t component_id, const char* value);
int32_t sieda_set_component_ref(SiedaProject* project, int32_t component_id, const char* ref);
int32_t sieda_find_component(const SiedaProject* project, const char* ref);  /* id or -1 */
int32_t sieda_find_pin(const SiedaProject* project, int32_t component_id, const char* pin_name); /* index or -1 */
/* Returns wire id or -1. */
int32_t sieda_connect(SiedaProject* project, int32_t comp_a, int32_t pin_a, int32_t comp_b, int32_t pin_b);
/* Marks (1) or clears (0) a no-connect flag on a pin: ERC no longer reports it unconnected. 1 on success. */
int32_t sieda_set_pin_no_connect(SiedaProject* project, int32_t component_id, int32_t pin, int32_t no_connect);
int32_t sieda_remove_wire(SiedaProject* project, int32_t wire_id);
/* Splits a wire at (x, y) with a junction (T-junction / bend point) and returns the junction's component id, or -1.
 * Wires can then start or end on the junction's pin 0; deleting a junction that joins two wires straightens them. */
int32_t sieda_split_wire(SiedaProject* project, int32_t wire_id, double x, double y);
/* Removes a chain of dangling junctions (a wire abandoned half-way) starting at `junction_id`. Returns the count. */
int32_t sieda_remove_dangling_junctions(SiedaProject* project, int32_t junction_id);

/* ---- live (real-time, interactive) simulation ------------------------------------------------------------------- */
/* Starts a live simulation of the project's current schematic (a snapshot: later edits need a restart) at the t = 0
 * operating point. NULL with *error_out when the circuit cannot be simulated. Free with sieda_live_free. */
SiedaLiveSim* sieda_live_start(const SiedaProject* project, char** error_out);
/* Advances by `duration` seconds in steps of `step` (firmware included) and keeps a scope trace of every net over that
 * span, decimated to at most `trace_points` samples. 1 on success, 0 with *error_out. */
int32_t sieda_live_run(SiedaLiveSim* sim, double duration, double step, int32_t trace_points, char** error_out);
/* {"time", "nets":[{"index","name","voltage"}], "devices":[{"component","ref","current","power"}],
 *  "leds":[{"component","ref","current","brightness" 0…1}], "switches":[{"component","ref","closed","momentary"}],
 *  "mcus":[{"component","ref","model","status","running","serial","cycles","clockHz"}],
 *  "trace":{"time":[…], "nets":[{"index","name","values":[…]}]}} */
char* sieda_live_state(const SiedaLiveSim* sim);
/* Opens/closes a switch while running (push-buttons: closed while held). */
void sieda_live_set_switch(SiedaLiveSim* sim, int32_t component_id, int32_t closed);
/* Sends text to a microcontroller's USART receiver (serial monitor input). */
void sieda_live_serial_input(SiedaLiveSim* sim, int32_t component_id, const char* text);
void sieda_live_free(SiedaLiveSim* sim);

/* ---- microcontroller firmware ----------------------------------------------------------------------------------- */
/* Attaches Intel HEX firmware to a microcontroller component (ATmega328P, ATtiny85); the simulator runs it. An empty
 * hex removes it. clock_hz <= 0 uses the default (16 MHz ATmega328P, 8 MHz ATtiny85). Returns 1, or 0 with *error_out
 * (free with sieda_string_free) for an unknown component, a part that is not a supported microcontroller, an invalid
 * HEX file or an image larger than the flash. */
int32_t sieda_set_firmware(SiedaProject* project, int32_t component_id, const char* hex, const char* name,
                           double clock_hz, char** error_out);
/* The component's firmware as Intel HEX ("" when none). */
char* sieda_component_firmware(const SiedaProject* project, int32_t component_id);
/* Built-in example firmware: [{"id","name","model","description"}]. */
char* sieda_firmware_examples_json(void);
/* Intel HEX of an example, or NULL. */
char* sieda_firmware_example_hex(const char* id);

/* ---- custom components (datasheet import) ---------------------------------------------------- */
/* spec_json: {"name","manufacturer","description","refPrefix","defaultValue","datasheet",
 *             "package":{"type":"SOIC|TSSOP|DIP|QFN|LQFP|SOT23|HEADER|TO220","pinCount":8},
 *             "pins":[{"number":"1","name":"GND","type":"power_in|input|output|bidirectional|passive|
 *                      power_out|open_collector|no_connect","description":""}]}
 * Returns the generated part (id, symbol, footprint geometry) as JSON, or NULL with *error_out set. */
char* sieda_custom_part_register(SiedaProject* project, const char* spec_json, char** error_out);
/* Same generation without adding the part to the project library (editor previews). */
char* sieda_custom_part_preview(const char* spec_json, char** error_out);
/* Removes a part from the project library; returns 0 if it is still used by a component. */
int32_t sieda_custom_part_remove(SiedaProject* project, const char* part_id);
/* Switches every instance of old_id to new_id (wires are re-mapped by pin number, then name). */
int32_t sieda_custom_part_replace(SiedaProject* project, const char* old_id, const char* new_id);
int32_t sieda_add_custom_component(SiedaProject* project, const char* part_id, const char* value, double x, double y,
                                   int32_t rotation, const char* ref);
char* sieda_packages_json(void);
/* Built-in standard parts: [{"category": "...", "spec": {<spec_json as above>}}, ...] */
char* sieda_standard_parts_json(void);

/* ---- standard values --------------------------------------------------------------------- */
/* Nearest IEC 60063 value; series = 12, 24 or 96. */
double sieda_nearest_standard_value(double value, int32_t series);
int32_t sieda_is_standard_value(double value, int32_t series);
/* Parses an engineering value ("4k7", "100nF", "2.2M"); returns 1 and writes *out on success, else 0. */
int32_t sieda_parse_value(const char* text, double* out);

/* ---- analysis ------------------------------------------------------------------------------ */
char* sieda_run_erc(const SiedaProject* project);
/* Basic circuit validation: E-series values, decoupling, DC-derived part ratings (VAL_* codes). */
char* sieda_run_circuit_validation(const SiedaProject* project);
/* Full design verification (ERC, DC, validation, placement, routing, DRC, manufacturing outputs):
   {"verdict":"pass|warning|fail","passed","errors","warnings","infos","stages":[...],"markdown"}. */
char* sieda_run_verification(const SiedaProject* project);
char* sieda_simulate_dc(const SiedaProject* project);
char* sieda_simulate_transient(const SiedaProject* project, double t_stop, double t_step);
char* sieda_spice_netlist(const SiedaProject* project);

/* ---- PCB ----------------------------------------------------------------------------------- */
void sieda_pcb_set_board(SiedaProject* project, double width, double height, double track_width, double clearance);
/* Copper layer count: 1 (single-sided), 2, 4 or 6. Existing tracks on removed layers are deleted. */
void sieda_pcb_set_layer_count(SiedaProject* project, int32_t layers);
/* all = 1 re-places every footprint; 0 only places footprints that are not on the board yet. */
void sieda_pcb_autoplace(SiedaProject* project, int32_t all);
int32_t sieda_pcb_move_footprint(SiedaProject* project, int32_t component_id, double x, double y);
/* Embedded passive: forms a resistor / capacitor inside the board on inner copper layer `layer` (1 … layers-2;
 * a capacitor uses `layer` and `layer`+1); 0 makes it a surface part again. Returns 0 for other parts. */
int32_t sieda_set_component_embedded(SiedaProject* project, int32_t component_id, int32_t layer);
/* Robotics: sets the robot platform ("rover", "fpv", "arm", "quadruped", "humanoid"; "" = none). 0 if unknown. */
int32_t sieda_set_robot_platform(SiedaProject* project, const char* platform);
/* {"platform","applies","platforms":[{id,name,description,guidance}],"segments":[{id,name,status,items:[{label,ok,
 * detail}],guidance}]} — the seven robot design segments checked on the current design. */
char* sieda_robot_segments_json(const SiedaProject* project);
/* Automotive ECU type ("bcm", "powertrain", "adas", "ev", "chassis", "gateway"; "" = none). 0 if unknown. */
int32_t sieda_set_ecu_type(SiedaProject* project, const char* type);
/* The six automotive ECU segments checked on the design (same shape as sieda_robot_segments_json). */
char* sieda_ecu_segments_json(const SiedaProject* project);
/// Aerospace mission ("leo", "geo", "launcher", "military", "commercial"; "" = none). Returns 0 for an unknown id.
/// Mechanical build: board thickness in mm (≤ 0 keeps it, otherwise 0.4–6.4) and underfill / corner bonding of heavy parts.
int32_t sieda_pcb_set_mechanical(SiedaProject* project, double thickness, int32_t underfill);
/// Isolation barrier spacing between galvanic domains in mm (0 = none, up to 25): placement, routing, pours and DRC
/// keep that far apart (8 = 2 × MOPP, 4 = 1 × MOPP).
int32_t sieda_pcb_set_isolation_gap(SiedaProject* project, double gap);
/// Medical device class ("bf", "cf", "life", "implant", "home"; "" = none). 0 for an unknown id.
int32_t sieda_set_medical_class(SiedaProject* project, const char* cls);
/// The four medical segments, same JSON shape as sieda_robot_segments_json. Caller frees.
char* sieda_medical_segments_json(const SiedaProject* project);
/// Retail device class ("countertop", "unattended", "mpos", "kiosk", "printer"; "" = none). 0 for an unknown id.
int32_t sieda_set_retail_device(SiedaProject* project, const char* device);
/// The four retail / POS segments, same JSON shape as sieda_robot_segments_json. Caller frees.
char* sieda_retail_segments_json(const SiedaProject* project);
/// Home appliance type ("laundry", "kitchen", "refrigeration", "hvac", "small"; "" = none). 0 for an unknown id.
int32_t sieda_set_appliance_type(SiedaProject* project, const char* type);
/// The four appliance segments, same JSON shape as sieda_robot_segments_json. Caller frees.
char* sieda_appliance_segments_json(const SiedaProject* project);
/// Naval platform ("combatant", "carrier", "submarine", "patrol", "commercial"; "" = none). 0 for an unknown id.
int32_t sieda_set_naval_platform(SiedaProject* project, const char* platform);
/// The five naval segments, same JSON shape as sieda_robot_segments_json. Caller frees.
char* sieda_naval_segments_json(const SiedaProject* project);
int32_t sieda_set_aerospace_mission(SiedaProject* project, const char* mission);
/// The five aerospace segments, same JSON shape as sieda_robot_segments_json. Caller frees.
char* sieda_aerospace_segments_json(const SiedaProject* project);
/* Stitches thermal vias at a power part's largest pad (own net, clearance kept). Returns the vias added. */
int32_t sieda_pcb_add_thermal_vias(SiedaProject* project, int32_t component_id);
/* Locks a placed footprint so Auto Place (and Auto Route's placement) keeps it where it is. */
int32_t sieda_pcb_lock_footprint(SiedaProject* project, int32_t component_id, int32_t locked);
int32_t sieda_pcb_rotate_footprint(SiedaProject* project, int32_t component_id, int32_t delta_degrees);
int32_t sieda_pcb_flip_footprint(SiedaProject* project, int32_t component_id);
/* Resizes the board outline to the placed footprints plus margin_mm, keeping parts and copper together. */
int32_t sieda_pcb_fit_board(SiedaProject* project, double margin_mm);
char* sieda_pcb_autoroute(SiedaProject* project); /* JSON route statistics */
void sieda_pcb_clear_routing(SiedaProject* project);
char* sieda_pcb_run_drc(const SiedaProject* project);
/* Standard design-rule presets: [{"name","description","trackWidth",…,"minHoleToHole"}] */
char* sieda_design_rule_presets_json(void);
/* Applies a preset by name (design values + fabrication limits); returns 0 for unknown names. */
int32_t sieda_pcb_apply_rule_preset(SiedaProject* project, const char* name);
/* Net class: route `net_name` with a `width_mm` track (0 removes the class). Returns 1 on success. */
int32_t sieda_pcb_set_net_width(SiedaProject* project, const char* net_name, double width_mm);
/* Sizes net classes from the DC operating point (IPC-2221 + 25 %). Returns {"NET": width_mm, …} of classes it set. */
char* sieda_pcb_auto_net_widths(SiedaProject* project);
/* enabled = 1 (default): the autorouter runs sieda_pcb_auto_net_widths first. */
void sieda_pcb_set_auto_size_nets(SiedaProject* project, int32_t enabled);
/* Board outline polygon as JSON [{"x":…,"y":…}, …] (mm, ≥ 3 points); "[]" restores the width × height rectangle.
 * The outline is shifted to start at (0,0) and the board size set to its bounds. Returns 0 for invalid JSON. */
int32_t sieda_pcb_set_outline(SiedaProject* project, const char* points_json);
/* Solder mask colour: "green" (default), "black", "blue", "red", "yellow", "white" or "purple". 0 for unknown names. */
int32_t sieda_pcb_set_solder_mask(SiedaProject* project, const char* colour);
/* Conformal coating: "none", "acrylic", "silicone", "urethane", "epoxy" or "parylene" (IPC-CC-830). A coated board
 * uses IPC-2221B column A5 for voltage spacing and tighter leakage spacing. 0 for unknown names. */
int32_t sieda_pcb_set_coating(SiedaProject* project, const char* coating);
/* Stack-up: laminate material id ("fr4", "fr4-hightg", "isola-370hr", "rogers-4350b", "megtron-6", "polyimide",
 * "ims-aluminium"), construction ("rigid", "rigid-flex", "metal-core"), single-ended and differential impedance
 * targets (Ω) and backdrilling of high-speed via stubs. 0 for an unknown material or construction. */
int32_t sieda_pcb_set_stackup(SiedaProject* project, const char* material, const char* construction, double single_ended_ohms,
                              double differential_ohms, int32_t backdrill);
/* The stack-up with per-layer impedance-controlled widths: {"material","materialName","er","lossTangent","tg",
 * "construction","singleEndedOhms","differentialOhms","layers":[{name,type,thickness,line,seWidth,diffWidth,diffGap}],
 * "materials":[{id,name,er,lossTangent,tg,note}]}. */
char* sieda_stackup_json(const SiedaProject* project);
/* Length / phase matching: enable serpentine tuning after Auto Route and set the intra-pair skew and bus length
 * tolerances (mm). */
int32_t sieda_pcb_set_length_matching(SiedaProject* project, int32_t enabled, double pair_skew_mm, double bus_mm);
/* HDI (IPC-2226): with `hdi` set, Auto Route cuts vias to the layers they connect (blind / buried) and one-dielectric
 * spans become laser microvias (drill / pad mm); `via_in_pad` orders VIPPO (filled, plated over) so vias may sit in
 * SMD pads. Returns 0 for invalid sizes. */
int32_t sieda_pcb_set_hdi(SiedaProject* project, int32_t hdi, double microvia_drill, double microvia_diameter,
                          int32_t via_in_pad);
/* Applies the HDI via spans to the routed board now; returns the vias changed. */
int32_t sieda_pcb_apply_hdi(SiedaProject* project);
/* Adds serpentines to the short members of differential pairs and buses now; returns the nets tuned. */
int32_t sieda_pcb_tune_lengths(SiedaProject* project);
/* {"enabled","pairSkewTolerance","busLengthTolerance","groups":[{name,kind:"pair"|"bus",tolerance,target,matched,
 * nets:[{name,length,delta,routed,ok}]}]} */
char* sieda_length_report_json(const SiedaProject* project);
/* Outline presets: "rectangle" (w × h), "rounded" (corner radius param), "circle" (diameter w),
 * "quad-x" (quadcopter frame: span w, square body h, arm width param). Returns 0 for unknown kinds. */
int32_t sieda_pcb_outline_preset(SiedaProject* project, const char* kind, double w, double h, double param);
/* Non-plated mounting hole with a copper/part keep-out circle (keepout_mm ≤ 0: twice the drill). Returns the hole count. */
int32_t sieda_pcb_add_mounting_hole(SiedaProject* project, double x, double y, double drill_mm, double keepout_mm);
void sieda_pcb_clear_mounting_holes(SiedaProject* project);
/* Copper pour of `net_name` on copper layer `layer` (0 = top); plane = 1 reserves the layer for the net (other nets
 * only pass through with vias). clearance_mm ≤ 0 uses the board clearance. Returns the zone index or -1. */
int32_t sieda_pcb_add_zone(SiedaProject* project, const char* net_name, int32_t layer, int32_t plane, double clearance_mm);
int32_t sieda_pcb_remove_zone(SiedaProject* project, int32_t index);
void sieda_pcb_clear_zones(SiedaProject* project);
/** Active tamper mesh (PCI PTS): serpentines of net_a (horizontal stripes, inner layer_a) and net_b (vertical
 * stripes, inner layer_b) laid over component `ref` plus margin_mm by the autorouter; each net joins exactly two pins
 * of the secure element. Returns the mesh index or -1. */
int32_t sieda_pcb_add_tamper_mesh(SiedaProject* project, const char* ref, const char* net_a, const char* net_b,
                                  int32_t layer_a, int32_t layer_b, double margin_mm);
void sieda_pcb_clear_tamper_meshes(SiedaProject* project);

/* ---- exports ------------------------------------------------------------------------------- */
/* format: "spice", "bom", "pnp", "gerber_top", "gerber_bottom", "gerber_l<N>" (copper layer N, 1-based), "gerber_mask_top", "gerber_mask_bottom",
 *         "gerber_silk_top", "gerber_edge", "drill", "drill_npth" (mounting holes), "stl", "obj". Returns NULL for unknown formats. */
char* sieda_export(const SiedaProject* project, const char* format);
/* Bill of materials: {"lines":[{item, refs, componentIds, quantity, type, value, footprint, description, rating,
 * manufacturer, mpn, supplierPart, unitPrice, dnp, lineCost, suggestedManufacturer, suggestedMpn, notes}],
 * "summary":{lines, placements, dnp, missingMpn, unpriced, costPerBoard}, "buildQuantity", "orderCost"}. */
char* sieda_bom_json(const SiedaProject* project);
/* Sets a part's sourcing from JSON {"manufacturer","mpn","supplierPart","unitPrice","dnp"} (fields left out keep
 * their value). 1 on success. */
int32_t sieda_set_component_sourcing(SiedaProject* project, int32_t component_id, const char* json);
/* Boards per order, for BOM cost totals (≥ 1). */
void sieda_set_build_quantity(SiedaProject* project, int32_t quantity);
/* Writes the complete fabrication package into directory `dir` (created if needed): gerbers/ (every copper layer,
 * mask, paste, silkscreen with designators, outline, drills, X2 job file, IPC-D-356A netlist), assembly/ (BOMs, CPL,
 * pick-and-place, assembly drawings), fab_notes.txt, <base>-gerbers.zip for upload, netlist and 3D STL.
 * `base` names the files (NULL or "" = project name). Returns JSON {"ok":bool,"files":[…],"error":"…"}. */
char* sieda_write_fabrication_package(const SiedaProject* project, const char* dir, const char* base);

/* ---- 3D ------------------------------------------------------------------------------------ */
SiedaMesh* sieda_mesh_build(const SiedaProject* project, int32_t include_components);
/* Copper (tracks, pads, via lands) of one layer laid flat at Y = 0, for the X-ray layer-stack view. */
SiedaMesh* sieda_mesh_build_layer(const SiedaProject* project, int32_t layer);
void sieda_mesh_free(SiedaMesh* mesh);
int32_t sieda_mesh_vertex_count(const SiedaMesh* mesh);
int32_t sieda_mesh_index_count(const SiedaMesh* mesh);
const float* sieda_mesh_positions(const SiedaMesh* mesh); /* 3 floats per vertex */
const float* sieda_mesh_normals(const SiedaMesh* mesh);   /* 3 floats per vertex */
const float* sieda_mesh_colors(const SiedaMesh* mesh);    /* 4 floats per vertex (RGBA) */
const uint32_t* sieda_mesh_indices(const SiedaMesh* mesh);

#ifdef __cplusplus
}
#endif

#endif /* SIEDA_C_H */
