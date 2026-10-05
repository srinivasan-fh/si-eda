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
/* Package variant of a passive / diode ("R_0603", "CP_Tant_B", "D_DO41_THT"…; "" = default). 1 on success. */
int32_t sieda_set_component_package(SiedaProject* project, int32_t component_id, const char* package);
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
/* Footprint editor: the part's footprint as an editable land pattern (spec JSON with package type "CUSTOM" and one
 * land per pad). NULL and *error_out (caller frees) on an invalid spec. Caller frees. */
char* sieda_custom_part_land_pattern(const char* spec_json, char** error_out);
/* Footprint editor checks: [{severity,code,message,pads:[n]}] for a spec's land pattern — overlapping pads, copper
 * gaps below min_gap mm, annular rings, pads without pins and pins without pads. Caller frees. */
char* sieda_check_land_pattern(const char* spec_json, double min_gap);
/* Symbol editor: the spec with an auto-arranged symbol layout (supplies top, grounds bottom, inputs left, outputs
 * right, ports grouped; repeated supply / ground pins stacked when stack != 0). NULL and *error_out on an invalid
 * spec. Caller frees. */
char* sieda_symbol_auto_arrange(const char* spec_json, int32_t stack, char** error_out);
/* Symbol editor checks: [{severity,code,message,pins:["number"]}] — pins missing from the symbol or placed twice,
 * unknown pins, different pins on one spot (errors), stacked signal pins (warning), stacked pins (info). */
char* sieda_check_symbol(const char* spec_json);
/* Removes a part from the project library; returns 0 if it is still used by a component. */
int32_t sieda_custom_part_remove(SiedaProject* project, const char* part_id);
/* Switches every instance of old_id to new_id (wires are re-mapped by pin number, then name). */
int32_t sieda_custom_part_replace(SiedaProject* project, const char* old_id, const char* new_id);
int32_t sieda_add_custom_component(SiedaProject* project, const char* part_id, const char* value, double x, double y,
                                   int32_t rotation, const char* ref);
char* sieda_packages_json(void);
/* Built-in standard parts: [{"category": "...", "spec": {<spec_json as above>}}, ...] */
char* sieda_standard_parts_json(void);

/* ---- library import (KiCad .kicad_mod / .kicad_sym, Eagle .lbr) ------------------------------ */
/* request_json: {"files":[{"name":"LM358.kicad_sym","content":"<file text>"}, ...],
 *                "pairs":{"<symbol name>":"<footprint name>"}}   (pairs optional)
 * Reads the files, pairs symbols with footprints and validates every part with the symbol and land-pattern checks.
 * Returns {"parts":[{"name","symbol","footprint","source","ok":bool,"error","warnings":[...],"spec":{<spec_json>}}],
 *          "files":[{"name","format","symbols","footprints","error"}],"symbols":n,"footprints":n}.
 * Parts with ok == true register as they are (sieda_custom_part_register with their spec). Malformed files are
 * reported in files[].error; the call never fails on file content. Caller frees. */
char* sieda_library_import(const char* request_json);

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
/* Advanced analyses (docs/SIMULATION.md). Each takes a JSON options object (numbers may be engineering strings such as
 * "10k" or "1MEG"; NULL or "" = defaults) and returns {"ok","error",…}. Nets are named as in the schematic, parts by
 * reference.
 * AC small-signal sweep: {"start":1,"stop":1e6,"pointsPerDecade":50,"source":"V1"} →
 *   {"stimulus":[refs],"frequency":[Hz],"nets":[{index,name,dc,magnitudeDb:[],phaseDeg:[],metrics:{lowFreqDb,peakDb,
 *   peakHz,f3dbHz,bwLowHz,bwHighHz,unityHz,phaseMarginDeg}}]} (readouts outside the sweep are null). */
char* sieda_simulate_ac(const SiedaProject* project, const char* options_json);
/* DC sweep of a source: {"source":"V1","start":0,"stop":5,"step":0.05} →
 *   {"source","unit","values":[],"nets":[{index,name,values}],"currents":[{component,ref,values}]}. */
char* sieda_simulate_dc_sweep(const SiedaProject* project, const char* options_json);
/* Parameter sweep: {"component":"R1","values":["1k","2k2"],"analysis":"dc|ac|transient","net":"OUT" (optional),
 *   AC options as above, "stop"/"step" for transient} → {"component","analysis","runs":[{value,ok,error,x:[Hz or s],
 *   nets:[{index,name, voltage | values | magnitudeDb,phaseDeg,metrics}]}]}. */
char* sieda_simulate_param_sweep(const SiedaProject* project, const char* options_json);
/* Monte Carlo + worst case over R/C/L tolerances: {"net":"OUT","measure":"dc|gain|f3db|peak","runs":100,"seed":1,
 *   "distribution":"uniform|gaussian","worstCase":true,"tolerances":{"resistor":0.01,"capacitor":0.1,"inductor":0.2},
 *   AC options for the AC measures} → {"net","measure","unit","nominal","runs","failedRuns","min","max","mean","sigma",
 *   "samples":[],"histogram":{edges,counts},"worstCase":{min,max,minParts,maxParts},"parts":[{ref,value,tolerance,
 *   fromValue,sensitivity}]}. */
char* sieda_simulate_monte_carlo(const SiedaProject* project, const char* options_json);
/* FFT / THD of a net over a transient run: {"net":"OUT","stop":"10m","step":"1u","fundamental":1000,"harmonics":10,
 *   "from":"5m"} (fundamental default: the first SIN source; window: whole periods after "from", default stop / 2) → {"fundamentalHz","thdPercent","dc","cycles","windowStart",
 *   "windowStop","frequency":[],"magnitudeDb":[],"harmonics":[{order,frequency,amplitude,dbc}]}. */
char* sieda_simulate_fft(const SiedaProject* project, const char* options_json);
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
/* Robotics: sets the robot platform ("rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"; "" = none).
 * 0 if unknown. */
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
/// Memory design type ("sdram", "ddr", "lpddr", "dimm", "rdimm"; "" = none). 0 for an unknown id.
int32_t sieda_set_memory_design(SiedaProject* project, const char* type);
/// The five memory segments, same JSON shape as sieda_robot_segments_json. Caller frees.
char* sieda_memory_segments_json(const SiedaProject* project);
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

/* ---- interactive routing ---------------------------------------------------------------------------------------
 * One route session per project. The router works on a copy of the board taken when the route begins and edits the
 * layout only on commit; any other board edit in between makes the commit fail (call cancel first).
 * options_json (NULL = keep the current options): {"mode":"shove"|"walkaround", "posture":"45"|"90"|"free",
 *   "swapPosture":bool, "width":mm (0 = net class), "pairGap":mm (0 = stack-up), "snap":bool}.
 * Begin / move / fix / via / options return the preview, caller frees:
 *   {"active","kind":"route"|"pair"|"drag"|"via","status","blocked","reachedTarget","nets":[…],"layer","width","gap",
 *    "endX","endY","length","netLength","targetLength","placed":[track],"head":[track],"vias":[via],
 *    "shovedTracks":[track],"shovedVias":[via],"hiddenTracks":[id],"hiddenVias":[id]}
 *   (track = {id,net,layer,width,ax,ay,bx,by}; via = {id,net,x,y,drill,diameter,fromLayer,toLayer,kind})
 * plus "error":"…" when that call failed (the preview is still the current state). */
char* sieda_router_begin(SiedaProject* project, const char* options_json, double x, double y, int32_t layer);
/* Differential pair from a pad of either member (nets X_P / X_N, X+ / X-, …). */
char* sieda_router_begin_pair(SiedaProject* project, const char* options_json, double x, double y, int32_t layer);
/* Drags track `track_id` grabbed at (x, y): it moves parallel to itself, neighbours follow, other nets are shoved. */
char* sieda_router_begin_drag(SiedaProject* project, const char* options_json, int32_t track_id, double x, double y);
char* sieda_router_move(SiedaProject* project, double x, double y);
/* Places the head (a click): the route continues from its end. */
char* sieda_router_fix(SiedaProject* project);
/* Places the head and a through via at its end; continues on to_layer (-1 = the other outer layer). */
char* sieda_router_add_via(SiedaProject* project, int32_t to_layer);
char* sieda_router_set_options(SiedaProject* project, const char* options_json);
/* Writes the route and every shoved item into the layout: {"ok","error","removedTracks":[track],"removedVias":[via],
 * "addedTracks":[id],"addedVias":[id]}. The session ends either way. */
char* sieda_router_commit(SiedaProject* project);
void sieda_router_cancel(SiedaProject* project);
int32_t sieda_router_active(const SiedaProject* project);
/* Length tuning: accordion meanders on track `track_id` (then the net's other tracks) until its net is target_mm
 * long; target_mm <= 0 matches the longest member of the net's pair / bus group. max_amplitude_mm <= 0 = 2 mm.
 * {"ok","message","net","before","after","target","changes":{…as commit…}}. */
char* sieda_router_tune_length(SiedaProject* project, int32_t track_id, double target_mm, double max_amplitude_mm);
/* Drags via `via_id` grabbed at (x, y): it follows the cursor, the tracks ending on it follow, other nets are shoved.
 * Returns the preview (kind "via"); continue with sieda_router_move / commit / cancel. */
char* sieda_router_begin_via_drag(SiedaProject* project, const char* options_json, int32_t via_id, double x, double y);
/* Interactive length tuning of the net of track `track_id`. options_json: {"target":mm (0 = the longest member of
 * the net's pair / bus group), "maxAmplitude":mm (0 = 2), "spacing":mm (meander legs, edge to edge; 0 = default),
 * "x","y" (meanders near this point first), "apply":bool (false = preview only, the board is unchanged)}.
 * {"ok","message","net","group","groupKind","tolerance","before","after","target","applied",
 *  "addedTracks":[track],"removedTracks":[id],"changes":{…as commit…}}. Caller frees. */
char* sieda_router_tune(SiedaProject* project, int32_t track_id, const char* options_json);
/* Locked tracks are never shoved or dragged. Returns 1 on success. */
int32_t sieda_pcb_lock_track(SiedaProject* project, int32_t track_id, int32_t locked);
/* Deletes one track / via by id. Returns 1 on success. */
int32_t sieda_pcb_remove_track(SiedaProject* project, int32_t track_id);
int32_t sieda_pcb_remove_via(SiedaProject* project, int32_t via_id);

/* ---- signal & power integrity --------------------------------------------------------------------------------------- */
/* Parses an IBIS (.ibs, IBIS 4.x / 5.x) file for preview at corner "typ", "min" (weak / slow) or "max" (strong / fast):
 * {"version","fileName","components":[{name,manufacturer,pins:[{pin,signal,model}]}],"models":[{id,name,source,type,
 * modelType,vHigh,riseTime,fallTime,rOut,cComp,cIn,vih,vil,rPkg,lPkg,cPkg,note}],"warnings":[...]}. NULL with *error_out
 * (caller frees) when the text is not IBIS. Caller frees. */
char* sieda_ibis_parse(const char* text, const char* corner, char** error_out);
/* Imports every [Model] of an IBIS file into the project (ids "ibis:<model name>", replacing models of the same id). With
 * `ref` naming a component, its pins are mapped by number to the models of the file's first [Pin] list. Returns the
 * number of models imported, or 0 with *error_out (caller frees). */
int32_t sieda_si_import_ibis(SiedaProject* project, const char* text, const char* corner, const char* ref, char** error_out);
/* Assigns a driver / receiver model (a logic family id such as "lvcmos33", or "ibis:<model>") to `kind` "net" (net
 * name), "component" (reference) or "pin" ("U1.12", reference.pin number or name). An empty model_id clears the
 * assignment. 0 for an unknown kind or model. */
int32_t sieda_si_assign_model(SiedaProject* project, const char* kind, const char* target, const char* model_id);
/* sign_off = 1 adds the "Signal & Power Integrity" stage to design verification; overshoot and crosstalk limits are
 * fractions of the signal swing (e.g. 0.15, 0.05). */
int32_t sieda_si_set_options(SiedaProject* project, int32_t sign_off, double overshoot_limit, double crosstalk_limit);
/* Power-integrity inputs of a rail: allowed ripple (%), load step (A) and DC load (A); 0 derives the value. */
int32_t sieda_pi_set_rail(SiedaProject* project, const char* net_name, double ripple_percent, double transient_amps,
                          double dc_amps);
/* {"signOff","overshootLimit","crosstalkLimit","models":[...],"families":[...],"componentModels":{},"pinModels":{},
 * "netModels":{},"rails":[{net,ripplePercent,transientCurrent,dcCurrent}]}. Caller frees. */
char* sieda_si_settings_json(const SiedaProject* project);
/* Signal nets for the SI panel, critical and fast first: [{net,name,length,delay,critical,criticalLength,driver,model,
 * modelId,fast,routed,receivers}]. Caller frees. */
char* sieda_si_net_list_json(const SiedaProject* project);
/* Transmission-line analysis of one net: driver, terminations, line sections (layer, width, length, Z0, delay),
 * receivers with overshoot / undershoot / ringback / settling / flight time, termination advice and the waveforms
 * {"waveform":{time,source,driver,receiver},"terminatedWaveform":...}. series_ohms >= 0 adds a what-if series resistor
 * at the driver (< 0: as designed). {"error":...} for an unknown or non-signal net. Caller frees. */
char* sieda_si_net_json(const SiedaProject* project, const char* net_name, double series_ohms);
/* Crosstalk pairs and return-path issues: {"pairs":[{aggressor,victim,layer,coupledLength,spacing,next,fext,noise,limit,
 * ok,x,y}],"returnPath":[{code,net,message,x,y}],"limit"}. Caller frees. */
char* sieda_si_crosstalk_json(const SiedaProject* project);
/* Power distribution of every rail: {"rails":[{net,name,voltage,ripplePercent,transientCurrent,dcCurrent,target,vrmKind,
 * decaps:[...],plane:{...},curve:{freq:[],z:[]},peaks:[{f,z}],worstZ,worstF,compliant,irDrop:{analyzed,worst,loads:[...]},
 * recommendations:[...]}]}. Caller frees. */
char* sieda_pi_json(const SiedaProject* project);
/* Every signal / power-integrity finding (SI_* and PI_* codes) as a violations array. Caller frees. */
char* sieda_si_checks_json(const SiedaProject* project);

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

/* ---- sheets, hierarchy, buses, annotation -------------------------------------------------- */
/* A design has one or more named sheets; components belong to one sheet and wires stay on it. Nets cross sheets
 * through global net labels and ground symbols, and through hierarchical ports (labels with scope "port" on a child
 * sheet) joined to sheet entries (scope "entry") on its parent sheet. Netlist, ERC, simulation and PCB always see the
 * whole (flattened) design. New components are placed on the active sheet.
 * {"active": id, "sheets":[{"id","name","parent" (0 = top level),"depth","components","ports":["…"]}]} */
char* sieda_sheets_json(const SiedaProject* project);
/* Adds a sheet (unique, non-empty name) under `parent` (0 = top level). Returns its id or -1. */
int32_t sieda_add_sheet(SiedaProject* project, const char* name, int32_t parent);
int32_t sieda_rename_sheet(SiedaProject* project, int32_t sheet, const char* name);
/* Re-parents a sheet (0 = top level); 0 for a cycle. */
int32_t sieda_set_sheet_parent(SiedaProject* project, int32_t sheet, int32_t parent);
/* Moves a sheet to position `index` in the sheet order (tabs, annotation order). */
int32_t sieda_reorder_sheet(SiedaProject* project, int32_t sheet, int32_t index);
/* Removes a sheet; one that holds components only with delete_contents = 1. The last sheet stays. 1 on success. */
int32_t sieda_remove_sheet(SiedaProject* project, int32_t sheet, int32_t delete_contents);
/* The sheet new components go on. 1 on success. */
int32_t sieda_set_active_sheet(SiedaProject* project, int32_t sheet);
/* Moves `count` components to `sheet` (junctions joining only moved parts follow; wires that would cross sheets are
 * removed). Returns the number moved. */
int32_t sieda_move_to_sheet(SiedaProject* project, const int32_t* component_ids, int32_t count, int32_t sheet);
/* Net label scope: "global" (default), "local" (its sheet only), "port" (hierarchical port) or "entry" (sheet entry
 * into target_sheet). 1 on success. */
int32_t sieda_set_label_scope(SiedaProject* project, int32_t component_id, const char* scope, int32_t target_sheet);
/* Sheet symbol: adds a sheet entry on the parent sheet for every port of `child` that has none, stacked from (x, y).
 * Returns the entries added, or -1 for an unknown or top-level sheet. */
int32_t sieda_place_sheet_entries(SiedaProject* project, int32_t child, double x, double y);
/* Bus notation ("D[0..7]", "A[15..12]", "D[0..3],WR") expanded to its members as a JSON array ("[]" if not a bus). */
char* sieda_expand_bus(const char* bus);
/* One net label per bus member on the given pins of a component (in order), placed outside each pin and wired.
 * scope: "global", "local" or "port" (NULL = global). Returns the labels added or -1. */
int32_t sieda_add_bus_labels(SiedaProject* project, int32_t component_id, const int32_t* pins, int32_t count,
                             const char* bus, const char* scope);
/* Re-numbers reference designators. options_json: {"order":"rows"|"columns","keepExisting":bool,
 * "sheetNumbering":bool} (NULL = rows, renumber all). Returns {"changed":[{"component","from","to"}]}. */
char* sieda_annotate(SiedaProject* project, const char* options_json);

/* ---- design variants ----------------------------------------------------------------------- */
/* Named assembly variants: per component fitted / not fitted (DNP) and value overrides. The active variant ("" =
 * base design) drives sieda_bom_json, the "bom", "bom_assembly", "cpl", "pnp", "assembly_*" exports and the
 * assembly files of the fabrication package, and simulation: sieda_simulate_*, the live board and
 * sieda_spice_netlist run the variant as assembled (values applied, unfitted / DNP parts left out; their results add
 * "variant" and "omitted":[refs]). Variants never change connectivity or the board.
 * {"active":"…","variants":[{"name","description","parts":[{"component","ref","fitted"?,"value"?}]}]} */
char* sieda_variants_json(const SiedaProject* project);
/* Adds a variant, optionally copying copy_from (NULL/"" = empty). 1 on success, 0 for an empty or taken name. */
int32_t sieda_add_variant(SiedaProject* project, const char* name, const char* copy_from);
int32_t sieda_rename_variant(SiedaProject* project, const char* name, const char* new_name);
int32_t sieda_remove_variant(SiedaProject* project, const char* name);
int32_t sieda_set_variant_description(SiedaProject* project, const char* name, const char* description);
/* fitted: -1 as the base design, 0 not fitted (DNP), 1 fitted. value: NULL keeps the override, "" clears it. */
int32_t sieda_set_variant_part(SiedaProject* project, const char* name, int32_t component_id, int32_t fitted,
                               const char* value);
/* "" or NULL selects the base design. 1 on success, 0 for an unknown variant. */
int32_t sieda_set_active_variant(SiedaProject* project, const char* name);
/* An assembly export ("bom", "bom_assembly", "cpl", "pnp", "assembly_top", "assembly_bottom") or the BOM JSON
 * ("bom_json") for a given variant ("" = base design). NULL for an unknown format or variant. */
char* sieda_export_variant(const SiedaProject* project, const char* format, const char* variant);

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
/* 1 byte per vertex: what the surface is made of (0 mask, 1 laminate, 2 copper finish, 3 gold, 4 tin, 5 solder,
   6 silkscreen, 7 plastic, 8 ceramic, 9 glass, 10 drilled hole, 11 package marking). */
const uint8_t* sieda_mesh_surfaces(const SiedaMesh* mesh);
const uint32_t* sieda_mesh_indices(const SiedaMesh* mesh);

/* ---- schematic capture: repeated sheets ----------------------------------------------------- */
/* Repeated (multi-instance) sheet: one block drawn once and used `count` times (the sheet itself is the first
 * channel). Each extra channel is a sheet of its own ("<name> [B]" …, same parent) holding copies of the block with
 * their own designators, nets, footprints and variant settings; edits to any channel go to the block. Only a sheet
 * without child sheets can be repeated; 1 ends the repetition. Returns the number of channels, or -1.
 * sieda_sheets_json and the snapshot add "instanceOf" (definition sheet; 0 on the definition), "channel", "refs" and
 * "instances" to repeated sheets; components add "instanceOf" (the block part copied) and "logicalRef". */
int32_t sieda_repeat_sheet(SiedaProject* project, int32_t sheet, int32_t count);
/* Channel designators: "sheet" (R1 → R201, R301 … by sheet number) or "suffix" (R1_A, R1_B …). 1 on success. */
int32_t sieda_set_instance_refs(SiedaProject* project, int32_t sheet, const char* scheme);
/* Channel label of a repeated sheet (letters, digits, '_' or '-'; unique in the block). 1 on success. */
int32_t sieda_set_sheet_channel(SiedaProject* project, int32_t sheet, const char* channel);

/* ---- schematic capture: graphical buses ---------------------------------------------------- */
/* A bus is a named polyline ("D[0..7]") on a sheet. Its members leave it through bus entries: net labels attached to
 * it ("bus": id in the snapshot) that join nets by name (scope "local" by default). The snapshot lists
 * "buses":[{"id","sheet","name","points":[{x,y}],"members":[…],"instanceOf"?}].
 * Adds a bus on the active sheet; points_json is [{"x":…,"y":…},…] (2 … 256 points). Returns its id or -1. */
int32_t sieda_add_bus(SiedaProject* project, const char* name, const char* points_json);
/* Removes a bus with its entries. 1 on success. */
int32_t sieda_remove_bus(SiedaProject* project, int32_t bus);
int32_t sieda_rename_bus(SiedaProject* project, int32_t bus, const char* name);
/* Moves a bus and its entries. 1 on success. */
int32_t sieda_move_bus(SiedaProject* project, int32_t bus, double dx, double dy);
/* Rips entries out for the members in members_json (["D0","D1"]; NULL or "[]" = every member without one), spaced
 * along the bus. scope: "local" (NULL), "global" or "port". Returns the entries added, -1 on bad input. */
int32_t sieda_rip_bus_entries(SiedaProject* project, int32_t bus, const char* members_json, const char* scope);
/* Wires bus members to the pins of a part they name (D0 → pin "D0"), else to its open pins in order, each through
 * an entry on the bus. Returns the connections made, -1 for an unknown bus or part. */
int32_t sieda_connect_bus_to_part(SiedaProject* project, int32_t bus, int32_t component_id, const char* scope);

/* ---- schematic capture: multi-unit parts ---------------------------------------------------- */
/* A custom part spec may list "units":[{"name":"A","pins":["1","2","3"]},…] (pins in no unit form a power unit "P").
 * Placing it by units puts a hidden package (every pin, the footprint: what the netlist, BOM and PCB see) and the
 * symbol of unit A; further units are placed on their own and join the same package. In the snapshot a unit is a
 * custom part (kind 16) with "unit", "unitName" and "unitOf" (its package); the package has "unitPackage":true and
 * "units"; custom part JSON adds "unitSymbols". sieda_annotate takes "packUnits":true to re-assign interchangeable
 * units to packages in placement order. sieda_add_custom_component still places such a part whole.
 * Returns the id of unit A, or -1 for an unknown part or one without units. */
int32_t sieda_add_custom_units(SiedaProject* project, const char* part_id, const char* value, double x, double y,
                               int32_t rotation, const char* ref);
/* Places unit `unit` (1-based) of the package of component_id (the package or any of its units). -1 when the unit is
 * out of range or already placed. */
int32_t sieda_add_part_unit(SiedaProject* project, int32_t component_id, int32_t unit, double x, double y, int32_t rotation);
/* Places the first unit not placed yet; -1 when all are placed. */
int32_t sieda_place_next_unit(SiedaProject* project, int32_t component_id, double x, double y);

/* ---- schematic capture: find / replace, net navigator, title block ------------------------- */
/* Finds text across every sheet. request: {"text","matchCase"?,"wholeWord"?,"fields"?:["ref","value","label","net",
 * "pin"]} (default: all but pins). Returns {"hits":[{"component","net","pin","sheet","field","text"}]} in sheet
 * order (a net hit names the first place the net appears). */
char* sieda_schematic_find(const SiedaProject* project, const char* request_json);
/* Replaces text in part values and net label names. request: {"text","replacement","matchCase"?,"wholeWord"?,
 * "fields"?:["value","label"]}. Repeated-sheet blocks and multi-unit parts are edited once. Returns the fields
 * changed (0 on bad input). */
int32_t sieda_schematic_replace(SiedaProject* project, const char* request_json);
/* Net navigator: {"net","name","places":[{"component","pin","sheet","x","y","kind":"pin"|"label"|"global"|"port"|
 * "entry"|"bus"|"ground","ref","name"}]}, in sheet order. */
char* sieda_net_places(const SiedaProject* project, int32_t net);
/* Title block fields (any of "title","company","revision","date","drawnBy"; up to 256 characters each). The
 * snapshot carries "titleBlock" (title defaults to the project name). 1 on success. */
int32_t sieda_set_title_block(SiedaProject* project, const char* json);

#ifdef __cplusplus
}
#endif

#endif /* SIEDA_C_H */
