cmake_minimum_required(VERSION 3.10)

set(VERSION 0.0.0)
project(avrl VERSION ${VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Conan toolchain (CMakeDeps + CMakeToolchain). Prefer an explicit
# -DCMAKE_TOOLCHAIN_FILE=... or `cmake --preset conan-release`.
# Fallback: look for the usual cmake_layout generator paths.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR "${CMAKE_TOOLCHAIN_FILE}" STREQUAL "")
    foreach(_avrl_tc
            "${CMAKE_SOURCE_DIR}/build/Release/generators/conan_toolchain.cmake"
            "${CMAKE_SOURCE_DIR}/build/Debug/generators/conan_toolchain.cmake"
            "${CMAKE_SOURCE_DIR}/build/build/Release/generators/conan_toolchain.cmake"
            "${CMAKE_SOURCE_DIR}/build/build/Debug/generators/conan_toolchain.cmake")
        if(EXISTS "${_avrl_tc}")
            set(CMAKE_TOOLCHAIN_FILE "${_avrl_tc}" CACHE FILEPATH "Conan Toolchain")
            break()
        endif()
    endforeach()
endif()
if(EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    get_filename_component(CONAN_GENERATORS_DIR "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
    set(CMAKE_PREFIX_PATH "${CONAN_GENERATORS_DIR}" ${CMAKE_PREFIX_PATH})
endif()

set(HEAD ${PROJECT_NAME})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2 -std=gnu11 -pthread")

# alligator/src provides common/pcre_parser.h (regex_match struct). avrl core
# otherwise depends only on libc + libpcre + libjansson -- OpenSSL is opt-in.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(../alligator/src)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

option(AVRL_WITH_OPENSSL "Build OpenSSL-backed crypto VRL functions (md5/sha*/hmac/encrypt/...)" ON)

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
    stdlib_util.c
    stdlib_type.c
    stdlib_string.c
    stdlib_collection.c
    stdlib_codec.c
    stdlib_number.c
    stdlib_random.c
    stdlib_path.c
    stdlib_parse.c
    json.c
    multiline.c
    vrl.c
)

if(AVRL_WITH_OPENSSL)
    list(APPEND AVRL_CORE_SOURCES stdlib_crypto.c)
endif()

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

set(AVRL_LINK_LIBS ${HEAD}_static ${PCRE_LIB} ${JANSSON_LIB} pthread m)

if(AVRL_WITH_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(${HEAD}_static PRIVATE AVRL_WITH_OPENSSL=1)
    target_compile_definitions(${HEAD} PRIVATE AVRL_WITH_OPENSSL=1)
    # Prefer Conan's openssl::openssl / OpenSSL::Crypto imported targets.
    set(AVRL_OPENSSL_TARGET "")
    if(TARGET openssl::openssl)
        set(AVRL_OPENSSL_TARGET openssl::openssl)
    elseif(TARGET OpenSSL::Crypto)
        set(AVRL_OPENSSL_TARGET OpenSSL::Crypto)
    endif()
    if(AVRL_OPENSSL_TARGET)
        target_link_libraries(${HEAD}_static PUBLIC ${AVRL_OPENSSL_TARGET})
        list(APPEND AVRL_LINK_LIBS ${AVRL_OPENSSL_TARGET})
    else()
        list(APPEND AVRL_LINK_LIBS ${OPENSSL_CRYPTO_LIBRARY})
        target_include_directories(${HEAD}_static PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_include_directories(${HEAD} PRIVATE ${OPENSSL_INCLUDE_DIR})
    endif()
    # Static OpenSSL on macOS/Linux may need system libs.
    if(APPLE)
        list(APPEND AVRL_LINK_LIBS "-framework Security" "-framework CoreFoundation")
    else()
        list(APPEND AVRL_LINK_LIBS dl)
    endif()
    message(STATUS "AVRL_WITH_OPENSSL=ON (crypto stdlib enabled)")
else()
    message(STATUS "AVRL_WITH_OPENSSL=OFF (crypto stdlib skipped)")
endif()

target_link_libraries(${HEAD} ${AVRL_LINK_LIBS})

install(TARGETS ${HEAD}_static ARCHIVE DESTINATION lib)
