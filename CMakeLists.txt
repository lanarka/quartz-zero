cmake_minimum_required(VERSION 3.22)
project(raysrv C)
set(CMAKE_C_STANDARD 99)
set(VERSION_MAJOR 0)
set(VERSION_MINOR 1)
set(CMAKE_BUILD_TYPE Release)

set(EXECUTABLE_NAME "quartz")
set(EXECUTABLE_NAME_DISK "qdisk")
set(EXECUTABLE_NAME_HOST "qhost")

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
    GIT_TAG "5.5"
    GIT_PROGRESS TRUE
)

FetchContent_MakeAvailable(raylib)

set(PROJECT_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/src/")

file(GLOB_RECURSE EXECUTABLE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/codepoints.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/svfs.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/vmnet.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/vmnet_peer.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/vm.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/quartz.c"
)

add_executable(${EXECUTABLE_NAME})
target_sources(${EXECUTABLE_NAME} PRIVATE ${EXECUTABLE_SOURCES})
target_include_directories(${EXECUTABLE_NAME} PRIVATE ${PROJECT_INCLUDE})
target_link_libraries(${EXECUTABLE_NAME} PRIVATE raylib)
target_compile_options(${EXECUTABLE_NAME} PRIVATE
    -Wall 
    -Wextra 
    -Wconversion 
    -Wstrict-prototypes
    -Wdiscarded-qualifiers
)

file(GLOB_RECURSE EXECUTABLE_SOURCES_DISK CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/svfs.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/qdisk.c"
)

add_executable(${EXECUTABLE_NAME_DISK})
target_sources(${EXECUTABLE_NAME_DISK} PRIVATE ${EXECUTABLE_SOURCES_DISK})
target_include_directories(${EXECUTABLE_NAME_DISK} PRIVATE ${PROJECT_INCLUDE})
target_compile_options(${EXECUTABLE_NAME_DISK} PRIVATE
    -Wall 
    -Wextra 
    -Wconversion 
    -Wstrict-prototypes
    -Wdiscarded-qualifiers
)

file(GLOB_RECURSE EXECUTABLE_SOURCES_HOST CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/vmnet.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/vmnet_peer.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/qhost.c"
)

add_executable(${EXECUTABLE_NAME_HOST})
target_sources(${EXECUTABLE_NAME_HOST} PRIVATE ${EXECUTABLE_SOURCES_HOST})
target_include_directories(${EXECUTABLE_NAME_HOST} PRIVATE ${PROJECT_INCLUDE})
target_compile_options(${EXECUTABLE_NAME_HOST} PRIVATE
    -Wall 
    -Wextra 
    -Wconversion 
    -Wstrict-prototypes
    -Wdiscarded-qualifiers
)
