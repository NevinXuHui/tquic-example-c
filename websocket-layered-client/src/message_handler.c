/**
 * 消息处理层实现
 * 
 * 负责 JSON 消息的序列化、反序列化和路由：
 * - JSON 消息处理
 * - 消息队列管理
 * - 超时和重试机制
 * - 消息路由和分发
 */

#include "message_handler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <cjson/cJSON.h>

// 消息处理器结构体
struct message_handler {
    message_handler_config_t config;
    message_event_callback_t callback;
    void *user_data;
    
    // WebSocket 连接
    ws_connection_t *ws_conn;
    
    // 消息队列
    message_queue_item_t *send_queue_head;
    message_queue_item_t *send_queue_tail;
    uint32_t queue_size;
    
    // 待响应的请求
    message_queue_item_t *pending_requests;
    
    // 统计信息
    message_stats_t stats;
    
    // 线程安全
    pthread_mutex_t mutex;
    pthread_cond_t queue_cond;
    
    // 状态
    bool running;
    pthread_t worker_thread;
};

// 默认配置
message_handler_config_t message_handler_config_default(void) {
    message_handler_config_t config = {
        .max_queue_size = 1000,
        .default_timeout_ms = 30000,
        .max_retry_count = 3,
        .heartbeat_interval_ms = 30000,
        .enable_compression = false,
        .enable_encryption = false
    };
    return config;
}

// 生成唯一 ID
char *generate_unique_id(void) {
    static uint64_t counter = 0;
    char *id = malloc(32);
    if (id) {
        snprintf(id, 32, "msg_%lu_%lu", time(NULL), __sync_add_and_fetch(&counter, 1));
    }
    return id;
}

// 获取当前时间戳（毫秒）
uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 创建 JSON 消息
json_message_t *json_message_create(const char *type, const char *data) {
    return json_message_create_with_id(type, NULL, data);
}

// 创建带 ID 的 JSON 消息
json_message_t *json_message_create_with_id(const char *type, const char *id, const char *data) {
    if (!type || !data) return NULL;
    
    json_message_t *msg = calloc(1, sizeof(json_message_t));
    if (!msg) return NULL;
    
    msg->type = strdup(type);
    msg->id = id ? strdup(id) : generate_unique_id();
    msg->data = strdup(data);
    msg->timestamp = get_timestamp_ms();
    msg->priority = MSG_PRIORITY_NORMAL;
    msg->timeout_ms = 30000;
    msg->retry_count = 0;
    
    if (!msg->type || !msg->id || !msg->data) {
        json_message_destroy(msg);
        return NULL;
    }
    
    return msg;
}

// 销毁 JSON 消息
void json_message_destroy(json_message_t *message) {
    if (!message) return;
    
    free(message->type);
    free(message->id);
    free(message->data);
    free(message);
}

// 序列化 JSON 消息为字符串
char *json_message_serialize(const json_message_t *message) {
    if (!message) return NULL;
    
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    
    cJSON_AddStringToObject(json, "type", message->type);
    cJSON_AddStringToObject(json, "id", message->id);
    cJSON_AddNumberToObject(json, "timestamp", message->timestamp);
    cJSON_AddNumberToObject(json, "priority", message->priority);
    
    // 解析 data 字段
    cJSON *data_json = cJSON_Parse(message->data);
    if (data_json) {
        cJSON_AddItemToObject(json, "data", data_json);
    } else {
        cJSON_AddStringToObject(json, "data", message->data);
    }
    
    char *json_string = cJSON_Print(json);
    cJSON_Delete(json);
    
    return json_string;
}

// 从字符串反序列化 JSON 消息
json_message_t *json_message_deserialize(const char *json_str) {
    if (!json_str) return NULL;
    
    cJSON *json = cJSON_Parse(json_str);
    if (!json) return NULL;
    
    cJSON *type_item = cJSON_GetObjectItem(json, "type");
    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    cJSON *timestamp_item = cJSON_GetObjectItem(json, "timestamp");
    cJSON *data_item = cJSON_GetObjectItem(json, "data");
    cJSON *priority_item = cJSON_GetObjectItem(json, "priority");
    
    if (!cJSON_IsString(type_item) || !cJSON_IsString(id_item)) {
        cJSON_Delete(json);
        return NULL;
    }
    
    json_message_t *msg = calloc(1, sizeof(json_message_t));
    if (!msg) {
        cJSON_Delete(json);
        return NULL;
    }
    
    msg->type = strdup(type_item->valuestring);
    msg->id = strdup(id_item->valuestring);
    
    if (cJSON_IsNumber(timestamp_item)) {
        msg->timestamp = (uint64_t)timestamp_item->valuedouble;
    } else {
        msg->timestamp = get_timestamp_ms();
    }
    
    if (cJSON_IsNumber(priority_item)) {
        msg->priority = (message_priority_t)priority_item->valueint;
    } else {
        msg->priority = MSG_PRIORITY_NORMAL;
    }
    
    // 序列化 data 字段
    if (data_item) {
        msg->data = cJSON_Print(data_item);
    } else {
        msg->data = strdup("{}");
    }
    
    msg->timeout_ms = 30000;
    msg->retry_count = 0;
    
    cJSON_Delete(json);
    
    if (!msg->type || !msg->id || !msg->data) {
        json_message_destroy(msg);
        return NULL;
    }
    
    return msg;
}

// 工作线程函数
static void *worker_thread_func(void *arg) {
    message_handler_t *handler = (message_handler_t *)arg;
    
    while (handler->running) {
        pthread_mutex_lock(&handler->mutex);
        
        // 等待队列中有消息
        while (handler->running && !handler->send_queue_head) {
            pthread_cond_wait(&handler->queue_cond, &handler->mutex);
        }
        
        if (!handler->running) {
            pthread_mutex_unlock(&handler->mutex);
            break;
        }
        
        // 取出队列头部消息
        message_queue_item_t *item = handler->send_queue_head;
        if (item) {
            handler->send_queue_head = item->next;
            if (!handler->send_queue_head) {
                handler->send_queue_tail = NULL;
            }
            handler->queue_size--;
        }
        
        pthread_mutex_unlock(&handler->mutex);
        
        if (item && item->message) {
            // 序列化消息
            char *json_str = json_message_serialize(item->message);
            if (json_str) {
                // 通过 WebSocket 发送
                if (handler->ws_conn) {
                    int result = ws_connection_send_text(handler->ws_conn, json_str, strlen(json_str));
                    
                    // 更新统计信息
                    pthread_mutex_lock(&handler->mutex);
                    if (result == 0) {
                        handler->stats.messages_sent++;
                        
                        // 触发发送成功事件
                        message_event_t event = {
                            .type = MSG_EVENT_SENT,
                            .message = item->message
                        };
                        if (handler->callback) {
                            handler->callback(&event, handler->user_data);
                        }
                    } else {
                        handler->stats.messages_error++;
                        
                        // 触发发送失败事件
                        message_event_t event = {
                            .type = MSG_EVENT_ERROR,
                            .message = item->message,
                            .error_code = result,
                            .error_description = "Failed to send message"
                        };
                        if (handler->callback) {
                            handler->callback(&event, handler->user_data);
                        }
                    }
                    pthread_mutex_unlock(&handler->mutex);
                }
                
                free(json_str);
            }
            
            // 清理消息项
            json_message_destroy(item->message);
            free(item);
        }
    }
    
    return NULL;
}

// 创建消息处理器
message_handler_t *message_handler_create(const message_handler_config_t *config,
                                         message_event_callback_t callback,
                                         void *user_data) {
    if (!config || !callback) return NULL;
    
    message_handler_t *handler = calloc(1, sizeof(message_handler_t));
    if (!handler) return NULL;
    
    handler->config = *config;
    handler->callback = callback;
    handler->user_data = user_data;
    handler->running = true;
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&handler->mutex, NULL) != 0 ||
        pthread_cond_init(&handler->queue_cond, NULL) != 0) {
        message_handler_destroy(handler);
        return NULL;
    }
    
    // 启动工作线程
    if (pthread_create(&handler->worker_thread, NULL, worker_thread_func, handler) != 0) {
        message_handler_destroy(handler);
        return NULL;
    }
    
    return handler;
}

// 销毁消息处理器
void message_handler_destroy(message_handler_t *handler) {
    if (!handler) return;
    
    // 停止工作线程
    handler->running = false;
    pthread_cond_signal(&handler->queue_cond);
    pthread_join(handler->worker_thread, NULL);
    
    // 清理队列
    message_queue_item_t *item = handler->send_queue_head;
    while (item) {
        message_queue_item_t *next = item->next;
        json_message_destroy(item->message);
        free(item);
        item = next;
    }
    
    // 清理待响应请求
    item = handler->pending_requests;
    while (item) {
        message_queue_item_t *next = item->next;
        json_message_destroy(item->message);
        free(item);
        item = next;
    }
    
    // 销毁同步对象
    pthread_mutex_destroy(&handler->mutex);
    pthread_cond_destroy(&handler->queue_cond);
    
    free(handler);
}

// 设置 WebSocket 连接
void message_handler_set_connection(message_handler_t *handler, ws_connection_t *conn) {
    if (!handler) return;
    
    pthread_mutex_lock(&handler->mutex);
    handler->ws_conn = conn;
    pthread_mutex_unlock(&handler->mutex);
}

// 发送 JSON 消息
int message_handler_send(message_handler_t *handler, const json_message_t *message) {
    if (!handler || !message) return -1;
    
    pthread_mutex_lock(&handler->mutex);
    
    // 检查队列是否已满
    if (handler->queue_size >= handler->config.max_queue_size) {
        pthread_mutex_unlock(&handler->mutex);
        
        // 触发队列满事件
        message_event_t event = {
            .type = MSG_EVENT_QUEUE_FULL,
            .message = (json_message_t *)message
        };
        if (handler->callback) {
            handler->callback(&event, handler->user_data);
        }
        
        return -1;
    }
    
    // 创建队列项
    message_queue_item_t *item = calloc(1, sizeof(message_queue_item_t));
    if (!item) {
        pthread_mutex_unlock(&handler->mutex);
        return -1;
    }
    
    // 复制消息
    item->message = json_message_create_with_id(message->type, message->id, message->data);
    if (!item->message) {
        free(item);
        pthread_mutex_unlock(&handler->mutex);
        return -1;
    }
    
    item->message->priority = message->priority;
    item->message->timeout_ms = message->timeout_ms;
    item->created_at = time(NULL);
    item->attempts = 0;
    
    // 添加到队列尾部
    if (handler->send_queue_tail) {
        handler->send_queue_tail->next = item;
    } else {
        handler->send_queue_head = item;
    }
    handler->send_queue_tail = item;
    handler->queue_size++;
    
    // 通知工作线程
    pthread_cond_signal(&handler->queue_cond);
    pthread_mutex_unlock(&handler->mutex);
    
    return 0;
}

// 发送请求消息（等待响应）
int message_handler_send_request(message_handler_t *handler, 
                                const char *type,
                                const char *data,
                                uint32_t timeout_ms,
                                char **response_id) {
    if (!handler || !type || !data) return -1;
    
    json_message_t *msg = json_message_create(type, data);
    if (!msg) return -1;
    
    msg->timeout_ms = timeout_ms > 0 ? timeout_ms : handler->config.default_timeout_ms;
    
    if (response_id) {
        *response_id = strdup(msg->id);
    }
    
    int result = message_handler_send(handler, msg);
    json_message_destroy(msg);
    
    return result;
}

// 发送响应消息
int message_handler_send_response(message_handler_t *handler,
                                 const char *request_id,
                                 const char *data,
                                 bool success) {
    if (!handler || !request_id || !data) return -1;
    
    // 构造响应数据
    cJSON *response_data = cJSON_CreateObject();
    cJSON_AddStringToObject(response_data, "request_id", request_id);
    cJSON_AddBoolToObject(response_data, "success", success);
    
    cJSON *payload = cJSON_Parse(data);
    if (payload) {
        cJSON_AddItemToObject(response_data, "payload", payload);
    } else {
        cJSON_AddStringToObject(response_data, "payload", data);
    }
    
    char *response_json = cJSON_Print(response_data);
    cJSON_Delete(response_data);
    
    if (!response_json) return -1;
    
    json_message_t *msg = json_message_create("response", response_json);
    free(response_json);
    
    if (!msg) return -1;
    
    int result = message_handler_send(handler, msg);
    json_message_destroy(msg);
    
    return result;
}

// 发送通知消息
int message_handler_send_notification(message_handler_t *handler,
                                     const char *type,
                                     const char *data) {
    if (!handler || !type || !data) return -1;
    
    json_message_t *msg = json_message_create(type, data);
    if (!msg) return -1;
    
    int result = message_handler_send(handler, msg);
    json_message_destroy(msg);
    
    return result;
}

// 处理接收到的 WebSocket 消息
void message_handler_on_websocket_message(message_handler_t *handler,
                                         const uint8_t *data,
                                         size_t length,
                                         ws_frame_type_t frame_type) {
    if (!handler || !data || length == 0) return;
    
    if (frame_type != WS_FRAME_TEXT) {
        return; // 只处理文本消息
    }
    
    // 确保字符串以 null 结尾
    char *json_str = malloc(length + 1);
    if (!json_str) return;
    
    memcpy(json_str, data, length);
    json_str[length] = '\0';
    
    // 反序列化消息
    json_message_t *msg = json_message_deserialize(json_str);
    free(json_str);
    
    if (!msg) return;
    
    // 更新统计信息
    pthread_mutex_lock(&handler->mutex);
    handler->stats.messages_received++;
    pthread_mutex_unlock(&handler->mutex);
    
    // 触发消息接收事件
    message_event_t event = {
        .type = MSG_EVENT_RECEIVED,
        .message = msg
    };
    
    if (handler->callback) {
        handler->callback(&event, handler->user_data);
    }
    
    json_message_destroy(msg);
}

// 获取消息统计信息
const message_stats_t *message_handler_get_stats(const message_handler_t *handler) {
    if (!handler) return NULL;
    return &handler->stats;
}
