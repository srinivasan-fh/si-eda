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
