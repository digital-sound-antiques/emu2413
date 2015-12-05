cmake_minimum_required(VERSION 3.0)
file(GLOB SOURCES *.c)
set(CMAKE_C_FLAGS "-Wall")
add_library(emu2413 STATIC ${SOURCES})
