cmake_minimum_required(VERSION 3.16.3)
project(wiz)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LeapCTargets.cmake")

find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIR})

add_executable(wiz wizard.c)

set_target_properties(wiz
  PROPERTIES
    LANGUAGE C)

target_link_libraries(wiz
  ${CURSES_LIBRARIES}
  LeapSDK::LeapC)
