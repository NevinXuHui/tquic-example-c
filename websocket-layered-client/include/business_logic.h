#ifndef BUSINESS_LOGIC_H
#define BUSINESS_LOGIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "message_handler.h"
#include "websocket_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// 业务事件类型
typedef enum {
    BIZ_EVENT_CONNECTED,
    BIZ_EVENT_DISCONNECTED,
    BIZ_EVENT_MESSAGE_RECEIVED,
    BIZ_EVENT_RESPONSE_RECEIVED,
    BIZ_EVENT_NOTIFICATION_RECEIVED,
    BIZ_EVENT_SUBSCRIPTION_CONFIRMED,
    BIZ_EVENT_SUBSCRIPTION_FAILED,
    BIZ_EVENT_PUBLISH_SUCCESS,
    BIZ_EVENT_PUBLISH_FAILED,
    BIZ_EVENT_ERROR,
    BIZ_EVENT_HEARTBEAT_TIMEOUT,
    BIZ_EVENT_RECONNECTED
} business_event_type_t;

// 业务事件数据
typedef struct {
    business_event_type_t type;
    char *message_type;
    char *message_id;
    char *data;
    int error_code;
    const char *error_description;
    uint64_t timestamp;
} business_event_t;

// 业务逻辑处理器
typedef struct business_logic business_logic_t;

// 业务事件回调函数类型
typedef void (*business_event_callback_t)(const business_event_t *event, void *user_data);

// 业务逻辑配置
typedef struct {
    const char *client_id;           // 客户端标识
    const char *client_version;      // 客户端版本
    uint32_t heartbeat_interval_ms;  // 心跳间隔
    uint32_t response_timeout_ms;    // 响应超时时间
    bool auto_reconnect;             // 自动重连
    uint32_t max_reconnect_attempts; // 最大重连次数
    uint32_t reconnect_delay_ms;     // 重连延迟
    bool enable_logging;             // 启用日志
} business_config_t;

// 订阅信息
typedef struct subscription {
    char *topic;
    bool active;
    uint64_t subscribed_at;
    uint64_t message_count;
    struct subscription *next;
} subscription_t;

// 业务统计信息
typedef struct {
    uint64_t requests_sent;
    uint64_t responses_received;
    uint64_t notifications_received;
    uint64_t subscriptions_active;
    uint64_t publications_sent;
    uint64_t heartbeats_sent;
    uint64_t reconnections;
    double avg_response_time_ms;
    time_t session_start_time;
} business_stats_t;

// 业务逻辑 API

/**
 * 创建业务逻辑处理器
 */
business_logic_t *business_logic_create(const business_config_t *config,
                                       business_event_callback_t callback,
                                       void *user_data);

/**
 * 销毁业务逻辑处理器
 */
void business_logic_destroy(business_logic_t *logic);

/**
 * 设置消息处理器
 */
void business_logic_set_message_handler(business_logic_t *logic, 
                                       message_handler_t *handler);

/**
 * 连接到服务器
 */
int business_logic_connect(business_logic_t *logic, 
                          const char *host, 
                          const char *port);

/**
 * 断开连接
 */
void business_logic_disconnect(business_logic_t *logic);

/**
 * 发送业务请求
 */
int business_logic_send_request(business_logic_t *logic,
                               const char *action,
                               const char *params,
                               char **request_id);

/**
 * 订阅主题
 */
int business_logic_subscribe_topic(business_logic_t *logic, const char *topic);

/**
 * 取消订阅主题
 */
int business_logic_unsubscribe_topic(business_logic_t *logic, const char *topic);

/**
 * 发布消息
 */
int business_logic_publish_message(business_logic_t *logic,
                                  const char *topic,
                                  const char *content);

/**
 * 发送心跳
 */
int business_logic_send_heartbeat(business_logic_t *logic);

/**
 * 处理消息事件
 */
void business_logic_on_message_event(business_logic_t *logic,
                                    const message_event_t *event);

/**
 * 获取业务统计信息
 */
const business_stats_t *business_logic_get_stats(const business_logic_t *logic);

/**
 * 获取活跃订阅列表
 */
const subscription_t *business_logic_get_subscriptions(const business_logic_t *logic);

/**
 * 获取默认配置
 */
business_config_t business_config_default(void);

// 业务消息构建器

/**
 * 构建认证请求
 */
char *build_auth_request(const char *username, const char *password, const char *token);

/**
 * 构建数据查询请求
 */
char *build_query_request(const char *query_type, const char *parameters);

/**
 * 构建数据更新请求
 */
char *build_update_request(const char *resource_id, const char *data);

/**
 * 构建订阅请求
 */
char *build_subscribe_request(const char *topic, const char *filters);

/**
 * 构建发布请求
 */
char *build_publish_request(const char *topic, const char *content, const char *metadata);

/**
 * 构建心跳请求
 */
char *build_heartbeat_request(const char *client_id, uint64_t timestamp);

// 业务消息解析器

/**
 * 解析认证响应
 */
bool parse_auth_response(const char *response, char **session_id, char **error_msg);

/**
 * 解析查询响应
 */
bool parse_query_response(const char *response, char **result_data, char **error_msg);

/**
 * 解析订阅确认
 */
bool parse_subscription_response(const char *response, char **topic, bool *success);

/**
 * 解析通知消息
 */
bool parse_notification(const char *notification, char **topic, char **content, uint64_t *timestamp);

/**
 * 解析错误消息
 */
bool parse_error_message(const char *error_msg, int *error_code, char **description);

// 工具函数

/**
 * 验证消息格式
 */
bool validate_message_format(const char *message);

/**
 * 生成客户端 ID
 */
char *generate_client_id(const char *prefix);

/**
 * 格式化时间戳
 */
char *format_timestamp(uint64_t timestamp);

#ifdef __cplusplus
}
#endif

#endif // BUSINESS_LOGIC_H
