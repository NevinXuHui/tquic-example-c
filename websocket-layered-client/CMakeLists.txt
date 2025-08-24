cmake_minimum_required(VERSION 3.16)

# 项目配置
project(layered-websocket-client
    VERSION 1.0.0
    DESCRIPTION "Layered WebSocket Client - Modular and extensible WebSocket client architecture"
    LANGUAGES C
)

# 设置 C 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 编译器标志
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror -pedantic")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG -fsanitize=address -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

# 选项
option(BUILD_EXAMPLES "Build example applications" ON)
option(BUILD_TESTS "Build test programs" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# 查找依赖库
find_package(PkgConfig REQUIRED)

# 查找 libev
pkg_check_modules(LIBEV libev)
if(NOT LIBEV_FOUND)
    # 手动查找 libev
    find_path(LIBEV_INCLUDE_DIR ev.h)
    find_library(LIBEV_LIBRARY ev)
    if(LIBEV_INCLUDE_DIR AND LIBEV_LIBRARY)
        set(LIBEV_FOUND TRUE)
        set(LIBEV_INCLUDE_DIRS ${LIBEV_INCLUDE_DIR})
        set(LIBEV_LIBRARIES ${LIBEV_LIBRARY})
        message(STATUS "Found libev manually: ${LIBEV_LIBRARY}")
    else()
        message(FATAL_ERROR "libev not found. Please install libev-dev")
    endif()
endif()

# 查找 cJSON
pkg_check_modules(CJSON libcjson)
if(NOT CJSON_FOUND)
    # 手动查找 cJSON
    find_path(CJSON_INCLUDE_DIR cjson/cJSON.h)
    find_library(CJSON_LIBRARY cjson)
    if(CJSON_INCLUDE_DIR AND CJSON_LIBRARY)
        set(CJSON_FOUND TRUE)
        set(CJSON_INCLUDE_DIRS ${CJSON_INCLUDE_DIR})
        set(CJSON_LIBRARIES ${CJSON_LIBRARY})
        message(STATUS "Found cJSON manually: ${CJSON_LIBRARY}")
    else()
        message(FATAL_ERROR "cJSON not found. Please install libcjson-dev")
    endif()
endif()

# 查找 pthread
find_package(Threads REQUIRED)

# 查找 OpenSSL
find_package(OpenSSL REQUIRED)

# TQUIC 库配置
set(TQUIC_DIR "${CMAKE_SOURCE_DIR}/../deps/tquic")
set(TQUIC_LIB_DIR "${TQUIC_DIR}/target/release")
set(TQUIC_INCLUDE_DIR "${TQUIC_DIR}/include")
set(BORINGSSL_INCLUDE_DIR "${TQUIC_DIR}/deps/boringssl/src/include")

# 检查 TQUIC 库是否存在
set(TQUIC_LIB_PATH "${TQUIC_LIB_DIR}/libtquic.a")
if(NOT EXISTS ${TQUIC_LIB_PATH})
    message(WARNING "TQUIC library not found at ${TQUIC_LIB_PATH}")
    message(STATUS "Creating stub TQUIC library for compilation")
    # 创建一个空的存根库用于编译测试
    file(WRITE "${CMAKE_BINARY_DIR}/stub_tquic.c" "// Stub TQUIC library\n")
    add_library(tquic STATIC "${CMAKE_BINARY_DIR}/stub_tquic.c")
    target_include_directories(tquic INTERFACE ${CMAKE_SOURCE_DIR}/include)
else()
    # 创建 TQUIC 导入库
    add_library(tquic STATIC IMPORTED)
    set_target_properties(tquic PROPERTIES
        IMPORTED_LOCATION ${TQUIC_LIB_PATH}
        INTERFACE_INCLUDE_DIRECTORIES "${TQUIC_INCLUDE_DIR};${BORINGSSL_INCLUDE_DIR}"
    )
endif()

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${TQUIC_INCLUDE_DIR}
    ${BORINGSSL_INCLUDE_DIR}
    ${LIBEV_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
)

# 源文件
set(WEBSOCKET_PROTOCOL_SOURCES
    src/websocket_protocol.c
)

set(MESSAGE_HANDLER_SOURCES
    src/message_handler.c
)

set(BUSINESS_LOGIC_SOURCES
    src/business_logic.c
)

set(EVENT_SYSTEM_SOURCES
    src/event_system.c
)

set(LAYERED_CLIENT_SOURCES
    src/layered_websocket_client.c
)

# 创建静态库
add_library(websocket_protocol STATIC ${WEBSOCKET_PROTOCOL_SOURCES})
add_library(message_handler STATIC ${MESSAGE_HANDLER_SOURCES})
add_library(business_logic STATIC ${BUSINESS_LOGIC_SOURCES})
add_library(event_system STATIC ${EVENT_SYSTEM_SOURCES})
add_library(layered_client STATIC ${LAYERED_CLIENT_SOURCES})

# 设置库的链接依赖
target_link_libraries(websocket_protocol
    tquic
    ${LIBEV_LIBRARIES}
    ${CMAKE_DL_LIBS}
    Threads::Threads
    m
)

target_link_libraries(message_handler
    websocket_protocol
    ${CJSON_LIBRARIES}
    Threads::Threads
)

target_link_libraries(business_logic
    message_handler
    websocket_protocol
    ${CJSON_LIBRARIES}
    Threads::Threads
)

target_link_libraries(event_system
    ${LIBEV_LIBRARIES}
    Threads::Threads
)

target_link_libraries(layered_client
    business_logic
    message_handler
    websocket_protocol
    event_system
    tquic
    ${LIBEV_LIBRARIES}
    ${CJSON_LIBRARIES}
    ${CMAKE_DL_LIBS}
    Threads::Threads
    m
)

# 设置库的编译定义
target_compile_definitions(websocket_protocol PRIVATE ${LIBEV_CFLAGS_OTHER})
target_compile_definitions(message_handler PRIVATE ${CJSON_CFLAGS_OTHER})
target_compile_definitions(business_logic PRIVATE ${CJSON_CFLAGS_OTHER})
target_compile_definitions(event_system PRIVATE ${LIBEV_CFLAGS_OTHER})

# 创建组合库（可选）
if(BUILD_SHARED_LIBS)
    add_library(layered_websocket_client_all SHARED
        ${WEBSOCKET_PROTOCOL_SOURCES}
        ${MESSAGE_HANDLER_SOURCES}
        ${BUSINESS_LOGIC_SOURCES}
        ${EVENT_SYSTEM_SOURCES}
        ${LAYERED_CLIENT_SOURCES}
    )
    
    target_link_libraries(layered_websocket_client_all
        tquic
        ${LIBEV_LIBRARIES}
        ${CJSON_LIBRARIES}
        ${CMAKE_DL_LIBS}
        Threads::Threads
        m
    )
    
    set_target_properties(layered_websocket_client_all PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )
endif()

# 构建示例程序
if(BUILD_EXAMPLES)
    # 聊天客户端示例
    add_executable(chat_client examples/chat_client.c)
    target_link_libraries(chat_client layered_client)

    # JSON 数据交换客户端示例
    add_executable(json_client examples/json_client.c)
    target_link_libraries(json_client layered_client)

    # 设置输出目录
    set_target_properties(chat_client json_client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()

# 构建测试程序
if(BUILD_TESTS)
    enable_testing()

    # 基础功能测试
    add_executable(test_basic tests/test_basic.c)
    target_link_libraries(test_basic layered_client)
    add_test(NAME basic_test COMMAND test_basic)

    # WebSocket 协议测试（如果存在）
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_websocket_protocol.c")
        add_executable(test_websocket_protocol tests/test_websocket_protocol.c)
        target_link_libraries(test_websocket_protocol websocket_protocol)
        add_test(NAME websocket_protocol_test COMMAND test_websocket_protocol)
    endif()

    # 消息处理器测试（如果存在）
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_message_handler.c")
        add_executable(test_message_handler tests/test_message_handler.c)
        target_link_libraries(test_message_handler message_handler)
        add_test(NAME message_handler_test COMMAND test_message_handler)
    endif()

    # 事件系统测试（如果存在）
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_event_system.c")
        add_executable(test_event_system tests/test_event_system.c)
        target_link_libraries(test_event_system event_system)
        add_test(NAME event_system_test COMMAND test_event_system)
    endif()

    # 设置测试输出目录
    set_target_properties(test_basic PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

    if(TARGET test_websocket_protocol)
        set_target_properties(test_websocket_protocol PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    endif()

    if(TARGET test_message_handler)
        set_target_properties(test_message_handler PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    endif()

    if(TARGET test_event_system)
        set_target_properties(test_event_system PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    endif()
endif()

# 安装配置
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install prefix" FORCE)
endif()

# 安装头文件
install(DIRECTORY include/ DESTINATION include/layered-websocket-client)

# 安装库文件
install(TARGETS 
    websocket_protocol
    message_handler
    business_logic
    event_system
    layered_client
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

if(BUILD_SHARED_LIBS)
    install(TARGETS layered_websocket_client_all
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
    )
endif()

# 安装示例程序
if(BUILD_EXAMPLES)
    install(TARGETS chat_client RUNTIME DESTINATION bin)
endif()

# 安装文档
install(FILES README.md DESTINATION share/doc/layered-websocket-client)

# 打印配置摘要
message(STATUS "")
message(STATUS "Layered WebSocket Client Configuration Summary:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Build examples: ${BUILD_EXAMPLES}")
message(STATUS "  Build tests: ${BUILD_TESTS}")
message(STATUS "  Build shared libs: ${BUILD_SHARED_LIBS}")
message(STATUS "  TQUIC library: ${TQUIC_LIB_PATH}")
message(STATUS "  libev: ${LIBEV_LIBRARIES}")
message(STATUS "  cJSON: ${CJSON_LIBRARIES}")
message(STATUS "")
