# - Find LIQUID
# Find the native LIQUID includes and library
#
#  LIQUID_INCLUDES    - where to find liquid/liquid.h
#  LIQUID_LIBRARIES   - List of libraries when using LIQUID.
#  LIQUID_FOUND       - True if LIQUID found.

if (LIQUID_INCLUDES)
  # Already in cache, be silent
  set (LIQUID_FIND_QUIETLY TRUE)
endif (LIQUID_INCLUDES)

# On Windows, try the FetchContent-provided prebuilt first
if (LIQUIDDSP_INCLUDE_DIR AND LIQUIDDSP_LIBRARY)
  set(LIQUID_INCLUDES ${LIQUIDDSP_INCLUDE_DIR})
  set(LIQUID_LIBRARIES ${LIQUIDDSP_LIBRARY})
  set(LIQUID_FOUND TRUE)
endif()

if (NOT LIQUID_FOUND)
  find_path (LIQUID_INCLUDES liquid/liquid.h)

  find_library (LIQUID_LIBRARIES NAMES liquid)

  # handle the QUIETLY and REQUIRED arguments and set LIQUID_FOUND to TRUE if
  # all listed variables are TRUE
  include (FindPackageHandleStandardArgs)
  find_package_handle_standard_args (LIQUID DEFAULT_MSG LIQUID_LIBRARIES LIQUID_INCLUDES)
endif()
