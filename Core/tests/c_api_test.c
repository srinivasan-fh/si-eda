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
    sieda_project_free(p);
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
