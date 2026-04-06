# FetchLiquidDSP.cmake - Download prebuilt LiquidDSP for Windows (MSVC)
#
# LiquidDSP (https://github.com/jgaeddert/liquid-dsp) is not available in
# vcpkg as of 2025. This script downloads prebuilt Windows binaries from the
# CubicSDR project, which bundles them for its own MSVC build.
#
# TODO: A better long-term approach would be to add a LiquidDSP port to
# vcpkg upstream. If you're interested in contributing:
#   - vcpkg docs: https://learn.microsoft.com/en-us/vcpkg/get_started/get-started-adding-to-registry
#   - liquid-dsp: https://github.com/jgaeddert/liquid-dsp
#   - Once a vcpkg port exists, add "liquid-dsp" to vcpkg.json and remove
#     this file entirely.

if (NOT MSVC)
    return()
endif()

include(FetchContent)

FetchContent_Declare(
    liquiddsp_prebuilt
    GIT_REPOSITORY https://github.com/cjcliffe/CubicSDR.git
    GIT_TAG        033330367cd179d7b04503027d41afa7514036f4
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    # Only need the external/liquid-dsp directory, but git sparse checkout
    # isn't supported by FetchContent. The full clone is ~30MB.
)

# Use FetchContent_Populate (not MakeAvailable) because we only need the
# downloaded files -- we do NOT want CMake to process CubicSDR's own build.
FetchContent_GetProperties(liquiddsp_prebuilt)
if (NOT liquiddsp_prebuilt_POPULATED)
    message(STATUS "Fetching prebuilt LiquidDSP from CubicSDR...")
    FetchContent_Populate(liquiddsp_prebuilt)
endif()

set(LIQUIDDSP_PREFIX "${liquiddsp_prebuilt_SOURCE_DIR}/external/liquid-dsp")
set(LIQUIDDSP_INCLUDE_DIR "${LIQUIDDSP_PREFIX}/include" CACHE PATH "LiquidDSP include directory")
set(LIQUIDDSP_LIBRARY "${LIQUIDDSP_PREFIX}/msvc/64/libliquid.lib" CACHE FILEPATH "LiquidDSP import library")
set(LIQUIDDSP_DLL "${LIQUIDDSP_PREFIX}/msvc/64/libliquid.dll" CACHE FILEPATH "LiquidDSP runtime DLL")

# Make the DLL available at runtime by copying it next to the executable
# after the build (handled in src/CMakeLists.txt via install or post-build)
