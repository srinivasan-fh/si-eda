cmake_minimum_required(VERSION 3.20)
project(SiEDA VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SIEDA_BUILD_TESTS "Build SiEDA core unit tests" ON)
option(SIEDA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(sieda_core STATIC
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
    Core/src/PcbSwap.cpp
    Core/src/PartUnits.cpp
    Core/src/SchematicSearch.cpp
    Core/src/Variants.cpp
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
    Core/src/Fabrication.cpp
    Core/src/Bom.cpp
    Core/src/Reliability.cpp
    Core/src/Stackup.cpp
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
add_executable(sieda-mcp Core/mcp/main.cpp)
target_link_libraries(sieda-mcp PRIVATE sieda_core)
target_compile_definitions(sieda-mcp PRIVATE SIEDA_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs")
if(NOT MSVC)
    target_compile_options(sieda-mcp PRIVATE -Wall -Wextra -Wpedantic)
    if(SIEDA_WARNINGS_AS_ERRORS)
        target_compile_options(sieda-mcp PRIVATE -Werror)
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
    endif()
    # Scale benchmark: place / route / DRC timings on generated large boards (not part of ctest).
    add_executable(sieda_route_bench Core/tests/bench/route_bench.cpp)
    target_link_libraries(sieda_route_bench PRIVATE sieda_core)
    # Interactive-router benchmark: head-update times on a generated, autorouted ~400-part board (not part of ctest).
    add_executable(sieda_router_bench Core/tests/bench/router_bench.cpp)
    target_link_libraries(sieda_router_bench PRIVATE sieda_core)
endif()
