cmake_minimum_required(VERSION 3.10)
project(tquic-websocket-server VERSION 1.0.0 LANGUAGES C)

# 设置 C 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

# 默认构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 查找依赖库
find_package(PkgConfig REQUIRED)

# 查找 libev
pkg_check_modules(LIBEV libev)
if(NOT LIBEV_FOUND)
    find_library(LIBEV_LIBRARIES ev)
    if(NOT LIBEV_LIBRARIES)
        message(FATAL_ERROR "libev not found")
    endif()
    set(LIBEV_INCLUDE_DIRS "")
endif()

# 查找 cJSON
pkg_check_modules(CJSON libcjson)
if(NOT CJSON_FOUND)
    find_library(CJSON_LIBRARIES cjson)
    if(NOT CJSON_LIBRARIES)
        message(FATAL_ERROR "libcjson not found")
    endif()
    set(CJSON_INCLUDE_DIRS "")
endif()

# 查找 OpenSSL
find_package(OpenSSL REQUIRED)

# TQUIC 库和头文件路径
set(TQUIC_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../deps/tquic/target/release/libtquic.a")
set(TQUIC_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../deps/tquic/include")
if(NOT EXISTS ${TQUIC_LIB_PATH})
    message(FATAL_ERROR "TQUIC library not found at ${TQUIC_LIB_PATH}")
endif()
if(NOT EXISTS ${TQUIC_INCLUDE_PATH})
    message(FATAL_ERROR "TQUIC headers not found at ${TQUIC_INCLUDE_PATH}")
endif()

# 包含目录
include_directories(${TQUIC_INCLUDE_PATH})
include_directories(${LIBEV_INCLUDE_DIRS})
include_directories(${CJSON_INCLUDE_DIRS})
include_directories(${OPENSSL_INCLUDE_DIR})

# 创建可执行文件
add_executable(tquic-websocket-server src/tquic_websocket_server.c)

# 链接库
target_link_libraries(tquic-websocket-server
    ${TQUIC_LIB_PATH}
    ${LIBEV_LIBRARIES}
    ${CJSON_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    pthread
    dl
    m
)

# 安装配置
install(TARGETS tquic-websocket-server
    RUNTIME DESTINATION bin
)

# 安装配置文件
install(FILES config/server.conf
    DESTINATION /etc/tquic-websocket-server
    OPTIONAL
)

# 安装 systemd 服务文件
install(FILES scripts/tquic-websocket-server.service
    DESTINATION /etc/systemd/system
    OPTIONAL
)

# 创建日志目录
install(DIRECTORY DESTINATION /var/log/tquic-websocket-server)

# 打印配置信息
message(STATUS "")
message(STATUS "TQUIC WebSocket Server Configuration Summary:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  TQUIC library: ${TQUIC_LIB_PATH}")
message(STATUS "  libev: ${LIBEV_LIBRARIES}")
message(STATUS "  cJSON: ${CJSON_LIBRARIES}")
message(STATUS "  OpenSSL: ${OPENSSL_LIBRARIES}")
message(STATUS "")
