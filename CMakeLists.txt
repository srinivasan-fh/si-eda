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
    Core/src/StandardParts.cpp
    Core/src/DeviceModels.cpp
    Core/src/Industry.cpp
    Core/src/Validation.cpp
    Core/src/Verification.cpp
    Core/src/Schematic.cpp
    Core/src/Sheets.cpp
    Core/src/Variants.cpp
    Core/src/Simulator.cpp
    Core/src/Analysis.cpp
    Core/src/Pcb.cpp
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
    Core/src/sieda_c.cpp
)
target_include_directories(sieda_core PUBLIC Core/include)
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

if(SIEDA_BUILD_TESTS)
    enable_testing()
    add_executable(sieda_core_tests Core/tests/core_tests.cpp Core/tests/c_api_test.c)
    target_link_libraries(sieda_core_tests PRIVATE sieda_core)
    add_test(NAME sieda_core_tests COMMAND sieda_core_tests)
endif()
