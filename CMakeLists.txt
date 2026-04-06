cmake_minimum_required(VERSION 3.11)
project(inspectrum CXX)
enable_testing()

# On Windows with MSVC, fetch prebuilt LiquidDSP if not provided externally.
# Users can skip this by setting -DLIQUID_INCLUDES=... -DLIQUID_LIBRARIES=...
if (MSVC AND NOT LIQUID_INCLUDES)
    include(cmake/FetchLiquidDSP.cmake)
endif()

add_subdirectory(src)
