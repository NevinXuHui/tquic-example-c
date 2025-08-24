#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "websocket_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// JSON 消息类型
typedef enum {
    MSG_TYPE_REQUEST,
    MSG_TYPE_RESPONSE,
    MSG_TYPE_NOTIFICATION,
    MSG_TYPE_HEARTBEAT,
    MSG_TYPE_ERROR,
    MSG_TYPE_SUBSCRIBE,
    MSG_TYPE_UNSUBSCRIBE,
    MSG_TYPE_PUBLISH,
    MSG_TYPE_CUSTOM
} message_type_t;

// 消息优先级
typedef enum {
    MSG_PRIORITY_LOW = 0,
    MSG_PRIORITY_NORMAL = 1,
    MSG_PRIORITY_HIGH = 2,
    MSG_PRIORITY_URGENT = 3
} message_priority_t;

// JSON 消息结构
typedef struct {
    char *type;              // 消息类型字符串
    char *id;                // 唯一标识符
    uint64_t timestamp;      // 时间戳
    char *data;              // JSON 数据字符串
    message_priority_t priority; // 消息优先级
    uint32_t timeout_ms;     // 超时时间（毫秒）
    uint32_t retry_count;    // 重试次数
} json_message_t;

// 消息队列项
typedef struct message_queue_item {
    json_message_t *message;
    struct message_queue_item *next;
    time_t created_at;
    uint32_t attempts;
} message_queue_item_t;

// 消息处理器
typedef struct message_handler message_handler_t;

// 消息事件类型
typedef enum {
    MSG_EVENT_RECEIVED,
    MSG_EVENT_SENT,
    MSG_EVENT_TIMEOUT,
    MSG_EVENT_ERROR,
    MSG_EVENT_QUEUE_FULL,
    MSG_EVENT_QUEUE_EMPTY
} message_event_type_t;

// 消息事件数据
typedef struct {
    message_event_type_t type;
    json_message_t *message;
    int error_code;
    const char *error_description;
} message_event_t;

// 消息事件回调函数类型
typedef void (*message_event_callback_t)(const message_event_t *event, void *user_data);

// 消息处理器配置
typedef struct {
    uint32_t max_queue_size;        // 最大队列大小
    uint32_t default_timeout_ms;    // 默认超时时间
    uint32_t max_retry_count;       // 最大重试次数
    uint32_t heartbeat_interval_ms; // 心跳间隔
    bool enable_compression;        // 启用压缩
    bool enable_encryption;         // 启用加密
} message_handler_config_t;

// 消息统计信息
typedef struct {
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t messages_timeout;
    uint64_t messages_error;
    uint64_t queue_size;
    uint64_t max_queue_size_reached;
    double avg_response_time_ms;
} message_stats_t;

// 消息处理器 API

/**
 * 创建消息处理器
 */
message_handler_t *message_handler_create(const message_handler_config_t *config,
                                         message_event_callback_t callback,
                                         void *user_data);

/**
 * 销毁消息处理器
 */
void message_handler_destroy(message_handler_t *handler);

/**
 * 设置 WebSocket 连接
 */
void message_handler_set_connection(message_handler_t *handler, ws_connection_t *conn);

/**
 * 发送 JSON 消息
 */
int message_handler_send(message_handler_t *handler, const json_message_t *message);

/**
 * 发送请求消息（等待响应）
 */
int message_handler_send_request(message_handler_t *handler, 
                                const char *type,
                                const char *data,
                                uint32_t timeout_ms,
                                char **response_id);

/**
 * 发送响应消息
 */
int message_handler_send_response(message_handler_t *handler,
                                 const char *request_id,
                                 const char *data,
                                 bool success);

/**
 * 发送通知消息
 */
int message_handler_send_notification(message_handler_t *handler,
                                     const char *type,
                                     const char *data);

/**
 * 订阅主题
 */
int message_handler_subscribe(message_handler_t *handler, const char *topic);

/**
 * 取消订阅主题
 */
int message_handler_unsubscribe(message_handler_t *handler, const char *topic);

/**
 * 发布消息到主题
 */
int message_handler_publish(message_handler_t *handler, 
                           const char *topic, 
                           const char *data);

/**
 * 处理接收到的 WebSocket 消息
 */
void message_handler_on_websocket_message(message_handler_t *handler,
                                         const uint8_t *data,
                                         size_t length,
                                         ws_frame_type_t frame_type);

/**
 * 处理 WebSocket 事件
 */
void message_handler_on_websocket_event(message_handler_t *handler,
                                       const ws_event_t *event);

/**
 * 获取消息统计信息
 */
const message_stats_t *message_handler_get_stats(const message_handler_t *handler);

/**
 * 获取默认配置
 */
message_handler_config_t message_handler_config_default(void);

// JSON 消息工具函数

/**
 * 创建 JSON 消息
 */
json_message_t *json_message_create(const char *type, const char *data);

/**
 * 创建带 ID 的 JSON 消息
 */
json_message_t *json_message_create_with_id(const char *type, 
                                           const char *id, 
                                           const char *data);

/**
 * 销毁 JSON 消息
 */
void json_message_destroy(json_message_t *message);

/**
 * 序列化 JSON 消息为字符串
 */
char *json_message_serialize(const json_message_t *message);

/**
 * 从字符串反序列化 JSON 消息
 */
json_message_t *json_message_deserialize(const char *json_str);

/**
 * 生成唯一 ID
 */
char *generate_unique_id(void);

/**
 * 获取当前时间戳（毫秒）
 */
uint64_t get_timestamp_ms(void);

#ifdef __cplusplus
}
#endif

#endif // MESSAGE_HANDLER_H
