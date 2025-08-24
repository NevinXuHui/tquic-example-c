/**
 * 分层 WebSocket 客户端主实现
 * 
 * 整合所有层次，提供统一的客户端 API：
 * - 协调各层之间的交互
 * - 提供简化的客户端接口
 * - 管理客户端生命周期
 * - 处理状态转换和事件分发
 */

#include "layered_websocket_client.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <inttypes.h>
#include <cjson/cJSON.h>

// 分层 WebSocket 客户端结构体
struct layered_websocket_client {
    client_config_t config;
    client_event_callback_t callback;
    void *user_data;
    
    // 各层组件
    ws_connection_t *ws_conn;
    message_handler_t *msg_handler;
    business_logic_t *business_logic;
    event_system_t *event_system;
    
    // 事件循环
    struct ev_loop *loop;
    ev_timer heartbeat_timer;
    ev_timer reconnect_timer;
    ev_signal sigint_watcher;
    ev_signal sigterm_watcher;
    
    // 状态管理
    client_state_t state;
    client_stats_t stats;
    
    // 线程安全
    pthread_mutex_t mutex;
    
    // 运行控制
    bool running;
    bool should_reconnect;
    uint32_t reconnect_attempts;
};

// 默认配置
client_config_t layered_client_config_default(void) {
    client_config_t config = {
        .host = "localhost",
        .port = "4433",
        .path = "/websocket",
        .origin = NULL,
        .client_id = "layered_client",
        .client_version = "1.0.0",
        .user_agent = "LayeredWebSocketClient/1.0",
        .connect_timeout_ms = 10000,
        .response_timeout_ms = 10000,
        .heartbeat_interval_ms = 30000,
        .auto_reconnect = true,
        .max_reconnect_attempts = 5,
        .reconnect_delay_ms = 1000,
        .reconnect_backoff_factor = 2,
        .max_message_size = 1024 * 1024,
        .message_queue_size = 1000,
        .enable_compression = false,
        .enable_encryption = false,
        .enable_logging = true,
        .log_level = "info",
        .log_file = NULL,
        .worker_threads = 2,
        .enable_priority_queue = true,
        .buffer_size = 8192
    };
    return config;
}

// 状态转换为字符串
const char *client_state_to_string(client_state_t state) {
    switch (state) {
        case CLIENT_STATE_DISCONNECTED: return "DISCONNECTED";
        case CLIENT_STATE_CONNECTING: return "CONNECTING";
        case CLIENT_STATE_CONNECTED: return "CONNECTED";
        case CLIENT_STATE_RECONNECTING: return "RECONNECTING";
        case CLIENT_STATE_SHUTTING_DOWN: return "SHUTTING_DOWN";
        case CLIENT_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

// 验证客户端配置
bool validate_client_config(const client_config_t *config, char **error_msg) {
    if (!config) {
        if (error_msg) *error_msg = strdup("Config is NULL");
        return false;
    }
    
    if (!config->host || strlen(config->host) == 0) {
        if (error_msg) *error_msg = strdup("Host is required");
        return false;
    }
    
    if (!config->port || strlen(config->port) == 0) {
        if (error_msg) *error_msg = strdup("Port is required");
        return false;
    }
    
    if (config->max_message_size == 0) {
        if (error_msg) *error_msg = strdup("Max message size must be > 0");
        return false;
    }
    
    return true;
}

// 打印客户端统计信息
void print_client_stats(const client_stats_t *stats) {
    if (!stats) return;
    
    printf("=== Client Statistics ===\n");
    printf("Current State: %s\n", client_state_to_string(stats->current_state));
    printf("Uptime: %" PRIu64 " ms\n", stats->uptime_ms);
    printf("Total Connections: %" PRIu64 "\n", stats->total_connections);
    printf("Successful Connections: %" PRIu64 "\n", stats->successful_connections);
    printf("Failed Connections: %" PRIu64 "\n", stats->failed_connections);
    printf("Reconnections: %" PRIu64 "\n", stats->reconnections);
    printf("Messages Sent: %" PRIu64 "\n", stats->messages_sent);
    printf("Messages Received: %" PRIu64 "\n", stats->messages_received);
    printf("Bytes Sent: %" PRIu64 "\n", stats->bytes_sent);
    printf("Bytes Received: %" PRIu64 "\n", stats->bytes_received);
    printf("Errors: %" PRIu64 "\n", stats->errors_count);
    printf("Avg Response Time: %.2f ms\n", stats->avg_response_time_ms);
    
    if (stats->last_connected_at > 0) {
        char *time_str = format_timestamp(stats->last_connected_at * 1000);
        printf("Last Connected: %s\n", time_str);
        free(time_str);
    }
    
    if (stats->last_message_at > 0) {
        char *time_str = format_timestamp(stats->last_message_at * 1000);
        printf("Last Message: %s\n", time_str);
        free(time_str);
    }
    
    printf("========================\n");
}

// 心跳定时器回调
static void heartbeat_timer_cb(EV_P_ ev_timer *w, int revents) {
    layered_websocket_client_t *client = (layered_websocket_client_t *)w->data;
    if (client && client->state == CLIENT_STATE_CONNECTED) {
        layered_client_send_heartbeat(client);
    }
}

// 重连定时器回调
static void reconnect_timer_cb(EV_P_ ev_timer *w, int revents) {
    layered_websocket_client_t *client = (layered_websocket_client_t *)w->data;
    if (client && client->should_reconnect) {
        ev_timer_stop(client->loop, &client->reconnect_timer);
        layered_client_reconnect(client);
    }
}

// 信号处理器
static void signal_cb(EV_P_ ev_signal *w, int revents) {
    layered_websocket_client_t *client = (layered_websocket_client_t *)w->data;
    if (client) {
        printf("\nReceived signal %d, shutting down gracefully...\n", w->signum);
        layered_client_stop(client);
    }
}

// WebSocket 事件处理器
static void on_websocket_event(const ws_event_t *event, void *user_data) {
    layered_websocket_client_t *client = (layered_websocket_client_t *)user_data;
    if (!client) return;
    
    pthread_mutex_lock(&client->mutex);
    
    client_state_t old_state = client->state;
    
    switch (event->type) {
        case WS_EVENT_CONNECTED:
            client->state = CLIENT_STATE_CONNECTED;
            client->stats.successful_connections++;
            client->stats.last_connected_at = time(NULL);
            client->reconnect_attempts = 0;
            
            // 启动心跳定时器
            if (client->config.heartbeat_interval_ms > 0) {
                ev_timer_init(&client->heartbeat_timer, heartbeat_timer_cb,
                             client->config.heartbeat_interval_ms / 1000.0,
                             client->config.heartbeat_interval_ms / 1000.0);
                client->heartbeat_timer.data = client;
                ev_timer_start(client->loop, &client->heartbeat_timer);
            }
            break;
            
        case WS_EVENT_DISCONNECTED:
            client->state = CLIENT_STATE_DISCONNECTED;
            ev_timer_stop(client->loop, &client->heartbeat_timer);
            
            // 检查是否需要重连
            if (client->config.auto_reconnect && 
                client->reconnect_attempts < client->config.max_reconnect_attempts &&
                client->running) {
                
                client->state = CLIENT_STATE_RECONNECTING;
                client->should_reconnect = true;
                
                // 启动重连定时器
                double delay = client->config.reconnect_delay_ms / 1000.0;
                for (uint32_t i = 0; i < client->reconnect_attempts; i++) {
                    delay *= client->config.reconnect_backoff_factor;
                }
                
                ev_timer_init(&client->reconnect_timer, reconnect_timer_cb, delay, 0);
                client->reconnect_timer.data = client;
                ev_timer_start(client->loop, &client->reconnect_timer);
            }
            break;
            
        case WS_EVENT_MESSAGE_RECEIVED:
            client->stats.messages_received++;
            client->stats.bytes_received += event->message.length;
            client->stats.last_message_at = time(NULL);
            
            // 转发给消息处理器
            if (client->msg_handler) {
                message_handler_on_websocket_message(client->msg_handler,
                                                   event->message.data,
                                                   event->message.length,
                                                   event->message.frame_type);
            }
            break;
            
        case WS_EVENT_ERROR:
            client->state = CLIENT_STATE_ERROR;
            client->stats.errors_count++;
            break;
            
        default:
            break;
    }
    
    // 触发状态变化事件
    if (old_state != client->state) {
        client_event_t client_event = {
            .type = CLIENT_EVENT_STATE_CHANGED,
            .old_state = old_state,
            .new_state = client->state,
            .timestamp = get_timestamp_ms()
        };
        
        if (client->callback) {
            client->callback(&client_event, client->user_data);
        }
    }
    
    pthread_mutex_unlock(&client->mutex);
}

// 消息事件处理器
static void on_message_event(const message_event_t *event, void *user_data) {
    layered_websocket_client_t *client = (layered_websocket_client_t *)user_data;
    if (!client) return;

    // 处理消息事件
    switch (event->type) {
        case MSG_EVENT_SENT:
            // 消息发送成功
            break;

        case MSG_EVENT_ERROR:
            // 消息发送失败
            break;

        case MSG_EVENT_RECEIVED:
            // 收到消息，转发给业务层
            if (client->business_logic) {
                business_logic_on_message_event(client->business_logic, event);
            }
            break;

        case MSG_EVENT_TIMEOUT:
            // 消息超时
            break;

        case MSG_EVENT_QUEUE_FULL:
            // 队列满
            break;

        case MSG_EVENT_QUEUE_EMPTY:
            // 队列空
            break;

        default:
            break;
    }
}

// 业务事件处理器
static void on_business_event(const business_event_t *event, void *user_data) {
    layered_websocket_client_t *client = (layered_websocket_client_t *)user_data;
    if (!client) return;
    
    // 转换为客户端事件
    client_event_t client_event = {
        .timestamp = event->timestamp
    };
    
    switch (event->type) {
        case BIZ_EVENT_MESSAGE_RECEIVED:
            client_event.type = CLIENT_EVENT_MESSAGE_RECEIVED;
            client_event.message_type = event->message_type;
            client_event.message_data = event->data;
            break;
            
        case BIZ_EVENT_ERROR:
            client_event.type = CLIENT_EVENT_ERROR;
            client_event.error_code = event->error_code;
            client_event.error_description = event->error_description;
            break;
            
        default:
            return; // 不处理其他事件
    }
    
    if (client->callback) {
        client->callback(&client_event, client->user_data);
    }
}

// 创建分层 WebSocket 客户端
layered_websocket_client_t *layered_client_create(const client_config_t *config,
                                                 client_event_callback_t callback,
                                                 void *user_data) {
    if (!config || !callback) return NULL;
    
    char *error_msg = NULL;
    if (!validate_client_config(config, &error_msg)) {
        if (error_msg) {
            fprintf(stderr, "Invalid config: %s\n", error_msg);
            free(error_msg);
        }
        return NULL;
    }
    
    layered_websocket_client_t *client = calloc(1, sizeof(layered_websocket_client_t));
    if (!client) return NULL;
    
    client->config = *config;
    client->callback = callback;
    client->user_data = user_data;
    client->state = CLIENT_STATE_DISCONNECTED;
    client->running = true;
    
    // 初始化互斥锁
    if (pthread_mutex_init(&client->mutex, NULL) != 0) {
        layered_client_destroy(client);
        return NULL;
    }
    
    // 创建事件循环
    client->loop = ev_loop_new(EVFLAG_AUTO);
    if (!client->loop) {
        layered_client_destroy(client);
        return NULL;
    }
    
    // 创建各层组件

    // 1. 事件系统
    printf("正在创建事件系统...\n");
    event_system_config_t event_config = event_system_config_default();
    event_config.worker_thread_count = config->worker_threads;
    event_config.enable_priority_queue = config->enable_priority_queue;
    client->event_system = event_system_create(&event_config);
    if (!client->event_system) {
        printf("❌ 事件系统创建失败\n");
        layered_client_destroy(client);
        return NULL;
    }
    printf("✅ 事件系统创建成功\n");
    
    // 2. WebSocket 协议层
    printf("正在创建 WebSocket 协议层...\n");
    ws_config_t ws_config = ws_config_default();
    ws_config.host = config->host;
    ws_config.port = config->port;
    ws_config.path = config->path;
    ws_config.origin = config->origin;
    ws_config.connect_timeout_ms = config->connect_timeout_ms;
    ws_config.ping_interval_ms = config->heartbeat_interval_ms;
    ws_config.auto_reconnect = false; // 由客户端层管理重连

    client->ws_conn = ws_connection_create(&ws_config, on_websocket_event, client);
    if (!client->ws_conn) {
        printf("❌ WebSocket 协议层创建失败\n");
        layered_client_destroy(client);
        return NULL;
    }
    printf("✅ WebSocket 协议层创建成功\n");
    
    // 3. 消息处理层
    printf("正在创建消息处理层...\n");
    message_handler_config_t msg_config = message_handler_config_default();
    msg_config.max_queue_size = config->message_queue_size;
    msg_config.default_timeout_ms = config->response_timeout_ms;

    client->msg_handler = message_handler_create(&msg_config, on_message_event, client);
    if (!client->msg_handler) {
        printf("❌ 消息处理层创建失败\n");
        layered_client_destroy(client);
        return NULL;
    }
    printf("✅ 消息处理层创建成功\n");
    
    // 4. 业务逻辑层
    printf("正在创建业务逻辑层...\n");
    business_config_t biz_config = business_config_default();
    biz_config.client_id = config->client_id;
    biz_config.client_version = config->client_version;
    biz_config.heartbeat_interval_ms = config->heartbeat_interval_ms;
    biz_config.response_timeout_ms = config->response_timeout_ms;
    biz_config.auto_reconnect = config->auto_reconnect;
    biz_config.enable_logging = config->enable_logging;

    client->business_logic = business_logic_create(&biz_config, on_business_event, client);
    if (!client->business_logic) {
        printf("❌ 业务逻辑层创建失败\n");
        layered_client_destroy(client);
        return NULL;
    }
    printf("✅ 业务逻辑层创建成功\n");
    
    // 连接各层
    ws_connection_set_event_loop(client->ws_conn, client->loop);
    event_system_set_event_loop(client->event_system, client->loop);
    message_handler_set_connection(client->msg_handler, client->ws_conn);
    business_logic_set_message_handler(client->business_logic, client->msg_handler);
    
    // 设置信号处理器
    ev_signal_init(&client->sigint_watcher, signal_cb, SIGINT);
    client->sigint_watcher.data = client;
    ev_signal_start(client->loop, &client->sigint_watcher);
    
    ev_signal_init(&client->sigterm_watcher, signal_cb, SIGTERM);
    client->sigterm_watcher.data = client;
    ev_signal_start(client->loop, &client->sigterm_watcher);
    
    // 初始化统计信息
    client->stats.current_state = client->state;
    
    return client;
}

// 销毁分层 WebSocket 客户端
void layered_client_destroy(layered_websocket_client_t *client) {
    if (!client) return;
    
    // 停止客户端
    layered_client_stop(client);
    
    // 销毁各层组件
    if (client->business_logic) {
        business_logic_destroy(client->business_logic);
    }
    
    if (client->msg_handler) {
        message_handler_destroy(client->msg_handler);
    }
    
    if (client->ws_conn) {
        ws_connection_destroy(client->ws_conn);
    }
    
    if (client->event_system) {
        event_system_destroy(client->event_system);
    }
    
    // 销毁事件循环
    if (client->loop) {
        ev_loop_destroy(client->loop);
    }
    
    // 销毁互斥锁
    pthread_mutex_destroy(&client->mutex);
    
    free(client);
}

// 连接到服务器
int layered_client_connect(layered_websocket_client_t *client) {
    if (!client) return -1;
    
    pthread_mutex_lock(&client->mutex);
    
    if (client->state != CLIENT_STATE_DISCONNECTED) {
        pthread_mutex_unlock(&client->mutex);
        return -1; // 已连接或正在连接
    }
    
    client->state = CLIENT_STATE_CONNECTING;
    client->stats.total_connections++;
    
    pthread_mutex_unlock(&client->mutex);
    
    // 启动事件系统
    event_system_start(client->event_system);
    
    // 连接 WebSocket
    return ws_connection_connect(client->ws_conn);
}

// 断开连接
void layered_client_disconnect(layered_websocket_client_t *client) {
    if (!client) return;
    
    pthread_mutex_lock(&client->mutex);
    client->should_reconnect = false;
    pthread_mutex_unlock(&client->mutex);
    
    // 停止定时器
    if (client->loop) {
        ev_timer_stop(client->loop, &client->heartbeat_timer);
        ev_timer_stop(client->loop, &client->reconnect_timer);
    }
    
    // 关闭 WebSocket 连接
    if (client->ws_conn) {
        ws_connection_close(client->ws_conn, 1000, "Client disconnect");
    }
}

// 运行客户端事件循环
int layered_client_run(layered_websocket_client_t *client) {
    if (!client || !client->loop) return -1;
    
    time_t start_time = time(NULL);
    
    // 运行事件循环
    ev_run(client->loop, 0);
    
    // 更新运行时间统计
    pthread_mutex_lock(&client->mutex);
    client->stats.uptime_ms = (time(NULL) - start_time) * 1000;
    pthread_mutex_unlock(&client->mutex);
    
    return 0;
}

// 停止客户端事件循环
void layered_client_stop(layered_websocket_client_t *client) {
    if (!client) return;
    
    pthread_mutex_lock(&client->mutex);
    client->running = false;
    client->state = CLIENT_STATE_SHUTTING_DOWN;
    pthread_mutex_unlock(&client->mutex);
    
    // 断开连接
    layered_client_disconnect(client);
    
    // 停止事件系统
    if (client->event_system) {
        event_system_stop(client->event_system);
    }
    
    // 停止事件循环
    if (client->loop) {
        ev_break(client->loop, EVBREAK_ALL);
    }
}

// 获取客户端状态
client_state_t layered_client_get_state(const layered_websocket_client_t *client) {
    if (!client) return CLIENT_STATE_ERROR;
    return client->state;
}

// 获取客户端统计信息
const client_stats_t *layered_client_get_stats(const layered_websocket_client_t *client) {
    if (!client) return NULL;
    return &client->stats;
}

// 发送业务请求
int layered_client_send_request(layered_websocket_client_t *client,
                               const char *action,
                               const char *params,
                               char **request_id) {
    if (!client || !client->business_logic) return -1;

    return business_logic_send_request(client->business_logic, action, params, request_id);
}

// 发送通知消息
int layered_client_send_notification(layered_websocket_client_t *client,
                                    const char *type,
                                    const char *data) {
    if (!client || !client->msg_handler) return -1;

    return message_handler_send_notification(client->msg_handler, type, data);
}

// 订阅主题
int layered_client_subscribe(layered_websocket_client_t *client, const char *topic) {
    if (!client || !client->business_logic) return -1;

    return business_logic_subscribe_topic(client->business_logic, topic);
}

// 取消订阅主题
int layered_client_unsubscribe(layered_websocket_client_t *client, const char *topic) {
    if (!client || !client->business_logic) return -1;

    return business_logic_unsubscribe_topic(client->business_logic, topic);
}

// 发布消息到主题
int layered_client_publish(layered_websocket_client_t *client,
                          const char *topic,
                          const char *content) {
    if (!client || !client->msg_handler) return -1;

    // 构建发布消息
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "topic", topic);
    cJSON_AddStringToObject(data, "content", content);
    cJSON_AddNumberToObject(data, "timestamp", get_timestamp_ms());

    char *json_str = cJSON_Print(data);
    cJSON_Delete(data);

    if (!json_str) return -1;

    int result = message_handler_send_notification(client->msg_handler, "publish", json_str);
    free(json_str);

    return result;
}

// 手动触发重连
int layered_client_reconnect(layered_websocket_client_t *client) {
    if (!client) return -1;

    pthread_mutex_lock(&client->mutex);

    if (client->state == CLIENT_STATE_CONNECTED) {
        pthread_mutex_unlock(&client->mutex);
        return 0; // 已连接
    }

    client->reconnect_attempts++;
    client->stats.reconnections++;

    if (client->reconnect_attempts > client->config.max_reconnect_attempts) {
        client->should_reconnect = false;
        pthread_mutex_unlock(&client->mutex);
        return -1; // 超过最大重连次数
    }

    pthread_mutex_unlock(&client->mutex);

    // 重新连接
    return ws_connection_connect(client->ws_conn);
}

// 发送心跳
int layered_client_send_heartbeat(layered_websocket_client_t *client) {
    if (!client || !client->business_logic) return -1;

    return business_logic_send_heartbeat(client->business_logic);
}

// 获取连接延迟（毫秒）
double layered_client_get_latency_ms(const layered_websocket_client_t *client) {
    if (!client) return -1.0;

    // 简化实现：返回平均响应时间
    return client->stats.avg_response_time_ms;
}

// 获取活跃订阅列表
const subscription_t *layered_client_get_subscriptions(const layered_websocket_client_t *client) {
    if (!client || !client->business_logic) return NULL;

    return business_logic_get_subscriptions(client->business_logic);
}

// 设置自动重连
void layered_client_set_auto_reconnect(layered_websocket_client_t *client, bool enable) {
    if (!client) return;

    pthread_mutex_lock(&client->mutex);
    client->config.auto_reconnect = enable;
    pthread_mutex_unlock(&client->mutex);
}

// 导出客户端统计信息为 JSON
char *export_client_stats_json(const client_stats_t *stats) {
    if (!stats) return NULL;

    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;

    cJSON_AddStringToObject(json, "current_state", client_state_to_string(stats->current_state));
    cJSON_AddNumberToObject(json, "uptime_ms", stats->uptime_ms);
    cJSON_AddNumberToObject(json, "total_connections", stats->total_connections);
    cJSON_AddNumberToObject(json, "successful_connections", stats->successful_connections);
    cJSON_AddNumberToObject(json, "failed_connections", stats->failed_connections);
    cJSON_AddNumberToObject(json, "reconnections", stats->reconnections);
    cJSON_AddNumberToObject(json, "messages_sent", stats->messages_sent);
    cJSON_AddNumberToObject(json, "messages_received", stats->messages_received);
    cJSON_AddNumberToObject(json, "bytes_sent", stats->bytes_sent);
    cJSON_AddNumberToObject(json, "bytes_received", stats->bytes_received);
    cJSON_AddNumberToObject(json, "errors_count", stats->errors_count);
    cJSON_AddNumberToObject(json, "avg_response_time_ms", stats->avg_response_time_ms);
    cJSON_AddNumberToObject(json, "last_connected_at", stats->last_connected_at);
    cJSON_AddNumberToObject(json, "last_message_at", stats->last_message_at);

    char *json_string = cJSON_Print(json);
    cJSON_Delete(json);

    return json_string;
}
