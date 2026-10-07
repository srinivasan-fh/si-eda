/* Compiled as C to guarantee sieda_c.h stays a valid C header (it is what Swift imports). */
#include <stdio.h>
#include <string.h>

#include "sieda/sieda_c.h"

int sieda_c_api_smoke_test(void) {
    SiedaProject* p = sieda_project_new("C API smoke test");
    if (!p) return 1;
    int32_t v = sieda_add_component(p, 5 /* VoltageSource */, "9", 0, 0, 0, NULL);
    int32_t r = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    int32_t g = sieda_add_component(p, 7 /* Ground */, NULL, 0, 80, 0, NULL);
    if (v < 0 || r < 0 || g < 0) return 2;
    if (sieda_connect(p, v, sieda_find_pin(p, v, "+"), r, 0) < 0) return 3;
    if (sieda_connect(p, r, 1, g, 0) < 0) return 4;
    if (sieda_connect(p, v, 1, g, 0) < 0) return 5;
    if (sieda_find_component(p, "R1") != r) return 6;

    char* dc = sieda_simulate_dc(p);
    if (!dc || !strstr(dc, "\"converged\":true")) return 7;
    sieda_string_free(dc);

    /* Advanced analyses: JSON options in, {"ok",…} out; bad options are reported, never thrown. */
    {
        char* ac = sieda_simulate_ac(p, "{\"source\":\"V1\",\"start\":10,\"stop\":\"1k\",\"pointsPerDecade\":10}");
        if (!ac || !strstr(ac, "\"ok\":true") || !strstr(ac, "\"magnitudeDb\":[")) return 40;
        sieda_string_free(ac);
        char* sweep = sieda_simulate_dc_sweep(p, "{\"source\":\"V1\",\"start\":0,\"stop\":9,\"step\":1}");
        if (!sweep || !strstr(sweep, "\"ok\":true") || !strstr(sweep, "\"values\":[0,1,2,3,4,5,6,7,8,9]")) return 41;
        sieda_string_free(sweep);
        char* param = sieda_simulate_param_sweep(p, "{\"component\":\"R1\",\"values\":[\"1k\",\"2k\"],\"analysis\":\"dc\"}");
        if (!param || !strstr(param, "\"ok\":true") || !strstr(param, "\"value\":\"2k\"")) return 42;
        sieda_string_free(param);
        char* mc = sieda_simulate_monte_carlo(p, "{\"net\":\"no such net\"}");
        if (!mc || !strstr(mc, "\"ok\":false")) return 43;
        sieda_string_free(mc);
        char* bad = sieda_simulate_fft(p, "{not json");
        if (!bad || !strstr(bad, "\"ok\":false") || !strstr(bad, "Invalid analysis options")) return 44;
        sieda_string_free(bad);
        if (sieda_simulate_ac(NULL, NULL) != NULL) return 45;
    }

    char* routed = sieda_pcb_autoroute(p);
    if (!routed || !strstr(routed, "\"failed\":0")) return 8;
    sieda_string_free(routed);

    char* verification = sieda_run_verification(p);
    if (!verification || !strstr(verification, "\"stages\":[") || !strstr(verification, "Design Verification Report")) return 22;
    sieda_string_free(verification);

    SiedaMesh* m = sieda_mesh_build(p, 1);
    if (!m || sieda_mesh_vertex_count(m) <= 0 || sieda_mesh_index_count(m) % 3 != 0) return 9;
    {
        /* Every vertex carries a surface tag (mask, finish, solder, silk, plastic…). */
        const uint8_t* surfaces = sieda_mesh_surfaces(m);
        if (!surfaces) return 9;
        for (int32_t v = 0; v < sieda_mesh_vertex_count(m); ++v)
            if (surfaces[v] > 11) return 9;
    }
    sieda_mesh_free(m);

    char* saved = sieda_project_save_json(p);
    char* loadErr = NULL;
    SiedaProject* q = sieda_project_load_json(saved, &loadErr);
    sieda_string_free(saved);
    if (!q || loadErr) return 10;
    if (sieda_find_component(q, "R1") < 0) return 11;
    sieda_project_free(q);

    if (sieda_export(p, "nonsense") != NULL) return 12;

    /* Custom part from a datasheet-style spec, placed and routed on a 4-layer board. */
    char* err = NULL;
    char* part = sieda_custom_part_register(p,
        "{\"name\":\"LM7805\",\"package\":{\"type\":\"TO-220\"},\"pins\":["
        "{\"number\":\"1\",\"name\":\"IN\",\"type\":\"power_in\"},"
        "{\"number\":\"2\",\"name\":\"GND\",\"type\":\"power_in\"},"
        "{\"number\":\"3\",\"name\":\"OUT\",\"type\":\"power_out\"}]}", &err);
    if (!part || err) return 13;
    const char* idStart = strstr(part, "\"id\":\"");
    if (!idStart) return 14;
    char id[64] = {0};
    idStart += 6;
    for (int i = 0; i < 63 && idStart[i] != '"'; ++i) id[i] = idStart[i];
    sieda_string_free(part);
    int32_t u = sieda_add_custom_component(p, id, NULL, 300, 0, 0, NULL);
    if (u < 0) return 15;
    if (sieda_find_pin(p, u, "OUT") != 2) return 16;
    if (sieda_custom_part_register(p, "{\"name\":\"\",\"pins\":[]}", &err) != NULL || !err) return 17;
    sieda_string_free(err);
    sieda_pcb_set_layer_count(p, 4);
    char* routed4 = sieda_pcb_autoroute(p);
    if (!routed4) return 18;
    sieda_string_free(routed4);
    SiedaMesh* layer = sieda_mesh_build_layer(p, 1);
    if (!layer) return 19;
    sieda_mesh_free(layer);
    char* inner = sieda_export(p, "gerber_l2");
    if (!inner || !strstr(inner, "Copper,L2,Inr")) return 20;
    sieda_string_free(inner);
    if (sieda_export(p, "gerber_l9") != NULL) return 21;
    if (!sieda_project_set_industry(p, "automotive") || sieda_project_set_industry(p, "nope")) return 25;
    {
        char* profiles = sieda_industry_profiles_json();
        if (!profiles || !strstr(profiles, "\"id\":\"space\"")) return 26;
        sieda_string_free(profiles);
    }
    {
        double parsed = 0;
        if (!sieda_parse_value("4k7", &parsed) || parsed < 4699.0 || parsed > 4701.0) return 23;
        if (sieda_parse_value("abc", &parsed)) return 24;
    }
    {
        /* Signal & power integrity: model assignment, sign-off, the analyses and an IBIS import error. */
        char* ibisErr = NULL;
        char* json = NULL;
        if (sieda_si_import_ibis(p, "not an ibis file", "typ", "", &ibisErr) != 0 || !ibisErr) return 60;
        sieda_string_free(ibisErr);
        if (!sieda_si_assign_model(p, "component", "R1", "lvcmos33") || sieda_si_assign_model(p, "net", "X", "nope")) return 61;
        if (!sieda_si_set_options(p, 1, 0.15, 0.05) || !sieda_pi_set_rail(p, "VCC", 5, 0.1, 0.2)) return 62;
        json = sieda_si_net_list_json(p);
        if (!json || json[0] != '[') return 63;
        sieda_string_free(json);
        json = sieda_pi_json(p);
        if (!json || !strstr(json, "\"rails\"")) return 64;
        sieda_string_free(json);
        json = sieda_si_crosstalk_json(p);
        if (!json || !strstr(json, "\"pairs\"")) return 65;
        sieda_string_free(json);
        json = sieda_run_verification(p);
        if (!json || !strstr(json, "Signal & Power Integrity")) return 66;
        sieda_string_free(json);
        json = sieda_si_settings_json(p);
        if (!json || !strstr(json, "\"families\"")) return 67;
        sieda_string_free(json);
        sieda_si_set_options(p, 0, 0.15, 0.05);
    }
    sieda_project_free(p);
    return 0;
}

/* Sheets, hierarchy, buses, annotation and design variants through the C ABI. */
int sieda_c_api_sheets_test(void) {
    SiedaProject* p = sieda_project_new("Sheets");
    if (!p) return 1;
    int32_t power = 1;
    int32_t load = sieda_add_sheet(p, "Load", 0);
    if (load <= power || sieda_add_sheet(p, "Load", 0) != -1) return 2;
    if (!sieda_rename_sheet(p, power, "Power")) return 3;
    int32_t v = sieda_add_component(p, 5, "5", 0, 0, 0, NULL);
    int32_t g = sieda_add_component(p, 7, NULL, 0, 80, 0, NULL);
    int32_t l1 = sieda_add_component(p, 15, "VIN", 60, 0, 0, NULL);
    if (sieda_connect(p, v, 0, l1, 0) < 0 || sieda_connect(p, v, 1, g, 0) < 0) return 4;
    if (!sieda_set_active_sheet(p, load) || sieda_set_active_sheet(p, 999)) return 5;
    int32_t r = sieda_add_component(p, 0, "1k", 100, 0, 0, NULL);
    int32_t l2 = sieda_add_component(p, 15, "VIN", 40, 0, 0, NULL);
    int32_t g2 = sieda_add_component(p, 7, NULL, 160, 80, 0, NULL);
    if (sieda_connect(p, l2, 0, r, 0) < 0 || sieda_connect(p, r, 1, g2, 0) < 0) return 6;
    if (sieda_connect(p, v, 0, r, 0) != -1) return 7; /* wires never cross sheets */
    {
        char* dc = sieda_simulate_dc(p);
        if (!dc || !strstr(dc, "\"converged\":true") || !strstr(dc, "\"name\":\"VIN\",\"voltage\":5")) return 8;
        sieda_string_free(dc);
    }
    {
        char* sheets = sieda_sheets_json(p);
        if (!sheets || !strstr(sheets, "\"name\":\"Load\"") || !strstr(sheets, "\"components\":3")) return 9;
        sieda_string_free(sheets);
        char* snap = sieda_project_snapshot(p);
        if (!snap || !strstr(snap, "\"sheets\":[") || !strstr(snap, "\"activeSheet\":")) return 10;
        sieda_string_free(snap);
    }
    /* Hierarchy: a child sheet with a port, its entry on the parent. */
    int32_t child = sieda_add_sheet(p, "Child", load);
    if (child < 0 || !sieda_set_sheet_parent(p, child, load) || sieda_set_sheet_parent(p, load, child)) return 11;
    sieda_set_active_sheet(p, child);
    int32_t port = sieda_add_component(p, 15, "SIG", 0, 0, 0, NULL);
    if (!sieda_set_label_scope(p, port, "port", 0) || sieda_set_label_scope(p, port, "bogus", 0)) return 12;
    if (sieda_place_sheet_entries(p, child, 300, 0) != 1 || sieda_place_sheet_entries(p, power, 0, 0) != -1) return 13;
    if (sieda_add_component(p, 15, "LOOSE", 0, 60, 0, NULL) < 0) return 14;
    {
        /* The port and its entry join (no dangling-label report for them); a loose label is reported with its sheet. */
        char* erc = sieda_run_erc(p);
        if (!erc || !strstr(erc, "\"code\":\"ERC_DANGLING_LABEL\"") || strstr(erc, "SIG") || !strstr(erc, "\"sheet\":")) return 14;
        sieda_string_free(erc);
    }
    {
        int32_t ids[1];
        ids[0] = r;
        if (sieda_move_to_sheet(p, ids, 1, power) != 1) return 15;
        if (!sieda_reorder_sheet(p, child, 0)) return 16;
    }
    /* Buses. */
    {
        char* members = sieda_expand_bus("D[0..2]");
        if (!members || strcmp(members, "[\"D0\",\"D1\",\"D2\"]") != 0) return 17;
        sieda_string_free(members);
        int32_t j = sieda_add_component(p, 12, NULL, 400, 0, 0, NULL);
        int32_t pins[2] = {0, 1};
        if (sieda_add_bus_labels(p, j, pins, 2, "A[1..0]", "local") != 2) return 18;
        if (sieda_add_bus_labels(p, j, pins, 2, "A[0..5]", NULL) != -1) return 19;
    }
    /* Annotation. */
    {
        int32_t extra = sieda_add_component(p, 0, "2k", 0, 200, 0, "R1");
        char* changed = sieda_annotate(p, "{\"order\":\"rows\",\"keepExisting\":true}");
        if (!changed || !strstr(changed, "\"changed\":[{")) return 20;
        sieda_string_free(changed);
        if (sieda_find_component(p, "R1") < 0 || extra < 0) return 21;
    }
    /* Variants. */
    if (!sieda_add_variant(p, "Lite", NULL) || sieda_add_variant(p, "Lite", NULL)) return 22;
    if (!sieda_set_variant_part(p, "Lite", r, 0, NULL) || !sieda_set_variant_part(p, "Lite", r, 0, "4k7")) return 23;
    if (sieda_set_variant_part(p, "Lite", g, 0, NULL)) return 24;
    if (!sieda_add_variant(p, "Pro", "Lite") || !sieda_rename_variant(p, "Pro", "Full")) return 25;
    if (!sieda_set_variant_description(p, "Full", "Everything fitted")) return 26;
    {
        char* bom = sieda_export_variant(p, "bom", "Lite");
        if (!bom || !strstr(bom, "4k7") || !strstr(strchr(bom, 10), ",DNP\n")) return 27;
        sieda_string_free(bom);
        if (sieda_export_variant(p, "bom", "Nope") != NULL || sieda_export_variant(p, "gerber_top", "") != NULL) return 28;
        char* base = sieda_export(p, "bom");
        if (!base || strstr(strchr(base, 10), ",DNP\n")) return 29;
        sieda_string_free(base);
        if (!sieda_set_active_variant(p, "Lite") || sieda_set_active_variant(p, "Nope")) return 30;
        char* active = sieda_export(p, "bom");
        if (!active || !strstr(strchr(active, 10), ",DNP\n")) return 31;
        sieda_string_free(active);
        char* json = sieda_bom_json(p);
        if (!json || !strstr(json, "\"dnp\":true")) return 32;
        sieda_string_free(json);
        char* list = sieda_variants_json(p);
        if (!list || !strstr(list, "\"active\":\"Lite\"") || !strstr(list, "\"name\":\"Full\"")) return 33;
        sieda_string_free(list);
    }
    /* Round trip. */
    {
        char* saved = sieda_project_save_json(p);
        char* err = NULL;
        SiedaProject* q = sieda_project_load_json(saved, &err);
        sieda_string_free(saved);
        if (!q || err) return 34;
        char* list = sieda_variants_json(q);
        if (!list || !strstr(list, "\"active\":\"Lite\"")) return 35;
        sieda_string_free(list);
        char* sheets = sieda_sheets_json(q);
        if (!sheets || !strstr(sheets, "\"name\":\"Child\"")) return 36;
        sieda_string_free(sheets);
        sieda_project_free(q);
    }
    if (!sieda_remove_variant(p, "Lite") || sieda_remove_variant(p, "Lite")) return 37;
    if (sieda_remove_sheet(p, power, 0)) return 38; /* still holds parts */
    if (!sieda_remove_sheet(p, child, 1)) return 39;
    sieda_project_free(p);
    return 0;
}

/* Library import: a KiCad footprint becomes an importable part; bad requests and files are reported, never NULL. */
int sieda_c_api_library_import_test(void) {
    char* result = sieda_library_import(
        "{\"files\":[{\"name\":\"R.kicad_mod\",\"content\":\"(footprint \\\"R_0603\\\" "
        "(pad \\\"1\\\" smd rect (at -0.8 0) (size 0.8 0.95) (layers \\\"F.Cu\\\")) "
        "(pad \\\"2\\\" smd rect (at 0.8 0) (size 0.8 0.95) (layers \\\"F.Cu\\\")))\"},"
        "{\"name\":\"bad.lbr\",\"content\":\"<eagle>\"}]}");
    if (!result || !strstr(result, "\"ok\":true") || !strstr(result, "\"name\":\"R_0603\"")) return 1;
    if (!strstr(result, "\"format\":\"eagle_lbr\"") || !strstr(result, "never closed")) return 2;
    sieda_string_free(result);
    result = sieda_library_import("not json");
    if (!result || !strstr(result, "Invalid import request")) return 3;
    sieda_string_free(result);
    result = sieda_library_import(NULL);
    if (!result || !strstr(result, "\"parts\":[]")) return 4;
    sieda_string_free(result);
    return 0;
}

/* Interactive routing through the C API: begin on a pad, preview, place a via, finish on the other pad, commit. */
int sieda_c_api_router_test(void) {
    SiedaProject* p = sieda_project_new("C API router");
    if (!p) return 1;
    int32_t r1 = sieda_add_component(p, 0 /* Resistor */, "1k", 0, 0, 0, NULL);
    int32_t r2 = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    if (r1 < 0 || r2 < 0 || sieda_connect(p, r1, 1, r2, 0) < 0) return 2;
    if (!sieda_pcb_move_footprint(p, r1, 10, 20) || !sieda_pcb_move_footprint(p, r2, 30, 20)) return 3;
    if (sieda_router_active(p)) return 4;

    char* bad = sieda_router_begin(p, NULL, 2, 2, 0); /* nothing there */
    if (!bad || !strstr(bad, "\"error\"") || sieda_router_active(p)) return 5;
    sieda_string_free(bad);

    char* begin = sieda_router_begin(p, "{\"mode\":\"shove\",\"posture\":\"45\"}", 10.95, 20, 0);
    if (!begin || strstr(begin, "\"error\"") || !strstr(begin, "\"active\":true") || !sieda_router_active(p)) return 6;
    sieda_string_free(begin);
    char* move = sieda_router_move(p, 18, 20);
    if (!move || !strstr(move, "\"head\":[{")) return 7;
    sieda_string_free(move);
    char* via = sieda_router_add_via(p, -1);
    if (!via || strstr(via, "\"error\"") || !strstr(via, "\"layer\":1")) return 8;
    sieda_string_free(via);
    char* options = sieda_router_set_options(p, "{\"posture\":\"90\"}");
    if (!options || strstr(options, "\"error\"")) return 9;
    sieda_string_free(options);
    char* across = sieda_router_move(p, 24, 20);
    if (!across) return 9;
    sieda_string_free(across);
    char* back = sieda_router_add_via(p, 0); /* the target pad is an SMD pad on top */
    if (!back || strstr(back, "\"error\"") || !strstr(back, "\"layer\":0")) return 9;
    sieda_string_free(back);
    char* end = sieda_router_move(p, 29.05, 20);
    if (!end || !strstr(end, "\"reachedTarget\":true")) return 10;
    sieda_string_free(end);
    char* commit = sieda_router_commit(p);
    if (!commit || !strstr(commit, "\"ok\":true") || !strstr(commit, "\"addedVias\":[")) return 11;
    sieda_string_free(commit);
    if (sieda_router_active(p)) return 12;

    char* snap = sieda_project_snapshot(p);
    if (!snap) return 13;
    const char* tracksAt = strstr(snap, "\"tracks\":[{");
    const char* idAt = tracksAt ? strstr(tracksAt, "\"id\":") : NULL;
    if (!idAt) return 14;
    int32_t track = 0;
    for (const char* c = idAt + 5; *c >= '0' && *c <= '9'; ++c) track = track * 10 + (*c - '0');
    sieda_string_free(snap);
    if (!sieda_pcb_lock_track(p, track, 1) || sieda_pcb_lock_track(p, -5, 1)) return 15;
    char* drag = sieda_router_begin_drag(p, NULL, track, 15, 20);
    if (!drag || !strstr(drag, "\"error\"")) return 16; /* locked */
    sieda_string_free(drag);
    if (!sieda_pcb_lock_track(p, track, 0)) return 17;

    char* tune = sieda_router_tune_length(p, track, 0, 0); /* not in a matched group */
    if (!tune || !strstr(tune, "\"ok\":false")) return 18;
    sieda_string_free(tune);
    char* pair = sieda_router_begin_pair(p, NULL, 10.95, 20, 0); /* not a differential pair */
    if (!pair || !strstr(pair, "\"error\"")) return 19;
    sieda_string_free(pair);
    sieda_router_cancel(p);

    if (!sieda_pcb_remove_track(p, track) || sieda_pcb_remove_track(p, track)) return 20;
    sieda_project_free(p);
    return 0;
}

/* Schematic capture: repeated sheets (channels). Returns 0 or the failing step. */
int sieda_c_api_capture_test(void) {
    SiedaProject* p = sieda_project_new("Capture");
    if (!p) return 1;
    int32_t block = sieda_add_sheet(p, "Amp", 1);
    if (block < 0 || !sieda_set_active_sheet(p, block)) return 2;
    int32_t in = sieda_add_component(p, 15, "IN", 0, 0, 0, NULL);
    int32_t r = sieda_add_component(p, 0, "10k", 80, 0, 0, NULL);
    if (!sieda_set_label_scope(p, in, "port", 0) || sieda_connect(p, in, 0, r, 0) < 0) return 3;
    if (sieda_repeat_sheet(p, block, 4) != 4 || sieda_repeat_sheet(p, block, 0) != -1) return 4;
    if (sieda_find_component(p, "R201") != r || sieda_find_component(p, "R501") < 0) return 5;
    {
        char* sheets = sieda_sheets_json(p);
        if (!sheets || !strstr(sheets, "\"name\":\"Amp [D]\"") || !strstr(sheets, "\"instances\":4") ||
            !strstr(sheets, "\"channel\":\"C\"")) return 6;
        sieda_string_free(sheets);
    }
    if (!sieda_set_instance_refs(p, block, "suffix") || sieda_set_instance_refs(p, block, "bogus")) return 7;
    if (sieda_find_component(p, "R1_D") < 0 || sieda_find_component(p, "R1_A") != r) return 8;
    if (!sieda_set_sheet_channel(p, block, "L") || sieda_set_sheet_channel(p, block, "") ||
        sieda_find_component(p, "R1_L") != r) return 9;
    {
        char* snap = sieda_project_snapshot(p);
        if (!snap || !strstr(snap, "\"logicalRef\":\"R1\"") || !strstr(snap, "\"instanceOf\":")) return 10;
        sieda_string_free(snap);
        char* saved = sieda_project_save_json(p);
        char* err = NULL;
        SiedaProject* q = sieda_project_load_json(saved, &err);
        sieda_string_free(saved);
        if (!q || err || sieda_find_component(q, "R1_D") < 0) return 11;
        sieda_project_free(q);
    }
    if (sieda_repeat_sheet(p, block, 1) != 1 || sieda_find_component(p, "R1") != r) return 12;
    sieda_project_free(p);
    return 0;
}

/* Graphical buses through the C API. Returns 0 or the failing step. */
int sieda_c_api_bus_test(void) {
    SiedaProject* p = sieda_project_new("Buses");
    if (!p) return 1;
    int32_t j1 = sieda_add_component(p, 12, NULL, 0, 0, 0, NULL);
    int32_t j2 = sieda_add_component(p, 12, NULL, 300, 0, 0, NULL);
    if (sieda_add_bus(p, "nope", "[{\"x\":0,\"y\":0},{\"x\":0,\"y\":50}]") != -1) return 2;
    if (sieda_add_bus(p, "D[0..1]", "not json") != -1 || sieda_add_bus(p, "D[0..1]", NULL) != -1) return 3;
    int32_t bus = sieda_add_bus(p, "D[0..1]", "[{\"x\":150,\"y\":-50},{\"x\":150,\"y\":50}]");
    if (bus <= 0) return 4;
    if (sieda_connect_bus_to_part(p, bus, j1, NULL) != 2 || sieda_connect_bus_to_part(p, bus, j2, "local") != 2) return 5;
    if (sieda_connect_bus_to_part(p, bus, j1, "entry") != -1 || sieda_connect_bus_to_part(p, 999, j1, NULL) != -1) return 6;
    {
        char* snap = sieda_project_snapshot(p);
        if (!snap || !strstr(snap, "\"buses\":[{") || !strstr(snap, "\"members\":[\"D0\",\"D1\"]") ||
            !strstr(snap, "\"bus\":")) return 7;
        sieda_string_free(snap);
    }
    int32_t bus2 = sieda_add_bus(p, "A[0..3]", "[{\"x\":0,\"y\":200},{\"x\":200,\"y\":200}]");
    if (sieda_rip_bus_entries(p, bus2, "[\"A0\"]", NULL) != 1 || sieda_rip_bus_entries(p, bus2, NULL, "global") != 3) return 8;
    if (sieda_rip_bus_entries(p, bus2, "[\"Z\"]", NULL) != -1 || sieda_rip_bus_entries(p, bus2, "{", NULL) != -1) return 9;
    if (!sieda_move_bus(p, bus2, 10, 0) || !sieda_rename_bus(p, bus2, "A[0..4]") || sieda_rename_bus(p, bus2, "x")) return 10;
    {
        char* erc = sieda_run_erc(p);
        if (!erc || !strstr(erc, "ERC_DANGLING_LABEL")) return 11; /* the ripped entries are not wired yet */
        sieda_string_free(erc);
    }
    if (!sieda_remove_bus(p, bus2) || sieda_remove_bus(p, bus2)) return 12;
    sieda_project_free(p);
    return 0;
}

/* Multi-unit parts through the C API. Returns 0 or the failing step. */
int sieda_c_api_units_test(void) {
    SiedaProject* p = sieda_project_new("Units");
    if (!p) return 1;
    char* err = NULL;
    char* info = sieda_custom_part_register(p,
        "{\"name\":\"DUAL-CAPI\",\"refPrefix\":\"U\",\"package\":{\"type\":\"SOIC\"},\"pins\":["
        "{\"number\":\"1\",\"name\":\"OUTA\",\"type\":\"output\"},{\"number\":\"2\",\"name\":\"INA-\",\"type\":\"input\"},"
        "{\"number\":\"3\",\"name\":\"INA+\",\"type\":\"input\"},{\"number\":\"4\",\"name\":\"V-\",\"type\":\"power_in\"},"
        "{\"number\":\"5\",\"name\":\"INB+\",\"type\":\"input\"},{\"number\":\"6\",\"name\":\"INB-\",\"type\":\"input\"},"
        "{\"number\":\"7\",\"name\":\"OUTB\",\"type\":\"output\"},{\"number\":\"8\",\"name\":\"V+\",\"type\":\"power_in\"}],"
        "\"units\":[{\"name\":\"A\",\"pins\":[\"1\",\"2\",\"3\"]},{\"name\":\"B\",\"pins\":[\"7\",\"6\",\"5\"]}]}",
        &err);
    if (!info || err) return 2;
    const char* at = strstr(info, "\"id\":\"");
    if (!at || !strstr(info, "\"unitSymbols\":[")) return 3;
    char id[128];
    at += 6;
    size_t n = 0;
    while (at[n] && at[n] != '"' && n < sizeof id - 1) { id[n] = at[n]; ++n; }
    id[n] = 0;
    sieda_string_free(info);
    int32_t a = sieda_add_custom_units(p, id, NULL, 0, 0, 0, NULL);
    if (a < 0 || sieda_add_custom_units(p, "NO-SUCH-PART", NULL, 0, 0, 0, NULL) != -1) return 4;
    int32_t b = sieda_place_next_unit(p, a, 0, 200);
    int32_t pw = sieda_add_part_unit(p, a, 3, 200, 0, 0);
    if (b < 0 || pw < 0 || sieda_place_next_unit(p, a, 0, 0) != -1 || sieda_add_part_unit(p, a, 1, 0, 0, 0) != -1) return 5;
    {
        char* snap = sieda_project_snapshot(p);
        if (!snap || !strstr(snap, "\"unitName\":\"P\"") || !strstr(snap, "\"unitPackage\":true")) return 6;
        sieda_string_free(snap);
        char* changed = sieda_annotate(p, "{\"packUnits\":true}");
        if (!changed) return 7;
        sieda_string_free(changed);
    }
    if (sieda_find_component(p, "U1") < 0) return 8;
    sieda_project_free(p);
    return 0;
}

/* Channel analysis through the C ABI: line loss, foil, channel JSON with an eye, Touchstone export / parse / eye. */
int sieda_c_api_channel_test(const char* project_json) {
    char* err = NULL;
    char* json = NULL;
    char* ts = NULL;
    SiedaProject* p = sieda_project_load_json(project_json, &err);
    if (!p) return 1;
    json = sieda_si_line_loss_json(p, "{\"roughness\":\"hammerstad\"}");
    if (!json || !strstr(json, "\"dbPerInch\"")) return 2;
    sieda_string_free(json);
    if (!sieda_si_set_copper_foil(p, "hvlp") || sieda_si_set_copper_foil(p, "gold")) return 3;
    if (!sieda_si_set_channel(p, "D_P", 5e9, 0.1, 0.3) || !sieda_si_set_channel(p, "D_P", 0, 0, 0)) return 4;
    json = sieda_si_channel_json(p, "{\"net\":\"D_P\",\"driver\":\"ideal\",\"eye\":{\"bitRate\":5e9}}");
    if (!json || !strstr(json, "\"SDD21\"") || !strstr(json, "\"eyeHeight\"")) return 5;
    sieda_string_free(json);
    json = sieda_si_channel_json(p, "{\"net\":\"NOPE\"}");
    if (!json || !strstr(json, "\"error\"")) return 6;
    sieda_string_free(json);
    json = sieda_si_channel_json(p, "not json");
    if (!json || !strstr(json, "\"error\"")) return 7;
    sieda_string_free(json);
    ts = sieda_si_channel_touchstone(p, "{\"net\":\"D_P\",\"points\":21}", &err);
    if (!ts || err) return 8;
    json = sieda_touchstone_parse(ts, 4, "13", &err);
    if (!json || err || !strstr(json, "\"ports\":4")) return 9;
    sieda_string_free(json);
    json = sieda_touchstone_channel_json(ts, 4, "{\"eye\":{\"bitRate\":2e9}}", &err);
    if (!json || err || !strstr(json, "\"eye\"")) return 10;
    sieda_string_free(json);
    sieda_string_free(ts);
    if (sieda_touchstone_parse("garbage", 2, "13", &err) != NULL || !err) return 11;
    sieda_string_free(err);
    err = NULL;
    if (sieda_si_channel_touchstone(p, "{\"net\":\"NOPE\"}", &err) != NULL || !err) return 12;
    sieda_string_free(err);
    sieda_project_free(p);
    return 0;
}

/* SPICE model import: parse, check, attach (stored in the project), read back, remove. */
int sieda_c_api_spice_test(void) {
    static const char* lib = "* vendor diode\n.model DTEST D(IS=1n N=1.8 RS=0.5\n+ CJO=4p TT=10n)\n.subckt NOPE a b\nR1 a b 1k\n";
    SiedaProject* p = sieda_project_new("SPICE models");
    if (!p) return 1;
    int32_t v = sieda_add_component(p, 5 /* VoltageSource */, "5", 0, 0, 0, NULL);
    int32_t r = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    int32_t d = sieda_add_component(p, 3 /* Diode */, "1N4148", 200, 0, 0, NULL);
    int32_t g = sieda_add_component(p, 7 /* Ground */, NULL, 0, 80, 0, NULL);
    if (v < 0 || r < 0 || d < 0 || g < 0) return 2;
    if (sieda_connect(p, v, 0, r, 0) < 0 || sieda_connect(p, r, 1, d, 0) < 0 || sieda_connect(p, d, 1, g, 0) < 0 ||
        sieda_connect(p, v, 1, g, 0) < 0)
        return 3;
    char* parsed = sieda_spice_parse(lib);
    if (!parsed || !strstr(parsed, "\"name\":\"DTEST\"") || !strstr(parsed, "\"ok\":false") || !strstr(parsed, "no .ends"))
        return 4;
    sieda_string_free(parsed);
    char* check = sieda_spice_check(p, d, lib, "DTEST", "");
    if (!check || !strstr(check, "\"ok\":true") || !strstr(check, "\"defaultPins\":\"A K\"")) return 5;
    sieda_string_free(check);
    char* error = NULL;
    if (sieda_set_spice_model(p, d, lib, "DTEST", "A Q", &error) != 0 || !error || !strstr(error, "no pin 'Q'")) return 6;
    sieda_string_free(error);
    error = NULL;
    if (sieda_set_spice_model(p, d, lib, "MISSING", "", &error) != 0 || !error) return 7;
    sieda_string_free(error);
    error = NULL;
    if (sieda_set_spice_model(p, d, lib, "DTEST", "", &error) != 1 || error) return 8;
    char* model = sieda_component_spice_model(p, d);
    /* Only the definition is stored, not the broken subckt next to it. */
    if (!model || !strstr(model, "\"model\":\"DTEST\"") || !strstr(model, "CJO=4p") || strstr(model, "NOPE")) return 9;
    sieda_string_free(model);
    char* dc = sieda_simulate_dc(p);
    if (!dc || !strstr(dc, "\"converged\":true")) return 10;
    sieda_string_free(dc);
    char* saved = sieda_project_save_json(p);
    if (!saved || !strstr(saved, "\"spice\"") || !strstr(saved, "CJO=4p")) return 11;
    SiedaProject* q = sieda_project_load_json(saved, NULL);
    sieda_string_free(saved);
    if (!q) return 12;
    model = sieda_component_spice_model(q, d);
    if (!model || !strstr(model, "\"model\":\"DTEST\"")) return 13;
    sieda_string_free(model);
    sieda_project_free(q);
    if (sieda_set_spice_model(p, d, "", "", "", NULL) != 1) return 14;
    model = sieda_component_spice_model(p, d);
    if (!model || !strstr(model, "\"model\":\"\"")) return 15;
    sieda_string_free(model);
    char* builtins = sieda_spice_builtin_models();
    if (!builtins || !strstr(builtins, "\"name\":\"UA741\"")) return 16;
    sieda_string_free(builtins);
    if (sieda_set_spice_model(NULL, d, lib, "DTEST", "", NULL) != 0) return 17;
    if (sieda_spice_check(NULL, d, lib, "DTEST", "") != NULL) return 18;
    char* garbage = sieda_spice_parse(NULL);
    if (!garbage) return 19;
    sieda_string_free(garbage);
    sieda_project_free(p);
    return 0;
}

/* Power-integrity planning through the C ABI. */
int sieda_c_api_pi_test(const char* project_json) {
    char* err = NULL;
    char* json = NULL;
    SiedaProject* p = sieda_project_load_json(project_json, &err);
    if (!p) return 1;
    if (!sieda_pi_set_vrm(p, "+3V3", 0.003, 150e3) || sieda_pi_set_vrm(p, "+3V3", -1, 0)) return 2;
    if (!sieda_pi_set_rail(p, "+3V3", 5, 0.2, 0)) return 3;
    json = sieda_si_settings_json(p);
    if (!json || !strstr(json, "\"vrmBandwidth\"")) return 4;
    sieda_string_free(json);
    json = sieda_pi_cavity_json(p, "+3V3");
    if (!json || !strstr(json, "\"zCavity\"")) return 5;
    sieda_string_free(json);
    json = sieda_pi_decap_plan_json(p, "+3V3");
    if (!json || !strstr(json, "\"additions\"")) return 6;
    sieda_string_free(json);
    json = sieda_pi_ir_map_json(p, "+3V3");
    if (!json || !strstr(json, "\"cells\"")) return 7;
    sieda_string_free(json);
    json = sieda_pi_ir_map_json(p, "NOPE");
    if (!json || !strstr(json, "\"error\"")) return 8;
    sieda_string_free(json);
    if (!sieda_pi_set_rail(p, "+3V3", 0, 0, 0)) return 9;
    json = sieda_si_settings_json(p);
    if (!json || !strstr(json, "\"vrmR\"")) return 10;
    sieda_string_free(json);
    if (!sieda_pi_set_vrm(p, "+3V3", 0, 0)) return 11;
    json = sieda_si_settings_json(p);
    if (!json || strstr(json, "+3V3")) return 12;
    sieda_string_free(json);
    sieda_project_free(p);
    return 0;
}

/* First integer after `key` in `json` (-1 when absent). */
static int32_t c_api_first_int_after(const char* json, const char* key) {
    const char* at = json ? strstr(json, key) : NULL;
    if (!at) return -1;
    at += strlen(key);
    while (*at && (*at < '0' || *at > '9')) ++at;
    if (!*at) return -1;
    int32_t v = 0;
    for (; *at >= '0' && *at <= '9'; ++at) v = v * 10 + (*at - '0');
    return v;
}

/* Via drag and interactive length tuning through the C API. */
int sieda_c_api_router_drag_tune_test(void) {
    SiedaProject* p = sieda_project_new("C API drag and tune");
    if (!p) return 1;
    int32_t r1 = sieda_add_component(p, 0 /* Resistor */, "1k", 0, 0, 0, NULL);
    int32_t r2 = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    if (r1 < 0 || r2 < 0 || sieda_connect(p, r1, 1, r2, 0) < 0) return 2;
    if (!sieda_pcb_move_footprint(p, r1, 10, 20) || !sieda_pcb_move_footprint(p, r2, 40, 20)) return 3;
    char* s = sieda_router_begin(p, "{\"mode\":\"shove\",\"posture\":\"45\"}", 10.95, 20, 0);
    sieda_string_free(s);
    s = sieda_router_move(p, 18, 20);
    sieda_string_free(s);
    s = sieda_router_add_via(p, -1);
    if (!s || strstr(s, "\"error\"") || !strstr(s, "\"kind\":\"through\"")) return 4;
    sieda_string_free(s);
    s = sieda_router_move(p, 30, 20);
    sieda_string_free(s);
    s = sieda_router_add_via(p, 0);
    sieda_string_free(s);
    s = sieda_router_move(p, 39.05, 20);
    if (!s || !strstr(s, "\"reachedTarget\":true") || !strstr(s, "\"netLength\":")) return 5;
    sieda_string_free(s);
    char* commit = sieda_router_commit(p);
    const int32_t via = c_api_first_int_after(commit, "\"addedVias\":[");
    const int32_t track = c_api_first_int_after(commit, "\"addedTracks\":[");
    sieda_string_free(commit);
    if (via < 0 || track < 0) return 6;

    char* drag = sieda_router_begin_via_drag(p, NULL, via, 18, 20);
    if (!drag || strstr(drag, "\"error\"") || !strstr(drag, "\"kind\":\"via\"")) return 7;
    sieda_string_free(drag);
    char* move = sieda_router_move(p, 18, 23);
    if (!move || !strstr(move, "\"vias\":[{") || !strstr(move, "\"hiddenVias\":[")) return 8;
    sieda_string_free(move);
    char* done = sieda_router_commit(p);
    if (!done || !strstr(done, "\"ok\":true") || !strstr(done, "\"removedVias\":[{")) return 9;
    sieda_string_free(done);
    char* missing = sieda_router_begin_via_drag(p, NULL, -3, 0, 0);
    if (!missing || !strstr(missing, "\"error\"")) return 10;
    sieda_string_free(missing);

    char* snap = sieda_project_snapshot(p);
    const int32_t first = c_api_first_int_after(snap ? strstr(snap, "\"tracks\":[{") : NULL, "\"id\":");
    sieda_string_free(snap);
    if (first < 0) return 11;
    char* preview = sieda_router_tune(p, first, "{\"target\":200,\"apply\":false,\"spacing\":0.5}");
    if (!preview || !strstr(preview, "\"ok\":true") || !strstr(preview, "\"applied\":false") ||
        !strstr(preview, "\"addedTracks\":[{"))
        return 12;
    sieda_string_free(preview);
    char* applied = sieda_router_tune(p, first, "{\"target\":200}");
    if (!applied || !strstr(applied, "\"applied\":true")) return 13;
    sieda_string_free(applied);
    char* bad = sieda_router_tune(p, first, "{not json");
    if (!bad || !strstr(bad, "\"error\"")) return 14;
    sieda_string_free(bad);
    /* A resistor's other pad has no net: no bus to start there. */
    char* bus = sieda_router_begin_bus(p, NULL, 10.95, 20, 0, 4);
    if (!bus || !strstr(bus, "\"error\"") || sieda_router_active(p)) return 15;
    sieda_string_free(bus);
    /* Both pads of R1 that matter are routed already: nothing to fan out. */
    char* fan = sieda_pcb_fanout(p, r1, "{\"viaType\":\"through\"}");
    if (!fan || !strstr(fan, "\"fanned\":0") || !strstr(fan, "\"ok\":false")) return 16;
    sieda_string_free(fan);
    sieda_project_free(p);
    return 0;
}

/* Noise analysis through the C API: JSON in, densities and contributions out; bad options reported, never thrown. */
int sieda_c_api_noise_test(void) {
    SiedaProject* p = sieda_project_new("noise");
    if (!p) return 1;
    int32_t v = sieda_add_component(p, 5 /* VoltageSource */, "0 AC 1", 0, 0, 0, NULL);
    int32_t r = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    int32_t c = sieda_add_component(p, 1 /* Capacitor */, "1n", 200, 0, 0, NULL);
    int32_t g = sieda_add_component(p, 7 /* Ground */, NULL, 0, 80, 0, NULL);
    if (sieda_connect(p, v, 0, r, 0) < 0 || sieda_connect(p, r, 1, c, 0) < 0 || sieda_connect(p, c, 1, g, 0) < 0 ||
        sieda_connect(p, v, 1, g, 0) < 0)
        return 2;
    char* bad = sieda_simulate_noise(p, "{\"output\":\"nope\"}");
    if (!bad || !strstr(bad, "\"ok\":false")) return 3;
    sieda_string_free(bad);
    char* junk = sieda_simulate_noise(p, "{oops");
    if (!junk || !strstr(junk, "\"ok\":false")) return 4;
    sieda_string_free(junk);
    if (sieda_simulate_noise(NULL, "{}") != NULL) return 5;
    /* Nets by index: one of the first nets is the RC node (numbering is the core's). */
    int found = 0;
    for (int net = 0; net < 4 && !found; ++net) {
        char options[160];
        snprintf(options, sizeof options, "{\"output\":%d,\"start\":10,\"stop\":\"1MEG\",\"pointsPerDecade\":5}", net);
        char* ok = sieda_simulate_noise(p, options);
        if (!ok) return 6;
        found = strstr(ok, "\"ok\":true") && strstr(ok, "\"outputDensity\":[") && strstr(ok, "\"kind\":\"thermal\"") &&
                strstr(ok, "\"inputSource\":\"V1\"");
        sieda_string_free(ok);
    }
    if (!found) return 7;
    sieda_project_free(p);
    return 0;
}

/* Transient with options: methods, adaptive steps, validation. */
int sieda_c_api_transient_ex_test(void) {
    SiedaProject* p = sieda_project_new("transient options");
    if (!p) return 1;
    int32_t v = sieda_add_component(p, 5 /* VoltageSource */, "PULSE(0 5 1m)", 0, 0, 0, NULL);
    int32_t r = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    int32_t c = sieda_add_component(p, 1 /* Capacitor */, "100n", 200, 0, 0, NULL);
    int32_t g = sieda_add_component(p, 7 /* Ground */, NULL, 0, 80, 0, NULL);
    if (sieda_connect(p, v, 0, r, 0) < 0 || sieda_connect(p, r, 1, c, 0) < 0 || sieda_connect(p, c, 1, g, 0) < 0 ||
        sieda_connect(p, v, 1, g, 0) < 0)
        return 2;
    char* a = sieda_simulate_transient_ex(p, "{\"stop\":\"2m\",\"step\":\"10u\",\"method\":\"trap\",\"adaptive\":true}");
    if (!a || !strstr(a, "\"ok\":true") || !strstr(a, "\"time\":[")) return 3;
    sieda_string_free(a);
    char* b = sieda_simulate_transient_ex(p, "{\"stop\":\"2m\",\"step\":\"10u\",\"method\":\"gear\"}");
    if (!b || !strstr(b, "\"ok\":false") || !strstr(b, "gear")) return 4;
    sieda_string_free(b);
    char* d = sieda_simulate_transient_ex(p, "{bad");
    if (!d || !strstr(d, "\"ok\":false")) return 5;
    sieda_string_free(d);
    char* e = sieda_simulate_transient_ex(p, NULL);
    if (!e || !strstr(e, "\"ok\":true")) return 6;
    sieda_string_free(e);
    if (sieda_simulate_transient_ex(NULL, "{}") != NULL) return 7;
    sieda_project_free(p);
    return 0;
}

/* Supplier data through the C API: parse, merge, roll-up, catalog match and hostile input. */
int sieda_c_api_supplier_test(void) {
    const char* body =
        "{\"SearchResults\":{\"NumberOfResult\":1,\"Parts\":[{\"ManufacturerPartNumber\":\"NE555DR\",\"Manufacturer\":"
        "\"Texas Instruments\",\"MouserPartNumber\":\"595-NE555DR\",\"AvailabilityInStock\":\"100\",\"Min\":\"1\","
        "\"Mult\":\"1\",\"PriceBreaks\":[{\"Quantity\":1,\"Price\":\"$0.30\",\"Currency\":\"USD\"}]}]}}";
    char* parsed = sieda_supplier_parse("mouser", body, "USD");
    if (!parsed || !strstr(parsed, "\"mpn\":\"NE555DR\"") || !strstr(parsed, "\"schema\":\"sieda.supplier/1\"")) return 1;
    char request[8192];
    snprintf(request, sizeof request, "{\"currency\":\"USD\",\"results\":[%s]}", parsed);
    sieda_string_free(parsed);
    char* merged = sieda_supplier_merge(request);
    if (!merged || !strstr(merged, "\"595-NE555DR\"")) return 2;
    sieda_string_free(merged);
    char* rollup = sieda_supplier_bom_rollup(
        "{\"quantities\":[1],\"lines\":[{\"item\":1,\"refs\":[\"U1\"],\"quantity\":1,\"mpn\":\"NE555DR\"}],"
        "\"parts\":[{\"mpn\":\"NE555DR\",\"offers\":[{\"supplier\":\"Mouser\",\"currency\":\"USD\",\"stock\":5,"
        "\"prices\":[{\"quantity\":1,\"price\":0.3}]}]}]}");
    if (!rollup || !strstr(rollup, "\"status\":\"priced\"")) return 3;
    sieda_string_free(rollup);
    char* match = sieda_supplier_catalog_match("NE555");
    if (!match || !strstr(match, "\"match\":\"NE555\"")) return 4;
    sieda_string_free(match);
    char* bad = sieda_supplier_parse(NULL, NULL, NULL);
    if (!bad || !strstr(bad, "\"errorKind\":\"source\"")) return 5;
    sieda_string_free(bad);
    bad = sieda_supplier_merge("not json");
    if (!bad || !strstr(bad, "\"parts\":[]")) return 6;
    sieda_string_free(bad);
    bad = sieda_supplier_bom_rollup(NULL);
    if (!bad || !strstr(bad, "\"error\"")) return 7;
    sieda_string_free(bad);
    bad = sieda_supplier_parse("nexar", "[[[[[[[[", "");
    if (!bad || !strstr(bad, "\"errorKind\":\"parse\"")) return 8;
    sieda_string_free(bad);
    return 0;
}

/* 3D models through the C API: import (text and base64), fit, preview mesh and bad requests. */
int sieda_c_api_model3d_test(void) {
    const char* cube =
        "{\"name\":\"cube.obj\",\"content\":\"v 0 0 0\\nv 2 0 0\\nv 2 2 0\\nv 0 2 0\\nv 0 0 1\\nv 2 0 1\\nv 2 2 1\\nv 0 2 1\\n"
        "f 1 4 3 2\\nf 5 6 7 8\\nf 1 2 6 5\\nf 2 3 7 6\\nf 3 4 8 7\\nf 4 1 5 8\\n\"}";
    char* imported = sieda_model3d_import(cube);
    if (!imported || !strstr(imported, "\"ok\":true") || !strstr(imported, "\"triangles\":12")) return 1;
    const char* at = strstr(imported, "\"id\":\"");
    if (!at) return 2;
    char id[32] = {0};
    memcpy(id, at + 6, 17);
    sieda_string_free(imported);
    char spec[1024];
    snprintf(spec, sizeof spec,
             "{\"name\":\"CUBEPART\",\"package\":{\"type\":\"SOIC\",\"pinCount\":8},\"pins\":[{\"number\":\"1\",\"name\":\"A\"}],"
             "\"model3d\":{\"id\":\"%s\",\"name\":\"cube.obj\",\"unit\":1,\"offset\":[5,5,5]}}", id);
    char* fit = sieda_model3d_fit(spec);
    if (!fit || !strstr(fit, "\"ok\":true") || !strstr(fit, "\"offset\":[-1,-1,0]")) return 3;
    sieda_string_free(fit);
    SiedaMesh* preview = sieda_model3d_preview(spec);
    if (!preview || sieda_mesh_vertex_count(preview) <= 0) return 4;
    sieda_mesh_free(preview);
    char* bad = sieda_model3d_import("{\"name\":\"x.step\",\"content\":\"ISO-10303-21;\"}");
    if (!bad || !strstr(bad, "\"ok\":false") || !strstr(bad, "STEP")) return 5;
    sieda_string_free(bad);
    bad = sieda_model3d_import("{\"name\":\"x.stl\",\"contentBase64\":\"!!!\"}");
    if (!bad || !strstr(bad, "\"ok\":false")) return 6;
    sieda_string_free(bad);
    bad = sieda_model3d_import(NULL);
    if (!bad || !strstr(bad, "\"ok\":false")) return 7;
    sieda_string_free(bad);
    bad = sieda_model3d_fit("{\"name\":\"X\",\"pins\":[{\"number\":\"1\"}]}");
    if (!bad || !strstr(bad, "\"ok\":false")) return 8;
    sieda_string_free(bad);
    if (sieda_model3d_preview("not json") != NULL) return 9;
    return 0;
}
/* Autoroute progress and cancel, router strategy and threads. Returns 0 or the failing step. */
typedef struct {
    int calls;
    int cancelAt;   /* cancel on this call (1-based), 0 = never */
    int lastPhase;
    int phaseBackwards;
} C_ApiRouteProgress;

static int32_t c_api_route_progress(void* user, int32_t phase, int32_t pass, int32_t done, int32_t total,
                                    int32_t unrouted) {
    C_ApiRouteProgress* p = (C_ApiRouteProgress*)user;
    (void)pass;
    (void)unrouted;
    ++p->calls;
    if (phase < p->lastPhase && phase != 2) p->phaseBackwards = 1; /* rip-up passes may follow routing */
    if (done < 0 || total < 0 || done > total + 1) p->phaseBackwards = 1;
    p->lastPhase = phase;
    return p->cancelAt > 0 && p->calls >= p->cancelAt ? 1 : 0;
}

int sieda_c_api_autoroute_progress_test(void) {
    SiedaProject* p = sieda_project_new("C API autoroute progress");
    if (!p) return 1;
    int32_t r[4];
    for (int k = 0; k < 4; ++k) {
        r[k] = sieda_add_component(p, 0 /* Resistor */, "1k", 40.0 * k, 0, 0, NULL);
        if (r[k] < 0) return 2;
        if (!sieda_pcb_move_footprint(p, r[k], 10 + 8.0 * k, 10 + 3.0 * k)) return 3;
    }
    for (int k = 0; k + 1 < 4; ++k)
        if (sieda_connect(p, r[k], 1, r[k + 1], 0) < 0) return 4;
    sieda_pcb_set_board(p, 50, 30, 0, 0);
    char* before = sieda_project_snapshot(p);
    if (!before) return 5;

    /* Cancel at the first report: nothing changes. */
    C_ApiRouteProgress cancel = {0, 1, 0, 0};
    char* stopped = sieda_pcb_autoroute_progress(p, c_api_route_progress, &cancel);
    if (!stopped || !strstr(stopped, "\"cancelled\":true") || cancel.calls != 1) return 6;
    sieda_string_free(stopped);
    char* after = sieda_project_snapshot(p);
    if (!after || strcmp(before, after) != 0) return 7;
    sieda_string_free(after);

    /* A full route reports progress and routes everything. */
    C_ApiRouteProgress watch = {0, 0, 0, 0};
    char* routed = sieda_pcb_autoroute_progress(p, c_api_route_progress, &watch);
    if (!routed || strstr(routed, "\"cancelled\"") || !strstr(routed, "\"failed\":0") || watch.calls < 3 ||
        watch.phaseBackwards || watch.lastPhase != 3)
        return 8;
    sieda_string_free(routed);

    /* The corridor router (forced) on the same board, single-threaded and on four threads: the same result (the core
     * tests compare the copper bit for bit). */
    if (!sieda_router_set_strategy(0) || sieda_router_set_strategy(7) || sieda_router_strategy() != 0) return 9;
    if (!sieda_router_set_strategy(2) || sieda_router_strategy() != 2) return 10;
    char* snaps[2] = {NULL, NULL};
    for (int t = 0; t < 2; ++t) {
        sieda_router_set_threads(t == 0 ? 1 : 4);
        sieda_pcb_clear_routing(p);
        snaps[t] = sieda_pcb_autoroute_progress(p, NULL, NULL);
        if (!snaps[t] || !strstr(snaps[t], "\"failed\":0")) return 11;
    }
    sieda_router_set_strategy(0);
    sieda_router_set_threads(0);
    if (!snaps[0] || !snaps[1] || strcmp(snaps[0], snaps[1]) != 0) return 12;
    sieda_string_free(snaps[0]);
    sieda_string_free(snaps[1]);
    sieda_string_free(before);
    sieda_project_free(p);
    return 0;
}

/* Schematic capture package: nested repeated sheets and channel parameters. Returns 0 or the failing step. */
int sieda_c_api_schematic_pro_test(void) {
    SiedaProject* p = sieda_project_new("Schematic pro");
    if (!p) return 1;
    int32_t amp = sieda_add_sheet(p, "Amp", 1);
    int32_t stage = sieda_add_sheet(p, "Stage", amp);
    if (amp < 0 || stage < 0 || !sieda_set_active_sheet(p, stage)) return 2;
    int32_t in = sieda_add_component(p, 15, "IN", 0, 0, 0, NULL);
    int32_t r = sieda_add_component(p, 0, "10k", 80, 0, 0, NULL);
    if (!sieda_set_label_scope(p, in, "port", 0) || sieda_connect(p, in, 0, r, 0) < 0) return 3;
    if (sieda_repeat_sheet(p, amp, 2) != -1) return 4; /* Stage is not repeated yet */
    if (sieda_repeat_sheet(p, stage, 2) != 2 || sieda_repeat_sheet(p, amp, 3) != 3) return 5;
    {
        char* sheets = sieda_sheets_json(p);
        if (!sheets || !strstr(sheets, "\"name\":\"Stage [C/B]\"") || !strstr(sheets, "\"channels\":3") ||
            !strstr(sheets, "\"path\":\"B/A\""))
            return 6;
        sieda_string_free(sheets);
    }
    if (!sieda_set_instance_refs(p, amp, "suffix") || sieda_find_component(p, "R1_C_B") < 0) return 7;
    int32_t rcb = sieda_find_component(p, "R1_C_B");
    if (!sieda_set_channel_value(p, rcb, "12k") || sieda_set_channel_value(NULL, rcb, "1k")) return 8;
    {
        char* snap = sieda_project_snapshot(p);
        if (!snap || !strstr(snap, "\"blockValue\":\"10k\"") || !strstr(snap, "\"value\":\"12k\"")) return 9;
        sieda_string_free(snap);
    }
    if (!sieda_set_channel_value(p, rcb, "") || !sieda_clear_channel_overrides(p, rcb)) return 10;
    if (!sieda_set_channel_package(p, rcb, "") || sieda_set_channel_package(p, rcb, "NOT_A_PACKAGE")) return 11;
    sieda_project_free(p);
    return 0;
}

/* Unit (gate) editor checks, gate swap and pin swap through the C API. Returns 0 or the failing step. */
int sieda_c_api_unit_editor_test(void) {
    const char* spec =
        "{\"name\":\"C-API-NAND\",\"package\":{\"type\":\"SOIC\",\"pinCount\":14},\"pins\":["
        "{\"number\":\"1\",\"name\":\"1A\",\"type\":\"input\"},{\"number\":\"2\",\"name\":\"1B\",\"type\":\"input\"},"
        "{\"number\":\"3\",\"name\":\"1Y\",\"type\":\"output\"},{\"number\":\"4\",\"name\":\"2A\",\"type\":\"input\"},"
        "{\"number\":\"5\",\"name\":\"2B\",\"type\":\"input\"},{\"number\":\"6\",\"name\":\"2Y\",\"type\":\"output\"},"
        "{\"number\":\"7\",\"name\":\"GND\",\"type\":\"power_in\"},{\"number\":\"8\",\"name\":\"3Y\",\"type\":\"output\"},"
        "{\"number\":\"9\",\"name\":\"3A\",\"type\":\"input\"},{\"number\":\"10\",\"name\":\"3B\",\"type\":\"input\"},"
        "{\"number\":\"11\",\"name\":\"4Y\",\"type\":\"output\"},{\"number\":\"12\",\"name\":\"4A\",\"type\":\"input\"},"
        "{\"number\":\"13\",\"name\":\"4B\",\"type\":\"input\"},{\"number\":\"14\",\"name\":\"VCC\",\"type\":\"power_in\"}],"
        "\"units\":[{\"name\":\"A\",\"pins\":[\"1\",\"2\",\"3\"],\"pinSwap\":[[\"1\",\"2\"]]},"
        "{\"name\":\"B\",\"pins\":[\"4\",\"5\",\"6\"],\"pinSwap\":[[\"4\",\"5\"]]},"
        "{\"name\":\"C\",\"pins\":[\"9\",\"10\",\"8\"],\"swap\":-1},{\"name\":\"D\",\"pins\":[\"12\",\"13\",\"11\"]}]}";
    char* issues = sieda_check_units(spec);
    if (!issues || !strstr(issues, "UNIT_POWER") || strstr(issues, "\"error\"")) return 1;
    sieda_string_free(issues);
    issues = sieda_check_units("{\"name\":\"X\",\"pins\":[{\"number\":\"1\",\"name\":\"A\"}],\"units\":[{\"name\":\"A\",\"pins\":[]}]}");
    if (!issues || !strstr(issues, "UNIT_INVALID")) return 2;
    sieda_string_free(issues);
    SiedaProject* p = sieda_project_new("Units");
    if (!p) return 3;
    char* err = NULL;
    char* part = sieda_custom_part_register(p, spec, &err);
    if (!part || err) return 4;
    const char* idStart = strstr(part, "\"id\":\"");
    if (!idStart) return 5;
    char id[128] = {0};
    {
        const char* s = idStart + 6;
        size_t n = 0;
        while (s[n] && s[n] != '"' && n + 1 < sizeof id) {
            id[n] = s[n];
            ++n;
        }
    }
    sieda_string_free(part);
    int32_t a = sieda_add_custom_units(p, id, NULL, 0, 0, 0, NULL);
    int32_t b = sieda_place_next_unit(p, a, 200, 0);
    int32_t c = sieda_place_next_unit(p, a, 400, 0);
    if (a < 0 || b < 0 || c < 0) return 6;
    if (!sieda_swap_pins(p, a, 0, 1) || sieda_swap_pins(p, a, 0, 2)) return 7;
    if (!sieda_swap_units(p, a, b) || sieda_swap_units(p, a, c) || sieda_swap_units(NULL, a, b)) return 8;
    sieda_project_free(p);
    return 0;
}

/* Signal harnesses and bus entries through the C API. Returns 0 or the failing step. */
int sieda_c_api_harness_test(void) {
    SiedaProject* p = sieda_project_new("Harness");
    if (!p) return 1;
    if (!sieda_set_harness_type(p, "I2C", "[\"SCL\",\"SDA\"]") || sieda_set_harness_type(p, "bad name", "[\"A\"]")) return 2;
    char* types = sieda_harness_types_json(p);
    if (!types || !strstr(types, "\"SDA\"")) return 3;
    sieda_string_free(types);
    int32_t h = sieda_add_harness_connector(p, "I2C", "BUS1", 100, 0);
    if (h < 0 || sieda_add_harness_connector(p, "NOPE", "X", 0, 0) != -1) return 4;
    if (sieda_place_harness_entries(p, h) != 0) return 5;
    {
        char* snap = sieda_project_snapshot(p);
        if (!snap || !strstr(snap, "\"harnessOf\":") || !strstr(snap, "\"harnessType\":\"I2C\"")) return 6;
        sieda_string_free(snap);
    }
    if (!sieda_set_label_harness(p, h, "") || !sieda_set_label_harness(p, h, "I2C")) return 7;
    if (sieda_place_harness_entries(p, h) != 2) return 8;
    if (!sieda_set_harness_type(p, "I2C", NULL) || sieda_set_harness_type(p, "I2C", "[]")) return 9;
    /* Bus entries by label. */
    int32_t bus = sieda_add_bus(p, "D[0..1]", "[{\"x\":0,\"y\":0},{\"x\":0,\"y\":100}]");
    int32_t label = sieda_add_component(p, 15, "D0", 20, 20, 0, NULL);
    if (bus <= 0 || !sieda_set_label_bus(p, label, bus) || sieda_set_label_bus(p, label, 999) ||
        sieda_set_label_bus(NULL, label, bus))
        return 10;
    sieda_project_free(p);
    return 0;
}

/* True arcs through the C API: a route with arc corners writes arc tracks (the snapshot carries their centre and
 * angles); "convert corners to arcs" on a sharp route; bad input is reported, never thrown. */
int sieda_c_api_arc_test(void) {
    const char* ids = "[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]";
    for (int pass = 0; pass < 2; ++pass) {
        SiedaProject* p = sieda_project_new("C API arcs");
        if (!p) return 1;
        int32_t r1 = sieda_add_component(p, 0 /* Resistor */, "1k", 0, 0, 0, NULL);
        int32_t r2 = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
        if (r1 < 0 || r2 < 0 || sieda_connect(p, r1, 1, r2, 0) < 0) return 2;
        if (!sieda_pcb_move_footprint(p, r1, 10, 20) || !sieda_pcb_move_footprint(p, r2, 34, 28)) return 3;
        const char* opts = pass == 0 ? "{\"cornerRadius\":1.5,\"arcCorners\":true}" : "{\"cornerRadius\":0}";
        char* s = sieda_router_begin(p, opts, 10.95, 20, 0);
        if (!s || strstr(s, "\"error\"")) return 4;
        sieda_string_free(s);
        s = sieda_router_move(p, 18, 20);
        sieda_string_free(s);
        s = sieda_router_fix(p);
        if (!s || strstr(s, "\"error\"")) return 5;
        sieda_string_free(s);
        s = sieda_router_move(p, 33.05, 28);
        if (!s || !strstr(s, "\"reachedTarget\":true")) return 6;
        sieda_string_free(s);
        s = sieda_router_commit(p);
        if (!s || !strstr(s, "\"ok\":true")) return 7;
        sieda_string_free(s);
        if (pass == 1) {
            char* pre = sieda_pcb_arc_corners(p, ids, "{\"radius\":1.5,\"apply\":false}");
            if (!pre || !strstr(pre, "\"ok\":true") || !strstr(pre, "\"applied\":false")) return 8;
            sieda_string_free(pre);
            char* done = sieda_pcb_arc_corners(p, ids, NULL);
            if (!done || !strstr(done, "\"ok\":true") || !strstr(done, "\"applied\":true")) return 9;
            sieda_string_free(done);
        }
        char* snap = sieda_project_snapshot(p);
        if (!snap || !strstr(snap, "\"arc\":true") || !strstr(snap, "\"radius\":") || !strstr(snap, "\"sweep\":")) return 10;
        sieda_string_free(snap);
        char* bad = sieda_pcb_arc_corners(p, "{not json", NULL);
        if (!bad || !strstr(bad, "\"error\"")) return 11;
        sieda_string_free(bad);
        if (sieda_pcb_arc_corners(NULL, ids, NULL) != NULL) return 12;
        sieda_project_free(p);
    }
    return 0;
}

/* Length rules and the new tuning options through the C API: a rule on a labelled net, the targets JSON, a
 * trombone with round corners tuned to the rule, match groups, bad input. */
int sieda_c_api_length_test(void) {
    SiedaProject* p = sieda_project_new("C API length");
    if (!p) return 1;
    int32_t r1 = sieda_add_component(p, 0 /* Resistor */, "1k", 0, 0, 0, NULL);
    int32_t r2 = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    int32_t label = sieda_add_component(p, 15 /* NetLabel */, "SIG", 50, -40, 0, NULL);
    if (r1 < 0 || r2 < 0 || label < 0) return 2;
    if (sieda_connect(p, r1, 1, r2, 0) < 0 || sieda_connect(p, label, 0, r1, 1) < 0) return 3;
    if (!sieda_pcb_move_footprint(p, r1, 10, 20) || !sieda_pcb_move_footprint(p, r2, 40, 20)) return 4;
    char* s = sieda_router_begin(p, NULL, 10.95, 20, 0);
    if (!s || strstr(s, "\"error\"")) return 5;
    sieda_string_free(s);
    s = sieda_router_move(p, 39.05, 20);
    sieda_string_free(s);
    s = sieda_router_commit(p);
    if (!s || !strstr(s, "\"ok\":true")) return 6;
    sieda_string_free(s);
    char* snap = sieda_project_snapshot(p);
    const char* tracksAt = snap ? strstr(snap, "\"tracks\":[{") : NULL;
    const char* idAt = tracksAt ? strstr(tracksAt, "\"id\":") : NULL;
    if (!idAt) return 7;
    int32_t track = 0;
    for (const char* c = idAt + 5; *c >= '0' && *c <= '9'; ++c) track = track * 10 + (*c - '0');
    sieda_string_free(snap);

    if (sieda_pcb_set_length_rule(p, "SIG", 35, 0.1) != 1) return 8;
    char* targets = sieda_length_targets_json(p);
    if (!targets || !strstr(targets, "\"net\":\"SIG\"") || !strstr(targets, "\"ok\":false")) return 9;
    sieda_string_free(targets);
    char* tune = sieda_router_tune(p, track, "{\"style\":\"trombone\",\"corner\":\"round\",\"maxAmplitude\":4,\"apply\":true}");
    if (!tune || !strstr(tune, "\"ok\":true") || !strstr(tune, "\"targetSource\":\"rule:SIG\"")) return 10;
    sieda_string_free(tune);
    targets = sieda_length_targets_json(p);
    if (!targets || !strstr(targets, "\"ok\":true")) return 11;
    sieda_string_free(targets);
    char* drc = sieda_pcb_run_drc(p);
    if (!drc || strstr(drc, "DRC_LENGTH")) return 12;
    sieda_string_free(drc);

    if (sieda_pcb_set_match_group(p, "{\"name\":\"G\",\"nets\":[\"SIG\",\"OTHER\"],\"tolerance\":0.2}") != 1) return 13;
    targets = sieda_length_targets_json(p);
    if (!targets || !strstr(targets, "\"name\":\"G\"")) return 14;
    sieda_string_free(targets);
    if (sieda_pcb_set_match_group(p, "{\"name\":\"G\",\"nets\":[]}") != 1) return 15;  /* removed */
    if (sieda_pcb_set_match_group(p, "{not json") != 0 || sieda_pcb_set_match_group(p, "{\"nets\":[]}") != 0) return 16;
    if (sieda_pcb_set_length_rule(p, "SIG", 0, 0) != 1) return 17;  /* removed */
    targets = sieda_length_targets_json(p);
    if (!targets || strcmp(targets, "{\"groups\":[],\"rules\":[]}") != 0) return 18;
    sieda_string_free(targets);
    if (sieda_pcb_set_length_rule(NULL, "SIG", 1, 1) != 0 || sieda_length_targets_json(NULL) != NULL) return 19;
    sieda_project_free(p);
    return 0;
}

/* Schematic directives and net classes through the C API. Returns 0 or the failing step. */
int sieda_c_api_directive_test(void) {
    SiedaProject* p = sieda_project_new("Directives");
    if (!p) return 1;
    int32_t j = sieda_add_component(p, 12, "USB", 0, 0, 0, NULL);
    int32_t lp = sieda_add_component(p, 15, "D_P", 100, 0, 0, NULL);
    int32_t ln = sieda_add_component(p, 15, "D_N", 100, 40, 0, NULL);
    if (j < 0 || lp < 0 || ln < 0) return 2;
    if (sieda_connect(p, j, 0, lp, 0) < 0 || sieda_connect(p, j, 1, ln, 0) < 0) return 3;
    if (!sieda_set_net_class(p, "{\"name\":\"HS\",\"trackWidth\":0.2,\"clearance\":0.3}") ||
        sieda_set_net_class(p, "{\"name\":\"bad name\"}"))
        return 4;
    char json[160];
    snprintf(json, sizeof json, "{\"component\":%d,\"pin\":0,\"netClass\":\"HS\",\"diffPair\":true}", (int)lp);
    int32_t id = sieda_add_directive(p, json);
    if (id <= 0 || sieda_add_directive(p, "{\"component\":-5}") != -1) return 5;
    char* rules = sieda_net_rules_json(p);
    if (!rules || !strstr(rules, "\"netName\":\"D_P\"") || !strstr(rules, "\"netName\":\"D_N\"") || !strstr(rules, "\"diffPair\":true"))
        return 6;
    sieda_string_free(rules);
    char* snap = sieda_project_snapshot(p);
    if (!snap || !strstr(snap, "\"netClassDefs\"") || !strstr(snap, "\"netClearances\"")) return 7;
    sieda_string_free(snap);
    snprintf(json, sizeof json, "{\"component\":%d,\"pin\":0,\"trackWidth\":0.5}", (int)lp);
    if (!sieda_update_directive(p, id, json) || sieda_update_directive(p, 999, json)) return 8;
    if (!sieda_remove_directive(p, id) || sieda_remove_directive(p, id)) return 9;
    if (!sieda_remove_net_class(p, "HS") || sieda_remove_net_class(p, "HS")) return 10;
    sieda_project_free(p);
    return 0;
}

/* Corner drag, multi-track drag and multi-route through the C API. */
int sieda_c_api_drag_multi_test(void) {
    SiedaProject* p = sieda_project_new("C API drags");
    if (!p) return 1;
    int32_t r1 = sieda_add_component(p, 0 /* Resistor */, "1k", 0, 0, 0, NULL);
    int32_t r2 = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    int32_t r3 = sieda_add_component(p, 0 /* Resistor */, "1k", 0, 100, 0, NULL);
    int32_t r4 = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 100, 0, NULL);
    if (r1 < 0 || r2 < 0 || r3 < 0 || r4 < 0) return 2;
    if (sieda_connect(p, r1, 1, r2, 0) < 0 || sieda_connect(p, r3, 1, r4, 0) < 0) return 3;
    if (!sieda_pcb_move_footprint(p, r1, 10, 10) || !sieda_pcb_move_footprint(p, r2, 30, 20) ||
        !sieda_pcb_move_footprint(p, r3, 10, 14) || !sieda_pcb_move_footprint(p, r4, 30, 24))
        return 4;
    /* Route R1 → R2 with a corner, then drag the corner. */
    char* s = sieda_router_begin(p, "{\"posture\":\"45\"}", 10.95, 10, 0);
    if (!s || strstr(s, "\"error\"")) return 5;
    sieda_string_free(s);
    s = sieda_router_move(p, 29.05, 20);
    if (!s || !strstr(s, "\"reachedTarget\":true")) return 6;
    sieda_string_free(s);
    s = sieda_router_commit(p);
    if (!s || !strstr(s, "\"ok\":true")) return 7;
    sieda_string_free(s);
    char* snap = sieda_project_snapshot(p);
    const char* tracksAt = snap ? strstr(snap, "\"tracks\":[{") : NULL;
    const char* idAt = tracksAt ? strstr(tracksAt, "\"id\":") : NULL;
    if (!idAt) return 8;
    int32_t track = 0;
    for (const char* c = idAt + 5; *c >= '0' && *c <= '9'; ++c) track = track * 10 + (*c - '0');
    sieda_string_free(snap);
    char* corner = sieda_router_begin_corner_drag(p, "{\"posture\":\"free\"}", track, 19, 10);
    if (!corner) return 9;
    if (!strstr(corner, "\"error\"")) { /* the track's far end is a corner: drag it a little and drop */
        sieda_string_free(corner);
        s = sieda_router_move(p, 19, 11);
        if (!s || !strstr(s, "\"kind\":\"corner\"")) return 10;
        sieda_string_free(s);
        s = sieda_router_commit(p);
        if (!s || !strstr(s, "\"ok\":true")) return 11;
        sieda_string_free(s);
    } else {
        sieda_string_free(corner);
        sieda_router_cancel(p);
    }
    /* A multi drag of nothing is refused; bad JSON is reported. */
    char* multi = sieda_router_begin_multi_drag(p, NULL, "[]", 0, 0);
    if (!multi || !strstr(multi, "\"error\"")) return 12;
    sieda_string_free(multi);
    char* bad = sieda_router_begin_multi_drag(p, NULL, "{nope", 0, 0);
    if (!bad || !strstr(bad, "\"error\"")) return 13;
    sieda_string_free(bad);
    /* Multi-route the two nets from their start pads together. */
    char* m = sieda_router_begin_multi(p, NULL, "[{\"x\":10.95,\"y\":10},{\"x\":10.95,\"y\":14}]", 0);
    if (!m || strstr(m, "\"error\"") || !strstr(m, "\"kind\":\"multi\"")) return 14;
    sieda_string_free(m);
    sieda_router_cancel(p);
    if (sieda_router_begin_multi(NULL, NULL, "[]", 0) != NULL) return 15;
    sieda_project_free(p);
    return 0;
}

/* Editing productivity, back-annotation, templates and PDF through the C API. Returns 0 or the failing step. */
int sieda_c_api_schematic_tools_test(void) {
    SiedaProject* p = sieda_project_new("Tools");
    if (!p) return 1;
    int32_t a = sieda_add_component(p, 0, "1k", 0, 0, 0, NULL);
    int32_t b = sieda_add_component(p, 0, "2k", 130, 40, 0, NULL);
    int32_t l = sieda_add_component(p, 15, "D0", -60, 0, 0, NULL);
    if (a < 0 || b < 0 || l < 0 || sieda_connect(p, l, 0, a, 0) < 0) return 2;
    char ids[64];
    snprintf(ids, sizeof ids, "[%d,%d]", (int)a, (int)b);
    if (sieda_align_components(p, ids, "top") != 1 || sieda_align_components(p, ids, "sideways") != -1) return 3;
    snprintf(ids, sizeof ids, "[%d,%d]", (int)l, (int)a);
    char* clip = sieda_copy_components(p, ids);
    if (!clip || !strstr(clip, "sieda.schematic-clip/1")) return 4;
    char* made = sieda_paste_components(p, clip, "{\"dy\":60,\"count\":2,\"stepY\":60,\"labelIncrement\":1}");
    if (!made || made[0] != '[') return 5;
    sieda_string_free(made);
    sieda_string_free(clip);
    if (sieda_paste_components(p, "{\"format\":\"x\"}", NULL) == NULL) return 6; /* nothing pasted: an empty list */
    if (!sieda_swap_pin_connections(p, a, 0, 1) || sieda_swap_pin_connections(p, a, 0, 9)) return 7;
    char* eco = sieda_eco_from_was_is(p, "R1 R7\nR42 R43\n");
    if (!eco || !strstr(eco, "\"applicable\":true") || !strstr(eco, "No part R42")) return 8;
    if (sieda_apply_eco(p, eco) != 1 || sieda_find_component(p, "R7") != a) return 9;
    sieda_string_free(eco);
    char* board = sieda_reannotate_from_board(p, 0);
    if (!board || board[0] != '[') return 10;
    sieda_string_free(board);
    char* templates = sieda_sheet_templates_json();
    if (!templates || !strstr(templates, "\"ANSI D\"")) return 11;
    sieda_string_free(templates);
    if (!sieda_set_sheet_size(p, 1, "A3") || sieda_set_sheet_size(p, 1, "Z9")) return 12;
    char* pdf = sieda_export_schematic_pdf(p);
    if (!pdf || strncmp(pdf, "%PDF-1.4", 8) != 0 || !strstr(pdf, "(A3)")) return 13;
    sieda_string_free(pdf);
    sieda_project_free(p);
    return 0;
}

/* ERC error reporting through the C API. Returns 0 or the failing step. */
int sieda_c_api_erc_severity_test(void) {
    SiedaProject* p = sieda_project_new("ERC levels");
    if (!p) return 1;
    if (sieda_add_component(p, 0, "1k", 0, 0, 0, NULL) < 0) return 2;
    if (sieda_set_erc_severity(p, "ERC_FLOATING_COMPONENT", "off") != 1) return 3;
    char* erc = sieda_run_erc(p);
    if (!erc || strstr(erc, "ERC_FLOATING_COMPONENT")) return 4;
    sieda_string_free(erc);
    if (sieda_set_erc_severity(p, "ERC_FLOATING_COMPONENT", "loud") != 0) return 5;
    if (sieda_set_erc_severity(p, "ERC_FLOATING_COMPONENT", "default") != 1) return 6;
    if (sieda_set_erc_severity(p, "ERC_FLOATING_COMPONENT", "default") != 0) return 7;
    if (sieda_set_erc_severity(NULL, "X", "off") != 0) return 8;
    sieda_project_free(p);
    return 0;
}

/* A label made a harness entry through the C API. Returns 0 or the failing step. */
int sieda_c_api_harness_entry_test(void) {
    SiedaProject* p = sieda_project_new("Harness entry");
    if (!p) return 1;
    if (!sieda_set_harness_type(p, "PAIR", "[\"P\",\"N\"]")) return 2;
    int32_t h = sieda_add_component(p, 15, "LINK", 0, 0, 0, NULL);
    int32_t e = sieda_add_component(p, 15, "P", 0, 40, 0, NULL);
    if (h < 0 || e < 0 || !sieda_set_label_harness(p, h, "PAIR")) return 3;
    if (!sieda_set_harness_entry(p, e, h) || sieda_set_harness_entry(p, h, e)) return 4;
    char* snap = sieda_project_snapshot(p);
    if (!snap || !strstr(snap, "\"harnessOf\":")) return 5;
    sieda_string_free(snap);
    if (!sieda_set_harness_entry(p, e, 0)) return 6;
    sieda_project_free(p);
    return 0;
}

int sieda_c_api_board_commands_test(void) {
    SiedaProject* p = sieda_project_new("C API board commands");
    if (!p) return 1;
    int32_t r1 = sieda_add_component(p, 0 /* Resistor */, "1k", 0, 0, 0, NULL);
    int32_t r2 = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    if (r1 < 0 || r2 < 0) return 2;
    if (sieda_connect(p, r1, 1, r2, 0) < 0) return 3;
    if (!sieda_pcb_move_footprint(p, r1, 10, 10) || !sieda_pcb_move_footprint(p, r2, 30, 20)) return 4;
    char* s = sieda_router_begin(p, "{\"posture\":\"45\",\"teardrops\":true,\"removeLoops\":true}", 10.95, 10, 0);
    if (!s || strstr(s, "\"error\"")) return 5;
    sieda_string_free(s);
    s = sieda_router_move(p, 29.05, 20);
    if (!s || !strstr(s, "\"reachedTarget\":true")) return 6;
    sieda_string_free(s);
    s = sieda_router_commit(p);
    if (!s || !strstr(s, "\"ok\":true")) return 7;
    sieda_string_free(s);
    char* snap = sieda_project_snapshot(p);
    if (!snap || !strstr(snap, "\"teardrop\":true")) return 8; /* auto teardrops on commit */
    const char* tracksAt = strstr(snap, "\"tracks\":[{");
    const char* idAt = tracksAt ? strstr(tracksAt, "\"id\":") : NULL;
    if (!idAt) return 9;
    int32_t track = 0;
    for (const char* c = idAt + 5; *c >= '0' && *c <= '9'; ++c) track = track * 10 + (*c - '0');
    sieda_string_free(snap);
    char* rm = sieda_pcb_teardrops(p, "[]", "{\"remove\":true}");
    if (!rm || !strstr(rm, "\"ok\":true")) return 10;
    sieda_string_free(rm);
    char* td = sieda_pcb_teardrops(p, "[]", "{\"apply\":false}");
    if (!td || !strstr(td, "\"ok\":true") || !strstr(td, "\"applied\":false")) return 11;
    sieda_string_free(td);
    char ids[32];
    snprintf(ids, sizeof ids, "[%d]", (int)track);
    char* g = sieda_pcb_gloss(p, ids, NULL);
    if (!g || strstr(g, "\"error\"") || !strstr(g, "\"message\"")) return 12;
    sieda_string_free(g);
    char* sh = sieda_pcb_shield_tracks(p, ids, "{\"pitch\":1.5}");
    if (!sh || strstr(sh, "\"error\"") || !strstr(sh, "\"ok\"")) return 13; /* no ground net: ok false */
    sieda_string_free(sh);
    char* st = sieda_pcb_stitch_vias(p, "{\"x0\":0,\"y0\":0,\"x1\":20,\"y1\":20}");
    if (!st || !strstr(st, "\"ok\":false")) return 14; /* no pours */
    sieda_string_free(st);
    char* bad = sieda_pcb_gloss(p, "{nope", NULL);
    if (!bad || !strstr(bad, "\"error\"")) return 15;
    sieda_string_free(bad);
    if (sieda_pcb_teardrops(NULL, "[]", NULL) != NULL || sieda_pcb_stitch_vias(NULL, NULL) != NULL) return 16;
    sieda_project_free(p);
    return 0;
}

/* Forward annotation (Update PCB ECO) through the C API. Returns 0 or the failing step. */
int sieda_c_api_update_pcb_test(void) {
    SiedaProject* p = sieda_project_new("Update PCB");
    if (!p) return 1;
    int32_t r = sieda_add_component(p, 0, "1k", 0, 0, 0, NULL);
    if (r < 0) return 2;
    char* eco = sieda_pcb_eco_preview(p);
    if (!eco || !strstr(eco, "\"section\":\"component\"") || !strstr(eco, "\"action\":\"add\"")) return 3;
    sieda_string_free(eco);
    char* none = sieda_apply_pcb_eco(p, "[]");
    if (!none || !strstr(none, "\"executed\":0")) return 4;
    sieda_string_free(none);
    char* all = sieda_apply_pcb_eco(p, NULL);
    if (!all || strstr(all, "\"executed\":0") || !strstr(all, "\"report\"")) return 5;
    sieda_string_free(all);
    eco = sieda_pcb_eco_preview(p);
    if (!eco || strcmp(eco, "[]") != 0) return 6;
    sieda_string_free(eco);
    if (sieda_pcb_eco_preview(NULL) != NULL) return 7;
    sieda_project_free(p);
    return 0;
}

int sieda_c_api_match_lengths_test(void) {
    SiedaProject* p = sieda_project_new("C API match lengths");
    if (!p) return 1;
    int32_t a1 = sieda_add_component(p, 0 /* Resistor */, "1k", 0, 0, 0, NULL);
    int32_t a2 = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 0, 0, NULL);
    int32_t b1 = sieda_add_component(p, 0 /* Resistor */, "1k", 0, 100, 0, NULL);
    int32_t b2 = sieda_add_component(p, 0 /* Resistor */, "1k", 100, 100, 0, NULL);
    if (a1 < 0 || a2 < 0 || b1 < 0 || b2 < 0) return 2;
    if (sieda_connect(p, a1, 1, a2, 0) < 0 || sieda_connect(p, b1, 1, b2, 0) < 0) return 3;
    if (!sieda_pcb_move_footprint(p, a1, 10, 10) || !sieda_pcb_move_footprint(p, a2, 40, 10) ||
        !sieda_pcb_move_footprint(p, b1, 10, 20) || !sieda_pcb_move_footprint(p, b2, 40, 26))
        return 4;
    /* Stop mode: a plain route reaches its pad. */
    char* s = sieda_router_begin(p, "{\"mode\":\"stop\"}", 10.95, 10, 0);
    if (!s || strstr(s, "\"error\"")) return 5;
    sieda_string_free(s);
    s = sieda_router_move(p, 39.05, 10);
    if (!s || !strstr(s, "\"reachedTarget\":true")) return 6;
    sieda_string_free(s);
    s = sieda_router_commit(p);
    if (!s || !strstr(s, "\"ok\":true")) return 7;
    sieda_string_free(s);
    s = sieda_router_begin(p, "{\"mode\":\"stop\"}", 10.95, 20, 0);
    if (!s) return 8;
    sieda_string_free(s);
    s = sieda_router_move(p, 39.05, 26);
    if (!s || !strstr(s, "\"reachedTarget\":true")) return 9;
    sieda_string_free(s);
    s = sieda_router_commit(p);
    if (!s || !strstr(s, "\"ok\":true")) return 10;
    sieda_string_free(s);
    /* Every track: the two nets are matched (the straight one lengthened). */
    char* snap = sieda_project_snapshot(p);
    if (!snap) return 11;
    char ids[256] = "[";
    const char* at = strstr(snap, "\"tracks\":[");
    int n = 0;
    while (at && (at = strstr(at, "\"id\":")) != NULL && n < 20) {
        int id = 0;
        const char* c = at + 5;
        for (; *c >= '0' && *c <= '9'; ++c) id = id * 10 + (*c - '0');
        char part[16];
        snprintf(part, sizeof part, "%s%d", n ? "," : "", id);
        strncat(ids, part, sizeof ids - strlen(ids) - 2);
        ++n;
        at = c;
        if (strstr(at, "\"vias\":[") && strstr(at, "\"vias\":[") < strstr(at, "\"id\":")) break;
    }
    strcat(ids, "]");
    sieda_string_free(snap);
    char* m = sieda_pcb_match_lengths(p, ids, "{\"tolerance\":0.2}");
    if (!m || strstr(m, "\"error\"") || !strstr(m, "\"target\"")) return 12;
    sieda_string_free(m);
    char* bad = sieda_pcb_match_lengths(p, "{x", NULL);
    if (!bad || !strstr(bad, "\"error\"")) return 13;
    sieda_string_free(bad);
    if (sieda_pcb_match_lengths(NULL, "[]", NULL) != NULL) return 14;
    sieda_project_free(p);
    return 0;
}

/* Drawn sheet-symbol size through the C API. Returns 0 or the failing step. */
int sieda_c_api_sheet_symbol_size_test(void) {
    SiedaProject* p = sieda_project_new("Sheet symbol");
    if (!p) return 1;
    int32_t child = sieda_add_sheet(p, "Child", 1);
    if (child <= 0) return 2;
    if (sieda_set_sheet_symbol_size(p, child, 160, 80) != 1) return 3;
    if (sieda_set_sheet_symbol_size(p, 999, 1, 1) != 0 || sieda_set_sheet_symbol_size(NULL, child, 1, 1) != 0) return 4;
    char* json = sieda_project_save_json(p);
    if (!json || !strstr(json, "symbolSize") || !strstr(json, "160")) return 5;
    sieda_string_free(json);
    sieda_project_free(p);
    return 0;
}

/* Helper sheets and per-channel parameters through the C API. Returns 0 or the failing step. */
int sieda_c_api_helper_sheets_test(void) {
    SiedaProject* p = sieda_project_new("Helpers");
    if (!p) return 1;
    int32_t block = sieda_add_sheet(p, "Block", 1);
    if (block <= 0) return 2;
    sieda_set_active_sheet(p, block);
    int32_t r = sieda_add_component(p, 0, "1k", 0, 0, 0, NULL);
    if (r < 0) return 3;
    int32_t helper = sieda_add_helper_sheet(p, "Helper", block);
    if (helper <= 0 || sieda_add_helper_sheet(NULL, "X", block) != -1 || sieda_add_helper_sheet(p, "", block) != -1) return 4;
    if (sieda_repeat_sheet(p, block, 2) != 2) return 5;
    if (sieda_set_helper_sheet(p, helper, 0) != 0 || sieda_set_helper_sheet(p, 999, 1) != 0) return 6;
    if (sieda_set_channel_fitted(p, r, 0) != 1 || sieda_set_channel_fitted(p, 9999, 0) != 0) return 7;
    if (sieda_set_channel_firmware(p, r, "", "", 0) != 1) return 8;
    if (sieda_clear_channel_override(p, r, "spice") != 1 || sieda_clear_channel_override(p, r, "bogus") != 0) return 9;
    char* err = NULL;
    if (sieda_set_channel_spice_model(p, r, "", "", "", &err) != 1 || err) return 10;
    if (sieda_set_channel_spice_model(p, 9999, "", "", "", &err) != 0 || !err) return 11;
    sieda_string_free(err);
    char* json = sieda_project_save_json(p);
    if (!json || !strstr(json, "helper")) return 12;
    sieda_string_free(json);
    sieda_project_free(p);
    return 0;
}

/* Clipboard with buses, outline alignment, fixed frames and the PDF with a font, through the C API. 0 or the step. */
int sieda_c_api_schematic_polish_test(void) {
    SiedaProject* p = sieda_project_new("Polish");
    if (!p) return 1;
    int32_t a = sieda_add_component(p, 0, "1k", 0, 0, 0, NULL);
    int32_t b = sieda_add_component(p, 10, "TL071", 200, 100, 0, NULL);
    int32_t bus = sieda_add_bus(p, "D[0..3]", "[[0,200],[300,200]]");
    if (a < 0 || b < 0 || bus <= 0) return 2;
    char ids[64];
    snprintf(ids, sizeof ids, "{\"components\":[%d],\"buses\":[%d]}", (int)a, (int)bus);
    char* clip = sieda_copy_components(p, ids);
    if (!clip || !strstr(clip, "\"buses\"")) return 3;
    sieda_string_free(clip);
    snprintf(ids, sizeof ids, "[%d,%d]", (int)a, (int)b);
    if (sieda_align_outlines(p, ids, "left") < 1 || sieda_align_outlines(p, ids, "bogus") != -1) return 4;
    if (sieda_set_sheet_frame(p, 1, 1, 0, 0) != 0) return 5; /* no template yet */
    if (sieda_set_sheet_size(p, 1, "A4") != 1 || sieda_set_sheet_frame(p, 1, 1, -100, -100) != 1) return 6;
    char* pdf = sieda_export_schematic_pdf_with_font(p, "/no/such/font.ttf");
    if (!pdf || strncmp(pdf, "%PDF", 4) != 0) return 7;
    sieda_string_free(pdf);
    if (sieda_export_schematic_pdf_with_font(NULL, NULL) != NULL) return 8;
    sieda_project_free(p);
    return 0;
}

/* PCB pin / gate swap through the C API. Returns 0 or the failing step. */
int sieda_c_api_pcb_swap_test(void) {
    SiedaProject* p = sieda_project_new("Swap");
    if (!p) return 1;
    char* none = sieda_pcb_swap_options(p, 12345);
    if (!none || strcmp(none, "[]") != 0) return 2;
    sieda_string_free(none);
    if (sieda_apply_pcb_swap(p, "{\"kind\":\"pin\",\"component\":1,\"pinA\":0,\"pinB\":1}") != 0) return 3;
    if (sieda_apply_pcb_swap(p, "not json") != 0 || sieda_apply_pcb_swap(NULL, "{}") != 0) return 4;
    if (sieda_optimize_pcb_swaps(p, -1, 5) != 0 || sieda_optimize_pcb_swaps(NULL, -1, 5) != -1) return 5;
    if (sieda_pcb_swap_options(NULL, 1) != NULL) return 6;
    sieda_project_free(p);
    return 0;
}

/* Autorouter strategies through the C API: presets, options (saved with the project), keep-outs and the report. */
int sieda_c_api_autoroute_strategy_test(void) {
    SiedaProject* p = sieda_project_new("C API autoroute strategy");
    if (!p) return 1;
    int32_t r[2];
    for (int k = 0; k < 2; ++k) {
        r[k] = sieda_add_component(p, 0 /* Resistor */, "1k", 40.0 * k, 0, 0, NULL);
        if (r[k] < 0) return 2;
        if (!sieda_pcb_move_footprint(p, r[k], 10 + 30.0 * k, 15)) return 3;
    }
    if (sieda_connect(p, r[0], 1, r[1], 0) < 0) return 4;
    sieda_pcb_set_board(p, 50, 30, 0, 0);
    char* presets = sieda_autoroute_presets();
    if (!presets || !strstr(presets, "\"quality\"") || !strstr(presets, "\"fanout\"")) return 5;
    sieda_string_free(presets);
    if (sieda_pcb_set_autoroute_options(p, "{\"preset\":\"quality\",\"minimizeVias\":true,\"gloss\":true}") != 1) return 6;
    if (sieda_pcb_set_autoroute_options(p, "not json") != 0 || sieda_pcb_set_autoroute_options(NULL, "{}") != 0) return 7;
    char* opts = sieda_pcb_autoroute_options(p);
    if (!opts || !strstr(opts, "\"minimizeVias\":true") || !strstr(opts, "\"preset\":\"quality\"")) return 8;
    sieda_string_free(opts);
    if (sieda_pcb_set_keepouts(p, "[{\"name\":\"K\",\"x0\":22,\"y0\":2,\"x1\":28,\"y1\":28,\"layer\":0}]") != 1) return 9;
    char* ks = sieda_pcb_keepouts(p);
    if (!ks || !strstr(ks, "\"name\":\"K\"")) return 10;
    sieda_string_free(ks);
    char* routed = sieda_pcb_autoroute(p);
    if (!routed || !strstr(routed, "\"failed\":0")) return 11;
    sieda_string_free(routed);
    char* report = sieda_pcb_route_report(p);
    if (!report || !strstr(report, "\"metrics\"") || !strstr(report, "\"unrouted\":0")) return 12;
    sieda_string_free(report);
    char* saved = sieda_project_save_json(p);
    if (!saved || !strstr(saved, "\"autorouter\"") || !strstr(saved, "\"keepouts\"")) return 13;
    sieda_string_free(saved);
    if (sieda_pcb_route_report(NULL) != NULL || sieda_pcb_set_keepouts(p, "[]") != 0) return 14;
    /* Pin / gate swap: off by default (not written), round-trips, and the route reports its metrics. */
    opts = sieda_pcb_autoroute_options(p);
    if (!opts || strstr(opts, "pinSwap")) return 15;
    sieda_string_free(opts);
    if (sieda_pcb_set_autoroute_options(p, "{\"pinSwap\":true}") != 1) return 16;
    opts = sieda_pcb_autoroute_options(p);
    if (!opts || !strstr(opts, "\"pinSwap\":true") || !strstr(opts, "\"minimizeVias\":true")) return 17;
    sieda_string_free(opts);
    routed = sieda_pcb_autoroute(p);
    if (!routed || !strstr(routed, "\"pinSwaps\":0") || !strstr(routed, "\"gateSwaps\":0") || !strstr(routed, "\"ratsnestAfter\""))
        return 18;
    sieda_string_free(routed);
    report = sieda_pcb_route_report(p);
    if (!report || !strstr(report, "\"ratsnestBefore\"") || !strstr(report, "\"swaps\":[]")) return 19;
    sieda_string_free(report);
    saved = sieda_project_save_json(p);
    /* The saved file is pretty-printed ("key": value). */
    if (!saved || (!strstr(saved, "\"pinSwap\":true") && !strstr(saved, "\"pinSwap\": true"))) return 20;
    sieda_string_free(saved);
    sieda_project_free(p);
    return 0;
}

/* MCP engine through the C API: handshake, notifications, a project attached by the host (the app's document), the
 * change callback, read-only options and the tool table. Returns 0 or the failing step. */
static int g_mcp_changes = 0;
static void mcp_changed(void* user, const char* tool) {
    (void)user;
    if (tool && strstr(tool, "schematic_")) ++g_mcp_changes;
}

int sieda_c_api_mcp_test(void) {
    if (sieda_mcp_new("not json") != NULL) return 1;
    if (sieda_mcp_handle(NULL, "{}") != NULL) return 2;
    SiedaMcpServer* s = sieda_mcp_new("{\"readOnly\":false,\"serverName\":\"sieda-test\"}");
    if (!s) return 3;
    char* r = sieda_mcp_handle(
        s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"}}");
    if (!r || !strstr(r, "\"protocolVersion\":\"2025-06-18\"") || !strstr(r, "\"name\":\"sieda-test\"")) return 4;
    sieda_string_free(r);
    r = sieda_mcp_handle(s, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    if (!r || r[0] != '\0') return 5; /* notifications: an empty string */
    sieda_string_free(r);
    r = sieda_mcp_handle(s, "{not json");
    if (!r || !strstr(r, "-32700")) return 6;
    sieda_string_free(r);

    SiedaProject* p = sieda_project_new("Host document");
    if (!p) return 7;
    if (sieda_add_component(p, 0 /* Resistor */, "1k", 0, 0, 0, NULL) < 0) return 8;
    if (sieda_mcp_attach_project(s, p) != 1) return 9;
    sieda_mcp_set_change_callback(s, mcp_changed, NULL);
    r = sieda_mcp_handle(s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"project_summary\"}}");
    if (!r || !strstr(r, "Host document") || strstr(r, "\"isError\":true")) return 10;
    sieda_string_free(r);
    r = sieda_mcp_handle(s, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":"
                            "\"schematic_add_component\",\"arguments\":{\"kind\":\"resistor\",\"value\":\"2k2\"}}}");
    if (!r || strstr(r, "\"isError\":true") || !strstr(r, "R2")) return 11;
    sieda_string_free(r);
    if (sieda_find_component(p, "R2") < 0) return 12; /* the host's project changed in place */
    if (g_mcp_changes != 1) return 13;
    r = sieda_mcp_handle(s, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"schematic_connect\","
                            "\"arguments\":{\"from\":\"R1.2\",\"to\":\"R2.1\"}}}");
    if (!r || strstr(r, "\"isError\":true") || !strstr(r, "\"wiresAdded\":1")) return 14;
    sieda_string_free(r);

    /* Read-only: the same edit is refused as a tool error, and the project is untouched. */
    if (sieda_mcp_set_options(s, "{\"readOnly\":true}") != 1) return 15;
    if (sieda_mcp_set_options(s, "[1,2") != 0) return 16;
    r = sieda_mcp_handle(s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":"
                            "\"schematic_add_component\",\"arguments\":{\"kind\":\"resistor\"}}}");
    if (!r || !strstr(r, "\"isError\":true") || !strstr(r, "read-only")) return 17;
    sieda_string_free(r);
    if (sieda_find_component(p, "R3") >= 0) return 18;
    if (sieda_mcp_set_project_path(s, "/tmp/host.siedaproj") != 1) return 19;

    char* tools = sieda_mcp_tools_json();
    if (!tools || !strstr(tools, "\"pcb_autoroute\"") || !strstr(tools, "\"inputSchema\"")) return 20;
    sieda_string_free(tools);

    if (sieda_mcp_attach_project(s, NULL) != 1) return 21; /* detach before the host frees its project */
    sieda_project_free(p);
    r = sieda_mcp_handle(s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"project_summary\"}}");
    if (!r || strstr(r, "Host document")) return 22;
    sieda_string_free(r);
    sieda_mcp_free(s);
    sieda_mcp_free(NULL);
    return 0;
}
