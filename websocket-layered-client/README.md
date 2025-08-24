# 分层 WebSocket 客户端

基于 TQUIC 的模块化、可扩展的 WebSocket 客户端架构，实现了协议层与业务逻辑的完全分离。

## 🎯 设计目标

- **分层架构** - 协议层、消息层、业务层完全解耦
- **实时通信** - 基于 JSON 的高性能实时消息传输
- **稳定可靠** - 自动重连、心跳检测、错误恢复
- **扩展性强** - 模块化设计，易于扩展和定制
- **线程安全** - 支持多线程环境下的并发操作

## 🏗️ 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                      应用层 (Application)                        │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐ │
│  │   聊天客户端     │  │  JSON数据客户端  │  │   自定义应用     │ │
│  │  chat_client    │  │  json_client    │  │  custom_app     │ │
│  │                 │  │                 │  │                 │ │
│  │ • 频道管理       │  │ • 结构化消息     │  │ • 业务特定逻辑   │ │
│  │ • 用户交互       │  │ • 多种消息类型   │  │ • 定制化功能     │ │
│  │ • 消息显示       │  │ • 实时数据交换   │  │ • 扩展接口       │ │
│  └─────────────────┘  └─────────────────┘  └─────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│                    业务逻辑层 (Business Logic)                   │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │  • 业务消息处理  • 订阅/发布  • 认证授权  • 状态管理       │ │
│  │  • 消息路由     • 事件分发   • 错误处理  • 统计收集       │ │
│  └───────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│                    消息处理层 (Message Handler)                  │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │  • JSON 序列化  • 消息路由  • 队列管理  • 超时处理        │ │
│  │  • 消息验证     • 格式转换  • 缓存管理  • 重试机制        │ │
│  └───────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│                    事件系统层 (Event System)                     │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │  • 异步事件     • 优先级队列  • 定时器    • 线程安全      │ │
│  │  • 事件调度     • 回调管理   • 内存池    • 性能监控      │ │
│  └───────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│                   协议层 (WebSocket Protocol)                    │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │  • 帧解析       • 连接管理   • 心跳检测  • 自动重连      │ │
│  │  • 握手处理     • 数据压缩   • 流控制    • 错误恢复      │ │
│  └───────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│                    传输层 (TQUIC/HTTP3)                          │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │  • QUIC 协议    • HTTP/3     • TLS 1.3   • UDP 传输      │ │
│  │  • 多路复用     • 0-RTT      • 拥塞控制  • 连接迁移      │ │
│  └───────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### 🎯 应用层示例说明

- **聊天客户端 (chat_client)**
  - 面向用户交互的实时聊天应用
  - 支持频道订阅、消息发送、用户管理
  - 展示了完整的用户界面和交互逻辑

- **JSON 数据客户端 (json_client)**
  - 面向开发者的数据交换工具
  - 支持多种消息类型和自定义 JSON 数据
  - 展示了结构化数据处理和 API 调用模式

## 📁 项目结构

```
websocket-layered-client/
├── include/                    # 头文件
│   ├── websocket_protocol.h    # WebSocket 协议层
│   ├── message_handler.h       # 消息处理层
│   ├── business_logic.h        # 业务逻辑层
│   ├── event_system.h          # 事件系统层
│   └── layered_websocket_client.h  # 主客户端
├── src/                        # 源文件
│   ├── websocket_protocol.c
│   ├── message_handler.c
│   ├── business_logic.c
│   ├── event_system.c
│   └── layered_websocket_client.c
├── examples/                   # 示例程序
│   ├── chat_client.c          # 聊天客户端示例
│   └── json_client.c          # JSON 数据交换示例
├── tests/                      # 测试程序
├── CMakeLists.txt             # CMake 构建文件
└── README.md                  # 项目文档
```

## 🚀 快速开始

### 环境要求

```bash
# Ubuntu/Debian
sudo apt install build-essential cmake pkg-config
sudo apt install libev-dev libcjson-dev libssl-dev

# CentOS/RHEL
sudo yum groupinstall "Development Tools"
sudo yum install cmake libev-devel libcjson-devel openssl-devel
```

### 编译构建

#### 方法一：使用构建脚本（推荐）

```bash
# 使用自动化构建脚本
cd websocket-layered-client
./build.sh

# 构建脚本会自动：
# 1. 检查和安装依赖
# 2. 配置 CMake
# 3. 并行编译
# 4. 显示构建结果
```

#### 方法二：手动构建

```bash
# 1. 确保 TQUIC 库已构建
cd ../deps/tquic
cargo build --release -F ffi

# 2. 构建分层客户端
cd ../websocket-layered-client
mkdir build && cd build
cmake ..
make -j$(nproc)
```

#### 构建选项

```bash
# 构建时可以指定选项
cmake -DBUILD_EXAMPLES=ON -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release ..

# 或使用构建脚本的选项
./build.sh --build-type Debug --enable-tests --verbose
```

### 运行示例

#### 1. 聊天客户端示例

```bash
# 启动聊天客户端
./bin/chat_client 127.0.0.1 4433 myusername

# 可用命令：
# /help          - 显示帮助
# /join <频道>   - 加入频道
# /leave <频道>  - 离开频道
# /list          - 列出已订阅频道
# /stats         - 显示统计信息
# /quit          - 退出程序
```

#### 2. JSON 数据交换客户端示例

```bash
# 启动 JSON 客户端
./bin/json_client 127.0.0.1 4433 json_user

# 可用命令：
# help                    - 显示帮助信息
# text <内容>            - 发送文本消息
# notify <内容>          - 发送通知消息
# request <内容>         - 发送请求消息
# heartbeat              - 发送心跳消息
# subscribe <主题>       - 订阅主题
# publish <主题> <内容>  - 发布消息到主题
# json <JSON字符串>      - 发送自定义JSON消息
# status                 - 显示客户端状态
# quit/exit              - 退出程序

# 示例用法：
# text Hello World!
# notify 系统维护通知
# subscribe news
# publish chat 大家好！
# json {"action": "login", "user_id": 123}
```

## 💡 核心特性

### 1. 分层架构设计

每一层都有明确的职责，层与层之间通过标准接口通信：

- **应用层**: 具体的应用程序实现
  - `chat_client` - 交互式聊天客户端，支持频道管理和用户交互
  - `json_client` - JSON 数据交换客户端，展示结构化消息处理
  - 可扩展支持更多自定义应用
- **业务逻辑层**: 实现具体的业务逻辑和消息处理
- **消息处理层**: 处理 JSON 消息的序列化、反序列化和路由
- **事件系统层**: 提供异步事件处理和调度机制
- **协议层**: 处理 WebSocket 协议细节和连接管理
- **传输层**: 基于 TQUIC 的高性能网络传输

### 2. JSON 消息格式

标准化的 JSON 消息格式，支持多种消息类型：

```json
{
  "type": "text",
  "id": "msg_1756023464_1",
  "timestamp": 1756023464435,
  "priority": 1,
  "data": "Hello from JSON client!"
}
```

支持的消息类型：
- **text** - 普通文本消息
- **notification** - 通知消息（高优先级）
- **request** - 请求消息
- **response** - 响应消息
- **heartbeat** - 心跳消息
- **subscribe** - 订阅消息
- **publish** - 发布消息
- **custom** - 自定义消息

复杂数据结构示例：
```json
{
  "type": "custom",
  "id": "msg_1756023580_5",
  "timestamp": 1756023580670,
  "priority": 1,
  "data": {
    "action": "user_login",
    "user_id": 12345,
    "session_token": "abc123",
    "permissions": ["read", "write"]
  }
}
```

### 3. 事件驱动架构

基于事件的异步处理模型：

```c
// 注册事件处理器
layered_client_register_message_handler(client, "chat_message", 
                                       on_chat_message, user_data);

// 事件处理函数
void on_chat_message(const message_event_t *event, void *user_data) {
    // 处理聊天消息
    printf("收到消息: %s\n", event->message->data);
}
```

### 4. 自动重连和心跳

内置的连接管理机制：

```c
client_config_t config = layered_client_config_default();
config.auto_reconnect = true;
config.max_reconnect_attempts = 5;
config.heartbeat_interval_ms = 30000;  // 30秒心跳
```

### 5. 订阅/发布模式

支持主题订阅和消息发布：

```c
// 订阅主题
layered_client_subscribe(client, "user_notifications");

// 发布消息
layered_client_publish(client, "chat_general", "Hello everyone!");
```

## 📋 JSON 客户端详细使用

JSON 客户端示例展示了如何使用分层 WebSocket 客户端进行结构化的 JSON 数据交换。

### 🎯 功能特性

- **多种消息类型** - 支持文本、通知、请求、发布等多种消息
- **结构化数据** - 自动 JSON 序列化和反序列化
- **实时显示** - 接收到的消息实时解析并格式化显示
- **交互式命令** - 简单易用的命令行界面
- **完整示例** - 展示了分层架构的完整使用流程

### 📝 消息示例

#### 发送文本消息
```bash
> text Hello World!
[发送] {
  "type": "text",
  "id": "msg_1756023464_1",
  "timestamp": 1756023464435,
  "priority": 1,
  "data": "Hello World!"
}
```

#### 发送通知消息
```bash
> notify 系统维护通知：服务器将在30分钟后重启
[发送] {
  "type": "notification",
  "id": "msg_1756023557_4",
  "timestamp": 1756023557196,
  "priority": 2,
  "data": "系统维护通知：服务器将在30分钟后重启"
}
```

#### 发送自定义 JSON
```bash
> json {"action": "user_login", "user_id": 12345, "permissions": ["read", "write"]}
[发送] {
  "type": "custom",
  "id": "msg_1756023580_5",
  "timestamp": 1756023580670,
  "priority": 1,
  "data": {
    "action": "user_login",
    "user_id": 12345,
    "permissions": ["read", "write"]
  }
}
```

#### 接收消息显示
```
=== 接收到 JSON 消息 ===
类型: custom
ID: msg_1756023580_5
时间: Sun Aug 24 16:19:40 2025
优先级: 1
数据: {
  "action": "user_login",
  "user_id": 12345,
  "permissions": ["read", "write"]
}
原始消息: {...}
========================
```

### 🔧 技术实现

JSON 客户端展示了以下技术要点：

1. **cJSON 集成** - 使用 cJSON 库进行 JSON 处理
2. **消息 ID 生成** - 自动生成唯一消息标识符
3. **时间戳处理** - 毫秒级时间戳生成和显示
4. **优先级管理** - 不同消息类型的优先级设置
5. **错误处理** - JSON 格式验证和错误提示
6. **线程安全输出** - 多线程环境下的安全输出

## 🔧 API 使用示例

### 基本使用

```c
#include "layered_websocket_client.h"

// 事件处理器
void on_client_event(const client_event_t *event, void *user_data) {
    switch (event->type) {
        case CLIENT_EVENT_CONNECTED:
            printf("连接成功\n");
            break;
        case CLIENT_EVENT_MESSAGE_RECEIVED:
            printf("收到消息: %s\n", event->message_data);
            break;
        default:
            break;
    }
}

int main() {
    // 创建配置
    client_config_t config = layered_client_config_default();
    config.host = "127.0.0.1";
    config.port = "4433";
    config.auto_reconnect = true;
    
    // 创建客户端
    layered_websocket_client_t *client = 
        layered_client_create(&config, on_client_event, NULL);
    
    // 连接并运行
    layered_client_connect(client);
    layered_client_run(client);
    
    // 清理
    layered_client_destroy(client);
    return 0;
}
```

### 高级功能

```c
// 发送业务请求
char *request_id;
layered_client_send_request(client, "get_user_info", 
                           "{\"user_id\": 123}", &request_id);

// 订阅主题
layered_client_subscribe(client, "stock_prices");

// 发布消息
layered_client_publish(client, "notifications", 
                      "{\"type\": \"alert\", \"message\": \"System maintenance\"}");

// 获取统计信息
const client_stats_t *stats = layered_client_get_stats(client);
printf("发送消息数: %llu\n", stats->messages_sent);
printf("平均响应时间: %.2f ms\n", stats->avg_response_time_ms);
```

## 🧪 测试

```bash
# 运行所有测试
make test

# 运行特定测试
./tests/test_websocket_protocol
./tests/test_message_handler
./tests/test_event_system
```

## 📊 性能特性

- **低延迟**: 基于 QUIC 协议的快速传输
- **高并发**: 事件驱动的异步处理
- **内存效率**: 零拷贝和对象池技术
- **可扩展**: 模块化设计支持水平扩展

## 🔒 安全特性

- **TLS 1.3 加密**: 端到端安全传输
- **消息验证**: JSON 格式验证和数据校验
- **连接认证**: 支持多种认证机制
- **错误隔离**: 各层独立的错误处理

## 🤝 扩展开发

### 添加自定义消息类型

1. 在业务逻辑层注册新的消息处理器
2. 实现消息构建和解析函数
3. 添加相应的事件处理逻辑

### 集成新的传输协议

1. 实现协议层接口
2. 添加协议特定的配置选项
3. 更新消息处理逻辑

## 🔄 开发状态

项目已完成完整的分层架构实现，所有核心功能均已可用：

- ✅ **架构设计** - 完成
- ✅ **接口定义** - 完成
- ✅ **协议层实现** - 完成（WebSocket over HTTP/3）
- ✅ **消息处理层** - 完成（JSON 序列化/反序列化）
- ✅ **业务逻辑层** - 完成（消息路由和处理）
- ✅ **事件系统** - 完成（异步事件处理）
- ✅ **示例程序** - 完成（聊天客户端 + JSON 客户端）
- ✅ **TQUIC 集成** - 完成（真正的 QUIC 连接）
- ✅ **双向通信** - 完成（发送和接收消息）

### 🎉 已实现的核心功能

1. **完整的分层架构** - 四层架构完美运行
2. **TQUIC/HTTP3 支持** - 基于真正的 QUIC 协议
3. **WebSocket 协议** - 完整的握手、帧解析、消息传输
4. **JSON 消息处理** - 结构化消息的发送和接收
5. **事件驱动架构** - 异步事件处理机制
6. **自动重连机制** - 连接断开后自动重连
7. **心跳检测** - 保持连接活跃状态
8. **订阅/发布模式** - 主题订阅和消息发布
9. **多种消息类型** - 文本、通知、请求、响应等
10. **线程安全** - 支持多线程环境

### 🚀 性能表现

- **连接建立** - 快速 QUIC 握手和 WebSocket 升级
- **消息传输** - 低延迟的双向通信
- **JSON 处理** - 高效的序列化和反序列化
- **内存管理** - 合理的内存使用和释放
- **错误处理** - 完善的错误恢复机制

### 📈 下一步计划

1. 添加更多示例程序（文件传输、实时数据同步等）
2. 完善测试用例覆盖
3. 性能优化和压力测试
4. 添加更多认证和安全特性
5. 支持更多消息格式（Protocol Buffers、MessagePack 等）

## 📄 许可证

Apache License 2.0

---

**🎉 享受模块化、高性能的 WebSocket 客户端开发体验！**
