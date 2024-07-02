cmake_minimum_required(VERSION 3.16.3)
project(wiz)

find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIR})

add_executable(wiz wizard.c)

set_target_properties(wiz
  PROPERTIES
    LANGUAGE C)

message(${CURSES_LIBRARIES})
target_link_libraries(wiz ${CURSES_LIBRARIES})
