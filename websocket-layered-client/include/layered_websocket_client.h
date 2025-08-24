#ifndef LAYERED_WEBSOCKET_CLIENT_H
#define LAYERED_WEBSOCKET_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <ev.h>
#include "websocket_protocol.h"
#include "message_handler.h"
#include "business_logic.h"
#include "event_system.h"

#ifdef __cplusplus
extern "C" {
#endif

// 客户端状态
typedef enum {
    CLIENT_STATE_DISCONNECTED,
    CLIENT_STATE_CONNECTING,
    CLIENT_STATE_CONNECTED,
    CLIENT_STATE_RECONNECTING,
    CLIENT_STATE_SHUTTING_DOWN,
    CLIENT_STATE_ERROR
} client_state_t;

// 分层 WebSocket 客户端
typedef struct layered_websocket_client layered_websocket_client_t;

// 客户端事件类型
typedef enum {
    CLIENT_EVENT_STATE_CHANGED,
    CLIENT_EVENT_MESSAGE_RECEIVED,
    CLIENT_EVENT_MESSAGE_SENT,
    CLIENT_EVENT_ERROR,
    CLIENT_EVENT_RECONNECTED,
    CLIENT_EVENT_SHUTDOWN_COMPLETE
} client_event_type_t;

// 客户端事件数据
typedef struct {
    client_event_type_t type;
    client_state_t old_state;
    client_state_t new_state;
    char *message_type;
    char *message_data;
    int error_code;
    const char *error_description;
    uint64_t timestamp;
} client_event_t;

// 客户端事件回调函数类型
typedef void (*client_event_callback_t)(const client_event_t *event, void *user_data);

// 客户端配置
typedef struct {
    // 连接配置
    const char *host;
    const char *port;
    const char *path;
    const char *origin;
    
    // 客户端信息
    const char *client_id;
    const char *client_version;
    const char *user_agent;
    
    // 超时配置
    uint32_t connect_timeout_ms;
    uint32_t response_timeout_ms;
    uint32_t heartbeat_interval_ms;
    
    // 重连配置
    bool auto_reconnect;
    uint32_t max_reconnect_attempts;
    uint32_t reconnect_delay_ms;
    uint32_t reconnect_backoff_factor;
    
    // 消息配置
    uint32_t max_message_size;
    uint32_t message_queue_size;
    bool enable_compression;
    bool enable_encryption;
    
    // 日志配置
    bool enable_logging;
    const char *log_level;
    const char *log_file;
    
    // 性能配置
    uint32_t worker_threads;
    bool enable_priority_queue;
    uint32_t buffer_size;
} client_config_t;

// 客户端统计信息
typedef struct {
    client_state_t current_state;
    uint64_t uptime_ms;
    uint64_t total_connections;
    uint64_t successful_connections;
    uint64_t failed_connections;
    uint64_t reconnections;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t errors_count;
    double avg_response_time_ms;
    time_t last_connected_at;
    time_t last_message_at;
} client_stats_t;

// 分层 WebSocket 客户端 API

/**
 * 创建分层 WebSocket 客户端
 */
layered_websocket_client_t *layered_client_create(const client_config_t *config,
                                                 client_event_callback_t callback,
                                                 void *user_data);

/**
 * 销毁分层 WebSocket 客户端
 */
void layered_client_destroy(layered_websocket_client_t *client);

/**
 * 连接到服务器
 */
int layered_client_connect(layered_websocket_client_t *client);

/**
 * 断开连接
 */
void layered_client_disconnect(layered_websocket_client_t *client);

/**
 * 发送业务请求
 */
int layered_client_send_request(layered_websocket_client_t *client,
                               const char *action,
                               const char *params,
                               char **request_id);

/**
 * 发送通知消息
 */
int layered_client_send_notification(layered_websocket_client_t *client,
                                    const char *type,
                                    const char *data);

/**
 * 订阅主题
 */
int layered_client_subscribe(layered_websocket_client_t *client, const char *topic);

/**
 * 取消订阅主题
 */
int layered_client_unsubscribe(layered_websocket_client_t *client, const char *topic);

/**
 * 发布消息到主题
 */
int layered_client_publish(layered_websocket_client_t *client,
                          const char *topic,
                          const char *content);

/**
 * 启动客户端事件循环
 */
int layered_client_run(layered_websocket_client_t *client);

/**
 * 停止客户端事件循环
 */
void layered_client_stop(layered_websocket_client_t *client);

/**
 * 获取客户端状态
 */
client_state_t layered_client_get_state(const layered_websocket_client_t *client);

/**
 * 获取客户端统计信息
 */
const client_stats_t *layered_client_get_stats(const layered_websocket_client_t *client);

/**
 * 获取默认配置
 */
client_config_t layered_client_config_default(void);

// 高级功能

/**
 * 设置自定义事件循环
 */
void layered_client_set_event_loop(layered_websocket_client_t *client, struct ev_loop *loop);

/**
 * 注册自定义消息处理器
 */
int layered_client_register_message_handler(layered_websocket_client_t *client,
                                           const char *message_type,
                                           message_event_callback_t handler,
                                           void *user_data);

/**
 * 注销自定义消息处理器
 */
void layered_client_unregister_message_handler(layered_websocket_client_t *client,
                                              const char *message_type);

/**
 * 设置认证信息
 */
int layered_client_set_auth(layered_websocket_client_t *client,
                           const char *username,
                           const char *password,
                           const char *token);

/**
 * 启用/禁用自动重连
 */
void layered_client_set_auto_reconnect(layered_websocket_client_t *client, bool enable);

/**
 * 手动触发重连
 */
int layered_client_reconnect(layered_websocket_client_t *client);

/**
 * 发送心跳
 */
int layered_client_send_heartbeat(layered_websocket_client_t *client);

/**
 * 获取连接延迟（毫秒）
 */
double layered_client_get_latency_ms(const layered_websocket_client_t *client);

/**
 * 获取活跃订阅列表
 */
const subscription_t *layered_client_get_subscriptions(const layered_websocket_client_t *client);

// 工具函数

/**
 * 状态转换为字符串
 */
const char *client_state_to_string(client_state_t state);

/**
 * 验证客户端配置
 */
bool validate_client_config(const client_config_t *config, char **error_msg);

/**
 * 打印客户端统计信息
 */
void print_client_stats(const client_stats_t *stats);

/**
 * 导出客户端统计信息为 JSON
 */
char *export_client_stats_json(const client_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif // LAYERED_WEBSOCKET_CLIENT_H
