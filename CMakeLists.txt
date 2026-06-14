cmake_minimum_required(VERSION 3.16)
project(rayforce C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_C_EXTENSIONS OFF)

file(GLOB_RECURSE SRC_FILES
        "${CMAKE_SOURCE_DIR}/src/*.c"
)

if (NOT WIN32)
    message(STATUS "Not building on windows, remove source files in src/core/win.")
    #list(FILTER SRC_FILES EXCLUDE REGEX "src/core/win/mman.c")
    list(FILTER SRC_FILES EXCLUDE REGEX "src/core/win/.*\\.c")
endif()

include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(rayforce STATIC ${SRC_FILES})
if (WIN32)
    message(STATUS "Building on windows, add ws2_32.")
    target_link_libraries(rayforce PUBLIC ws2_32)
endif()

add_executable(main ${CMAKE_SOURCE_DIR}/src/app/main.c)

target_link_libraries(main rayforce)

# test
#file(GLOB_RECURSE TEST_SRC_FILES
#        "${CMAKE_SOURCE_DIR}/test/*.c"
#)
#include_directories(${CMAKE_SOURCE_DIR}/test)
#add_library(rayforce_test STATIC ${TEST_SRC_FILES})
#
#add_executable(test ${CMAKE_SOURCE_DIR}/test/main.c)
#target_link_libraries(test rayforce_test rayforce)