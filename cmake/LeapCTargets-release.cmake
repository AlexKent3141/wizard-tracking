#----------------------------------------------------------------
# Generated CMake target import file for configuration "release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "LeapSDK::LeapC" for configuration "release"
set_property(TARGET LeapSDK::LeapC APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(LeapSDK::LeapC PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/ultraleap-hand-tracking-service/libLeapC.so.5"
  IMPORTED_SONAME_RELEASE "libLeapC.so.5"
  )

list(APPEND _IMPORT_CHECK_TARGETS LeapSDK::LeapC )
list(APPEND _IMPORT_CHECK_FILES_FOR_LeapSDK::LeapC "${_IMPORT_PREFIX}/lib/ultraleap-hand-tracking-service/libLeapC.so.5" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
