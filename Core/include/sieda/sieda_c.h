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
int32_t sieda_remove_wire(SiedaProject* project, int32_t wire_id);

/* ---- analysis ------------------------------------------------------------------------------ */
char* sieda_run_erc(const SiedaProject* project);
char* sieda_simulate_dc(const SiedaProject* project);
char* sieda_simulate_transient(const SiedaProject* project, double t_stop, double t_step);
char* sieda_spice_netlist(const SiedaProject* project);

/* ---- PCB ----------------------------------------------------------------------------------- */
void sieda_pcb_set_board(SiedaProject* project, double width, double height, double track_width, double clearance);
/* all = 1 re-places every footprint; 0 only places footprints that are not on the board yet. */
void sieda_pcb_autoplace(SiedaProject* project, int32_t all);
int32_t sieda_pcb_move_footprint(SiedaProject* project, int32_t component_id, double x, double y);
int32_t sieda_pcb_rotate_footprint(SiedaProject* project, int32_t component_id, int32_t delta_degrees);
int32_t sieda_pcb_flip_footprint(SiedaProject* project, int32_t component_id);
/* Resizes the board outline to the placed footprints plus margin_mm, keeping parts and copper together. */
int32_t sieda_pcb_fit_board(SiedaProject* project, double margin_mm);
char* sieda_pcb_autoroute(SiedaProject* project); /* JSON route statistics */
void sieda_pcb_clear_routing(SiedaProject* project);
char* sieda_pcb_run_drc(const SiedaProject* project);

/* ---- exports ------------------------------------------------------------------------------- */
/* format: "spice", "bom", "pnp", "gerber_top", "gerber_bottom", "gerber_mask_top", "gerber_mask_bottom",
 *         "gerber_silk_top", "gerber_edge", "drill", "stl", "obj". Returns NULL for unknown formats. */
char* sieda_export(const SiedaProject* project, const char* format);

/* ---- 3D ------------------------------------------------------------------------------------ */
SiedaMesh* sieda_mesh_build(const SiedaProject* project, int32_t include_components);
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
