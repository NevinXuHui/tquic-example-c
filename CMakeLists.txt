cmake_minimum_required(VERSION 3.16)

# Project configuration
project(tquic-examples
    VERSION 1.0.0
    DESCRIPTION "TQUIC WebSocket Examples - C implementation of WebSocket over HTTP/3"
    LANGUAGES C
)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror -pedantic")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

# Options
option(BUILD_WEBSOCKET_EXAMPLES "Build WebSocket examples" ON)
option(BUILD_SIMPLE_EXAMPLES "Build simple QUIC examples" ON)
option(BUILD_TESTS "Build test programs" OFF)

# TQUIC library configuration
set(TQUIC_DIR "${CMAKE_SOURCE_DIR}/deps/tquic")
set(TQUIC_LIB_DIR "${TQUIC_DIR}/target/release")
set(TQUIC_INCLUDE_DIR "${TQUIC_DIR}/include")
set(BORINGSSL_INCLUDE_DIR "${TQUIC_DIR}/deps/boringssl/src/include")

# Check if TQUIC library exists, if not build it
set(TQUIC_LIB_PATH "${TQUIC_LIB_DIR}/libtquic.a")
if(NOT EXISTS ${TQUIC_LIB_PATH})
    message(STATUS "TQUIC library not found, building...")
    execute_process(
        COMMAND git submodule update --init --recursive
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE GIT_RESULT
    )
    if(NOT GIT_RESULT EQUAL "0")
        message(FATAL_ERROR "Failed to initialize git submodules")
    endif()
    
    execute_process(
        COMMAND cargo build --release -F ffi
        WORKING_DIRECTORY ${TQUIC_DIR}
        RESULT_VARIABLE CARGO_RESULT
    )
    if(NOT CARGO_RESULT EQUAL "0")
        message(FATAL_ERROR "Failed to build TQUIC library")
    endif()
endif()

# Find required system libraries
find_library(LIBEV_LIBRARY ev REQUIRED)
find_library(LIBDL_LIBRARY dl REQUIRED)
find_library(LIBM_LIBRARY m REQUIRED)
find_library(LIBPTHREAD_LIBRARY pthread REQUIRED)

# Create TQUIC imported library
add_library(tquic STATIC IMPORTED)
set_target_properties(tquic PROPERTIES
    IMPORTED_LOCATION ${TQUIC_LIB_PATH}
    INTERFACE_INCLUDE_DIRECTORIES "${TQUIC_INCLUDE_DIR};${BORINGSSL_INCLUDE_DIR}"
)

# System libraries target
add_library(system_libs INTERFACE)
target_link_libraries(system_libs INTERFACE
    ${LIBEV_LIBRARY}
    ${LIBDL_LIBRARY}
    ${LIBM_LIBRARY}
    ${LIBPTHREAD_LIBRARY}
)

# Common compile definitions and include directories
add_library(common_config INTERFACE)
target_include_directories(common_config INTERFACE
    ${CMAKE_SOURCE_DIR}
    ${TQUIC_INCLUDE_DIR}
    ${BORINGSSL_INCLUDE_DIR}
)

# Function to create executable with common configuration
function(add_tquic_executable target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE
        common_config
        tquic
        system_libs
    )
    
    # Set output directory
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endfunction()

# Simple QUIC examples
if(BUILD_SIMPLE_EXAMPLES)
    add_tquic_executable(simple_server simple_server.c)
    add_tquic_executable(simple_client simple_client.c)
    add_tquic_executable(simple_h3_server simple_h3_server.c)
    add_tquic_executable(simple_h3_client simple_h3_client.c)
endif()

# WebSocket examples
if(BUILD_WEBSOCKET_EXAMPLES)
    add_tquic_executable(tquic_websocket_server tquic_websocket_server.c)
    add_tquic_executable(tquic_websocket_client tquic_websocket_client.c)
    add_tquic_executable(tquic_websocket_interactive_client tquic_websocket_interactive_client.c)
endif()

# Custom target to build TQUIC library
add_custom_target(build_tquic
    COMMAND git submodule update --init --recursive
    COMMAND cargo build --release -F ffi
    WORKING_DIRECTORY ${TQUIC_DIR}
    COMMENT "Building TQUIC library"
)

# Custom target to clean TQUIC build
add_custom_target(clean_tquic
    COMMAND cargo clean
    WORKING_DIRECTORY ${TQUIC_DIR}
    COMMENT "Cleaning TQUIC build artifacts"
)

# Install configuration
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install prefix" FORCE)
endif()

# Install targets conditionally
set(INSTALL_TARGETS "")

if(BUILD_SIMPLE_EXAMPLES)
    list(APPEND INSTALL_TARGETS
        simple_server
        simple_client
        simple_h3_server
        simple_h3_client
    )
endif()

if(BUILD_WEBSOCKET_EXAMPLES)
    list(APPEND INSTALL_TARGETS
        tquic_websocket_server
        tquic_websocket_client
        tquic_websocket_interactive_client
    )
endif()

if(INSTALL_TARGETS)
    install(TARGETS ${INSTALL_TARGETS}
        RUNTIME DESTINATION bin
    )
endif()

# Install documentation
install(FILES README.md LICENSE DESTINATION share/doc/tquic-examples)

# Print configuration summary
message(STATUS "")
message(STATUS "TQUIC Examples Configuration Summary:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  C Flags: ${CMAKE_C_FLAGS}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Build simple examples: ${BUILD_SIMPLE_EXAMPLES}")
message(STATUS "  Build WebSocket examples: ${BUILD_WEBSOCKET_EXAMPLES}")
message(STATUS "  Build tests: ${BUILD_TESTS}")
message(STATUS "  TQUIC directory: ${TQUIC_DIR}")
message(STATUS "  TQUIC library: ${TQUIC_LIB_PATH}")
message(STATUS "")
