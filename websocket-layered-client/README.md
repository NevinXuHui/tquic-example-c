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
┌─────────────────────────────────────────────────────────┐
│                   应用层 (Application)                    │
│  ┌─────────────────┐  ┌─────────────────┐  ┌──────────┐ │
│  │   聊天客户端     │  │   数据同步客户端  │  │  自定义应用 │ │
│  └─────────────────┘  └─────────────────┘  └──────────┘ │
├─────────────────────────────────────────────────────────┤
│                   业务逻辑层 (Business Logic)             │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  • 业务消息处理  • 订阅/发布  • 认证授权  • 状态管理   │ │
│  └─────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                   消息处理层 (Message Handler)            │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  • JSON 序列化  • 消息路由  • 队列管理  • 超时处理    │ │
│  └─────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                   事件系统层 (Event System)               │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  • 异步事件  • 优先级队列  • 定时器  • 线程安全      │ │
│  └─────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                   协议层 (WebSocket Protocol)            │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  • 帧解析  • 连接管理  • 心跳检测  • 自动重连        │ │
│  └─────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                   传输层 (TQUIC/HTTP3)                   │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  • QUIC 协议  • HTTP/3  • TLS 1.3  • UDP 传输       │ │
│  └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

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
│   └── chat_client.c          # 聊天客户端示例
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

### 运行示例

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

## 💡 核心特性

### 1. 分层架构设计

每一层都有明确的职责，层与层之间通过标准接口通信：

- **协议层**: 处理 WebSocket 协议细节
- **消息层**: 处理 JSON 消息的序列化和路由
- **业务层**: 实现具体的业务逻辑
- **事件层**: 提供异步事件处理机制

### 2. JSON 消息格式

标准化的 JSON 消息格式，支持多种消息类型：

```json
{
  "type": "chat_message",
  "id": "msg_12345",
  "timestamp": 1640995200000,
  "data": {
    "user": "alice",
    "message": "Hello, World!",
    "channel": "general"
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

当前项目提供了完整的架构设计和接口定义，各层的具体实现正在开发中：

- ✅ **架构设计** - 完成
- ✅ **接口定义** - 完成
- ✅ **示例程序** - 完成
- 🚧 **协议层实现** - 开发中
- 🚧 **消息处理层** - 开发中
- 🚧 **业务逻辑层** - 开发中
- 🚧 **事件系统** - 开发中

### 下一步计划

1. 实现 WebSocket 协议层的核心功能
2. 完成 JSON 消息处理和路由机制
3. 实现事件系统的异步处理
4. 添加完整的测试用例
5. 性能优化和稳定性测试

## 📄 许可证

Apache License 2.0

---

**🎉 享受模块化、高性能的 WebSocket 客户端开发体验！**
