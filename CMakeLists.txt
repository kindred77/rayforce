cmake_minimum_required(VERSION 3.16)
project(rayforce C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_C_EXTENSIONS OFF)

file(GLOB_RECURSE SRC_FILES
        "${CMAKE_SOURCE_DIR}/src/*.c"
)

if (WIN32)
    list(FILTER SRC_FILES EXCLUDE REGEX "src/core/win/mman.c")
endif()

include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(rayforce STATIC ${SRC_FILES})