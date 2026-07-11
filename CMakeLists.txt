cmake_minimum_required(VERSION 3.8)

set(VERSION 0.0.0)
project(avrl C)
project(avrl VERSION ${VERSION})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(HEAD ${PROJECT_NAME})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2 -std=gnu11 -pthread")

# alligator/src provides common/pcre_parser.h (regex_match struct). avrl core
# otherwise depends only on libc + libpcre + libjansson -- no heavy deps.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(../alligator/src)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

if(APPLE)
    # homebrew include locations (best effort)
    file(GLOB PCRE_INC_DIRS /opt/homebrew/Cellar/pcre/*/include /usr/local/Cellar/pcre/*/include)
    file(GLOB JANSSON_INC_DIRS /opt/homebrew/Cellar/jansson/*/include /usr/local/Cellar/jansson/*/include)
    include_directories(${PCRE_INC_DIRS} ${JANSSON_INC_DIRS})
    file(GLOB PCRE_LIB_DIRS /opt/homebrew/Cellar/pcre/*/lib /usr/local/Cellar/pcre/*/lib)
    file(GLOB JANSSON_LIB_DIRS /opt/homebrew/Cellar/jansson/*/lib /usr/local/Cellar/jansson/*/lib)
    link_directories(${PCRE_LIB_DIRS} ${JANSSON_LIB_DIRS})
endif()
include_directories(/usr/local/include)
link_directories(/usr/local/lib)

set(AVRL_CORE_SOURCES
    value.c
    pcre_wrap.c
    lexer.c
    parser.c
    interp.c
    stdlib.c
    json.c
    multiline.c
    vrl.c
)

add_library(${HEAD}_static STATIC ${AVRL_CORE_SOURCES})
set_target_properties(${HEAD}_static PROPERTIES C_STANDARD 11 OUTPUT_NAME ${HEAD})
target_include_directories(${HEAD}_static PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../alligator/src
)

add_executable(${HEAD} test.c)
set_target_properties(${HEAD} PROPERTIES C_STANDARD 11)

find_library(PCRE_LIB NAMES pcre HINTS /usr/lib64 /usr/lib /usr/local/lib ${PCRE_LIB_DIRS})
if(NOT PCRE_LIB)
    message(FATAL_ERROR "PCRE library not found. Install pcre-devel (or equivalent).")
endif()
find_library(JANSSON_LIB NAMES jansson HINTS /usr/lib64 /usr/lib /usr/local/lib ${JANSSON_LIB_DIRS})
if(NOT JANSSON_LIB)
    message(FATAL_ERROR "jansson library not found. Install jansson-devel (or equivalent).")
endif()

target_link_libraries(${HEAD} ${HEAD}_static ${PCRE_LIB} ${JANSSON_LIB} pthread m)

install(TARGETS ${HEAD}_static ARCHIVE DESTINATION lib)
