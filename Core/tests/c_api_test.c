/* Compiled as C to guarantee sieda_c.h stays a valid C header (it is what Swift imports). */
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
