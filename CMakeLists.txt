cmake_minimum_required(VERSION 3.20)
project(SiEDA VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SIEDA_BUILD_TESTS "Build SiEDA core unit tests" ON)
option(SIEDA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# Memory and undefined-behaviour checking for every target (the CI sanitizer job); leaks are reported at exit.
option(SIEDA_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
# libFuzzer targets for the file readers (Core/tests/fuzz/fuzz.cpp); clang only, implies the sanitizers.
option(SIEDA_FUZZ "Build the libFuzzer targets (clang)" OFF)
if(SIEDA_SANITIZE OR SIEDA_FUZZ)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
if(SIEDA_FUZZ)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# Built-in example projects too large to build in code (MCP project_load_example), embedded from examples/.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/example_smps.cpp
                   COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/examples/smps-230vac-5-48v/smps_230vac.siedaproj
                           -DOUT=${CMAKE_CURRENT_BINARY_DIR}/example_smps.cpp -DNAME=kSmpsExample -P ${CMAKE_CURRENT_SOURCE_DIR}/Core/studio/embed.cmake
                   DEPENDS examples/smps-230vac-5-48v/smps_230vac.siedaproj Core/studio/embed.cmake)
add_library(sieda_core STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/example_smps.cpp
    Core/src/Json.cpp
    Core/src/Units.cpp
    Core/src/Library.cpp
    Core/src/CustomParts.cpp
    Core/src/LibraryImport.cpp
    Core/src/StandardParts.cpp
    Core/src/DeviceModels.cpp
    Core/src/Industry.cpp
    Core/src/Validation.cpp
    Core/src/Verification.cpp
    Core/src/Schematic.cpp
    Core/src/Sheets.cpp
    Core/src/Instances.cpp
    Core/src/Buses.cpp
    Core/src/Harnesses.cpp
    Core/src/Directives.cpp
    Core/src/SchematicEdit.cpp
    Core/src/Eco.cpp
    Core/src/InteractivePlacement.cpp
    Core/src/SchematicPdf.cpp
    Core/src/PdfFont.cpp
    Core/src/LocalModel.cpp
    Core/src/CircuitValues.cpp
    Core/src/sieda_c_llm.cpp
    Core/src/PcbSwap.cpp
    Core/src/PartUnits.cpp
    Core/src/SchematicSearch.cpp
    Core/src/Variants.cpp
    Core/src/ProjectDiff.cpp
    Core/src/CloudSync.cpp
    Core/src/ExternalLibrary.cpp
    Core/src/Mechanical.cpp
    Core/src/CustomRules.cpp
    Core/src/Simulator.cpp
    Core/src/Analysis.cpp
    Core/src/SpiceModels.cpp
    Core/src/SpiceDevices.cpp
    Core/src/SpiceBuild.cpp
    Core/src/Noise.cpp
    Core/src/Convergence.cpp
    Core/src/Waveforms.cpp
    Core/src/Pcb.cpp
    Core/src/GlobalRouter.cpp
    Core/src/Zones.cpp
  Core/src/Avr.cpp
  Core/src/Firmware.cpp
    Core/src/Mesh.cpp
    Core/src/Export.cpp
    Core/src/Documents.cpp
    Core/src/Fabrication.cpp
    Core/src/FabExchange.cpp
    Core/src/RouteExchange.cpp
    Core/src/Panel.cpp
    Core/src/Dfm.cpp
    Core/src/FieldSolver.cpp
    Core/src/Bom.cpp
    Core/src/Reliability.cpp
    Core/src/Stackup.cpp
    Core/src/Thermal.cpp
    Core/src/LengthMatch.cpp
    Core/src/LengthRules.cpp
    Core/src/Autoroute.cpp
    Core/src/RouteQuality.cpp
    Core/src/sieda_c_autoroute.cpp
    Core/src/InteractiveRouter.cpp
    Core/src/TrackGeometry.cpp
    Core/src/Ibis.cpp
    Core/src/SignalIntegrity.cpp
    Core/src/PowerIntegrity.cpp
    Core/src/LossyLine.cpp
    Core/src/Channel.cpp
    Core/src/Eye.cpp
    Core/src/Touchstone.cpp
    Core/src/PdnPlanning.cpp
    Core/src/Embedded.cpp
    Core/src/Robotics.cpp
    Core/src/Automotive.cpp
    Core/src/Aerospace.cpp
    Core/src/Naval.cpp
    Core/src/Isolation.cpp
    Core/src/Medical.cpp
    Core/src/Retail.cpp
    Core/src/Appliance.cpp
    Core/src/Memory.cpp
    Core/src/Project.cpp
    Core/src/Suppliers.cpp
    Core/src/Model3D.cpp
    Core/src/AltiumLibrary.cpp
    Core/src/sieda_c.cpp
    Core/src/sieda_c_routing.cpp
    Core/src/sieda_c_placement.cpp
    Core/src/Mcp.cpp
    Core/src/McpTools.cpp
    Core/src/McpRender.cpp
    Core/src/sieda_c_mcp.cpp
)
target_include_directories(sieda_core PUBLIC Core/include)
find_package(Threads REQUIRED)
target_link_libraries(sieda_core PUBLIC Threads::Threads)  # the autorouter routes independent nets in parallel
# Optional HarfBuzz: shapes the schematic PDF's embedded-font text (Indic conjuncts, Arabic joining, right-to-left).
option(SIEDA_HARFBUZZ "Shape PDF text with HarfBuzz when it is installed" ON)
find_package(PkgConfig QUIET)
if(SIEDA_HARFBUZZ AND PkgConfig_FOUND)
    pkg_check_modules(HARFBUZZ QUIET IMPORTED_TARGET harfbuzz)
    if(HARFBUZZ_FOUND)
        target_link_libraries(sieda_core PUBLIC PkgConfig::HARFBUZZ)
        target_compile_definitions(sieda_core PRIVATE SIEDA_HAVE_HARFBUZZ)
    endif()
endif()
if(MINGW)  # self-contained .exe files: no MinGW runtime DLLs to ship beside them
    add_link_options(-static)
endif()
if(WIN32)  # M_PI and friends from <cmath>; no min / max macros from <windows.h>
    target_compile_definitions(sieda_core PUBLIC _USE_MATH_DEFINES NOMINMAX)
endif()
if(MSVC)
    target_compile_options(sieda_core PRIVATE /W4)
else()
    target_compile_options(sieda_core PRIVATE -Wall -Wextra -Wpedantic)
    if(SIEDA_WARNINGS_AS_ERRORS)
        target_compile_options(sieda_core PRIVATE -Werror)
    endif()
endif()

add_executable(sieda-cli Core/cli/main.cpp)
target_link_libraries(sieda-cli PRIVATE sieda_core)

# MCP server over stdio (docs/MCP.md): any MCP client (Claude Desktop / Code, Cursor, VS Code …) drives SiEDA.
add_executable(sieda-mcp Core/mcp/main.cpp Core/mcp/http_bridge.cpp)
target_link_libraries(sieda-mcp PRIVATE sieda_core)
target_compile_definitions(sieda-mcp PRIVATE SIEDA_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs")
if(WIN32)
    target_link_libraries(sieda-mcp PRIVATE ws2_32)
endif()
if(NOT MSVC)
    target_compile_options(sieda-mcp PRIVATE -Wall -Wextra -Wpedantic)
    if(SIEDA_WARNINGS_AS_ERRORS)
        target_compile_options(sieda-mcp PRIVATE -Werror)
    endif()
endif()

# SiEDA Studio: the desktop app on Linux and Windows, served by the core to the system browser (docs/STUDIO.md).
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/studio_ui.cpp
                   COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/Core/studio/ui.html
                           -DOUT=${CMAKE_CURRENT_BINARY_DIR}/studio_ui.cpp -DNAME=kStudioHtml -P ${CMAKE_CURRENT_SOURCE_DIR}/Core/studio/embed.cmake
                   DEPENDS Core/studio/ui.html Core/studio/embed.cmake)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/studio_i18n.cpp
                   COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/Core/studio/i18n.json
                           -DOUT=${CMAKE_CURRENT_BINARY_DIR}/studio_i18n.cpp -DNAME=kStudioI18n -P ${CMAKE_CURRENT_SOURCE_DIR}/Core/studio/embed.cmake
                   DEPENDS Core/studio/i18n.json Core/studio/embed.cmake)
add_executable(sieda-studio Core/studio/main.cpp ${CMAKE_CURRENT_BINARY_DIR}/studio_ui.cpp ${CMAKE_CURRENT_BINARY_DIR}/studio_i18n.cpp)
target_link_libraries(sieda-studio PRIVATE sieda_core)
target_compile_definitions(sieda-studio PRIVATE SIEDA_STUDIO_VERSION="${PROJECT_VERSION}")
if(WIN32)
    enable_language(RC)  # the app icon and version details (packaging/windows)
    configure_file(packaging/windows/sieda-studio.rc.in ${CMAKE_CURRENT_BINARY_DIR}/sieda-studio.rc @ONLY)
    target_sources(sieda-studio PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/sieda-studio.rc)
    target_link_libraries(sieda-studio PRIVATE ws2_32 shell32)
    set_target_properties(sieda-studio PROPERTIES WIN32_EXECUTABLE OFF)
endif()
if(NOT MSVC)
    target_compile_options(sieda-studio PRIVATE -Wall -Wextra -Wpedantic)
    if(SIEDA_WARNINGS_AS_ERRORS)
        target_compile_options(sieda-studio PRIVATE -Werror)
    endif()
endif()

if(SIEDA_BUILD_TESTS)
    enable_testing()
    add_executable(sieda_core_tests Core/tests/core_tests.cpp Core/tests/c_api_test.c)
    target_link_libraries(sieda_core_tests PRIVATE sieda_core)
    target_compile_definitions(sieda_core_tests PRIVATE SIEDA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Core/tests/fixtures"
                               SIEDA_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs")
    add_test(NAME sieda_core_tests COMMAND sieda_core_tests)
    # The stdio server end to end: a scripted transcript on stdin, every stdout line must be a JSON-RPC response.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME sieda_mcp_stdio
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Core/tests/mcp_stdio_test.py
                         $<TARGET_FILE:sieda-mcp> ${CMAKE_CURRENT_SOURCE_DIR}/Core/tests/fixtures/mcp/transcript.jsonl)
        # The Python API (python/sieda.py) drives sieda-mcp end to end.
        add_test(NAME sieda_python_api
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_sieda.py $<TARGET_FILE:sieda-mcp>)
        # The simulator against ngspice (operating points, AC, transient), when ngspice is installed.
        find_program(NGSPICE ngspice)
        if(NGSPICE)
            add_test(NAME sieda_sim_correlation
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Core/tests/sim_correlation_test.py
                             $<TARGET_FILE:sieda-mcp> ${NGSPICE})
        endif()
        # SiEDA Studio's server: token and Host checks, tool calls, /state, /quit.
        add_test(NAME sieda_studio
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Core/tests/studio_test.py $<TARGET_FILE:sieda-studio>)
        # sieda-mcp --connect (stdio to the app's live HTTP endpoint) against a small in-process HTTP server.
        if(NOT WIN32)
            add_test(NAME sieda_mcp_connect
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Core/tests/mcp_connect_test.py
                             $<TARGET_FILE:sieda-mcp>)
        endif()
    endif()
    # Scale benchmark: place / route / DRC timings on generated large boards. CTest runs a ~350-part board under a
    # generous budget (about 10× today's time) so an algorithmic slow-down or memory blow-up fails CI.
    add_executable(sieda_route_bench Core/tests/bench/route_bench.cpp)
    target_link_libraries(sieda_route_bench PRIVATE sieda_core)
    # Batch conversion: a Protel netlist becomes a project (sieda-cli --convert).
    add_test(NAME sieda_cli_convert COMMAND sieda-cli --convert ${CMAKE_CURRENT_BINARY_DIR}/converted
             ${CMAKE_CURRENT_SOURCE_DIR}/Core/tests/fixtures/netlist/two_parts.net)
    add_test(NAME sieda_scale_budget COMMAND sieda_route_bench --clusters 12 --layers 6 --seed 1 --budget 90 800 --edit-budget 1500)
    # Interactive-router benchmark: head-update times on a generated, autorouted ~400-part board (not part of ctest).
    add_executable(sieda_router_bench Core/tests/bench/router_bench.cpp)
    target_link_libraries(sieda_router_bench PRIVATE sieda_core)
endif()

if(SIEDA_FUZZ)
    foreach(target json project spice library idf)
        string(TOUPPER ${target} upper)
        add_executable(sieda_fuzz_${target} Core/tests/fuzz/fuzz.cpp)
        target_compile_definitions(sieda_fuzz_${target} PRIVATE SIEDA_FUZZ_${upper})
        target_link_options(sieda_fuzz_${target} PRIVATE -fsanitize=fuzzer)
        target_link_libraries(sieda_fuzz_${target} PRIVATE sieda_core)
    endforeach()
endif()
