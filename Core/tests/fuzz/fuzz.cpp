// libFuzzer targets for everything SiEDA reads from outside: project / JSON files, SPICE model libraries, KiCad /
// Eagle / Altium libraries and 3D models, and IDF placement files. Built only with -DSIEDA_FUZZ=ON (clang); one
// binary per target, chosen by SIEDA_FUZZ_<NAME>. A crash, hang or sanitizer report is a bug: inputs may be refused
// (an exception or an error result), never mishandled.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include "sieda/Json.hpp"
#include "sieda/LibraryImport.hpp"
#include "sieda/Mechanical.hpp"
#include "sieda/Project.hpp"
#include "sieda/SpiceModels.hpp"

using namespace sieda;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    try {
#if defined(SIEDA_FUZZ_JSON)
        (void)Json::parse(text).dump();
#elif defined(SIEDA_FUZZ_PROJECT)
        Project p = Project::fromJson(Json::parse(text));
        (void)p.snapshot().dump();
#elif defined(SIEDA_FUZZ_SPICE)
        const SpiceLibrary library = parseSpiceLibrary(text);
        for (const auto& entry : spiceLibraryEntries(library)) (void)flattenSpiceModel(library, entry.name);
#elif defined(SIEDA_FUZZ_LIBRARY)
        // The first byte picks the format, the rest is the file.
        static const char* names[] = {"f.kicad_mod", "f.kicad_sym", "f.lbr", "f.SchLib", "f.PcbLib", "f.IntLib",
                                      "f.wrl", "f.stl", "f.obj"};
        if (size == 0) return 0;
        (void)libraryImportToJson(importLibraryFiles({{names[data[0] % 9], text.substr(1)}})).dump();
#elif defined(SIEDA_FUZZ_IDF)
        Schematic sch;
        sch.addComponent(ComponentKind::Resistor, "10k", {0, 0});
        sch.addComponent(ComponentKind::Capacitor, "100n", {100, 0});
        (void)importIdfPlacement(sch, text);
#else
#error "Define one SIEDA_FUZZ_<TARGET>"
#endif
    } catch (const std::exception&) {
    }
    return 0;
}
