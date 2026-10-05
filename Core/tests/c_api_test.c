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
    sieda_project_free(p);
    return 0;
}
