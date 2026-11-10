# The recompiler's PC-side tools (xbe_symbols, xbe_recomp) built with MSVC, without the PC game's
# SDL3/ImGui/Vulkan dependencies. Only these are needed to generate the code the Xbox 360 build compiles.
cmake_minimum_required(VERSION 3.25)
project(BreX360Tools LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../../pc-recomp/sdk)

include(FetchContent)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zydis tomlplusplus)

# the tools use clang's printf format attribute only as a warning aid
set(TOOL_DEFS _CRT_SECURE_NO_WARNINGS)

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS ${SDK}/tools/recomp/*.cpp)
add_executable(xbe_recomp ${RECOMP_SOURCES})
target_include_directories(xbe_recomp PRIVATE ${SDK}/runtime ${SDK}/tools/recomp)
target_compile_definitions(xbe_recomp PRIVATE ${TOOL_DEFS})
target_compile_options(xbe_recomp PRIVATE /utf-8 /bigobj /FI${CMAKE_CURRENT_SOURCE_DIR}/msvc_compat.h)
target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)

add_executable(xbe_symbols ${SDK}/tools/symbols/main.cpp)
target_include_directories(xbe_symbols PRIVATE ${SDK}/runtime)
target_compile_definitions(xbe_symbols PRIVATE ${TOOL_DEFS})
target_compile_options(xbe_symbols PRIVATE /utf-8 /bigobj /FI${CMAKE_CURRENT_SOURCE_DIR}/msvc_compat.h)
target_link_libraries(xbe_symbols PRIVATE Zydis tomlplusplus::tomlplusplus)
