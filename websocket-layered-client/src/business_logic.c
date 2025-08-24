/**
 * 业务逻辑层实现
 * 
 * 负责应用特定的业务逻辑：
 * - 业务消息处理
 * - 订阅/发布模式
 * - 用户认证和授权
 * - 业务状态管理
 */

#include "business_logic.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <cjson/cJSON.h>

// 业务逻辑处理器结构体
struct business_logic {
    business_config_t config;
    business_event_callback_t callback;
    void *user_data;
    
    // 消息处理器
    message_handler_t *msg_handler;
    
    // 订阅管理
    subscription_t *subscriptions;
    
    // 统计信息
    business_stats_t stats;
    
    // 线程安全
    pthread_mutex_t mutex;
    
    // 状态
    bool connected;
    char *session_id;
    time_t last_heartbeat;
};

// 默认配置
business_config_t business_config_default(void) {
    business_config_t config = {
        .client_id = "layered_client",
        .client_version = "1.0.0",
        .heartbeat_interval_ms = 30000,
        .response_timeout_ms = 10000,
        .auto_reconnect = true,
        .max_reconnect_attempts = 5,
        .reconnect_delay_ms = 1000,
        .enable_logging = true
    };
    return config;
}

// 生成客户端 ID
char *generate_client_id(const char *prefix) {
    char *id = malloc(64);
    if (id) {
        snprintf(id, 64, "%s_%lu_%d", 
                prefix ? prefix : "client", 
                time(NULL), 
                rand() % 10000);
    }
    return id;
}

// 格式化时间戳
char *format_timestamp(uint64_t timestamp) {
    char *formatted = malloc(32);
    if (formatted) {
        time_t t = timestamp / 1000;
        struct tm *tm_info = localtime(&t);
        strftime(formatted, 32, "%Y-%m-%d %H:%M:%S", tm_info);
    }
    return formatted;
}

// 验证消息格式
bool validate_message_format(const char *message) {
    if (!message) return false;
    
    cJSON *json = cJSON_Parse(message);
    if (!json) return false;
    
    bool valid = cJSON_HasObjectItem(json, "type") && 
                 cJSON_HasObjectItem(json, "data");
    
    cJSON_Delete(json);
    return valid;
}

// 构建认证请求
char *build_auth_request(const char *username, const char *password, const char *token) {
    cJSON *data = cJSON_CreateObject();
    if (!data) return NULL;
    
    if (username) cJSON_AddStringToObject(data, "username", username);
    if (password) cJSON_AddStringToObject(data, "password", password);
    if (token) cJSON_AddStringToObject(data, "token", token);
    
    char *json_str = cJSON_Print(data);
    cJSON_Delete(data);
    
    return json_str;
}

// 构建数据查询请求
char *build_query_request(const char *query_type, const char *parameters) {
    cJSON *data = cJSON_CreateObject();
    if (!data) return NULL;
    
    cJSON_AddStringToObject(data, "query_type", query_type);
    
    if (parameters) {
        cJSON *params = cJSON_Parse(parameters);
        if (params) {
            cJSON_AddItemToObject(data, "parameters", params);
        } else {
            cJSON_AddStringToObject(data, "parameters", parameters);
        }
    }
    
    char *json_str = cJSON_Print(data);
    cJSON_Delete(data);
    
    return json_str;
}

// 构建订阅请求
char *build_subscribe_request(const char *topic, const char *filters) {
    cJSON *data = cJSON_CreateObject();
    if (!data) return NULL;
    
    cJSON_AddStringToObject(data, "topic", topic);
    
    if (filters) {
        cJSON *filter_obj = cJSON_Parse(filters);
        if (filter_obj) {
            cJSON_AddItemToObject(data, "filters", filter_obj);
        }
    }
    
    char *json_str = cJSON_Print(data);
    cJSON_Delete(data);
    
    return json_str;
}

// 构建心跳请求
char *build_heartbeat_request(const char *client_id, uint64_t timestamp) {
    cJSON *data = cJSON_CreateObject();
    if (!data) return NULL;
    
    cJSON_AddStringToObject(data, "client_id", client_id);
    cJSON_AddNumberToObject(data, "timestamp", timestamp);
    cJSON_AddStringToObject(data, "status", "alive");
    
    char *json_str = cJSON_Print(data);
    cJSON_Delete(data);
    
    return json_str;
}

// 解析认证响应
bool parse_auth_response(const char *response, char **session_id, char **error_msg) {
    if (!response) return false;
    
    cJSON *json = cJSON_Parse(response);
    if (!json) return false;
    
    cJSON *success_item = cJSON_GetObjectItem(json, "success");
    bool success = cJSON_IsTrue(success_item);
    
    if (success && session_id) {
        cJSON *session_item = cJSON_GetObjectItem(json, "session_id");
        if (cJSON_IsString(session_item)) {
            *session_id = strdup(session_item->valuestring);
        }
    }
    
    if (!success && error_msg) {
        cJSON *error_item = cJSON_GetObjectItem(json, "error");
        if (cJSON_IsString(error_item)) {
            *error_msg = strdup(error_item->valuestring);
        }
    }
    
    cJSON_Delete(json);
    return success;
}

// 解析通知消息
bool parse_notification(const char *notification, char **topic, char **content, uint64_t *timestamp) {
    if (!notification) return false;
    
    cJSON *json = cJSON_Parse(notification);
    if (!json) return false;
    
    cJSON *topic_item = cJSON_GetObjectItem(json, "topic");
    cJSON *content_item = cJSON_GetObjectItem(json, "content");
    cJSON *timestamp_item = cJSON_GetObjectItem(json, "timestamp");
    
    bool success = false;
    
    if (cJSON_IsString(topic_item) && topic) {
        *topic = strdup(topic_item->valuestring);
        success = true;
    }
    
    if (cJSON_IsString(content_item) && content) {
        *content = strdup(content_item->valuestring);
    } else if (content_item && content) {
        *content = cJSON_Print(content_item);
    }
    
    if (cJSON_IsNumber(timestamp_item) && timestamp) {
        *timestamp = (uint64_t)timestamp_item->valuedouble;
    }
    
    cJSON_Delete(json);
    return success;
}

// 消息事件处理器
static void on_message_event(const message_event_t *event, void *user_data) {
    business_logic_t *logic = (business_logic_t *)user_data;
    if (!logic || !event) return;
    
    pthread_mutex_lock(&logic->mutex);
    
    switch (event->type) {
        case MSG_EVENT_RECEIVED:
            if (event->message) {
                logic->stats.notifications_received++;

                // 打印接收到的消息用于调试
                printf("[业务层] 收到消息: 类型=%s, ID=%s, 数据=%s\n",
                       event->message->type ? event->message->type : "NULL",
                       event->message->id ? event->message->id : "NULL",
                       event->message->data ? event->message->data : "NULL");

                // 根据消息类型处理
                if (strcmp(event->message->type, "notification") == 0) {
                    business_event_t biz_event = {
                        .type = BIZ_EVENT_NOTIFICATION_RECEIVED,
                        .message_type = event->message->type,
                        .message_id = event->message->id,
                        .data = event->message->data,
                        .timestamp = event->message->timestamp
                    };

                    if (logic->callback) {
                        logic->callback(&biz_event, logic->user_data);
                    }
                } else if (strcmp(event->message->type, "response") == 0) {
                    logic->stats.responses_received++;

                    business_event_t biz_event = {
                        .type = BIZ_EVENT_RESPONSE_RECEIVED,
                        .message_type = event->message->type,
                        .message_id = event->message->id,
                        .data = event->message->data,
                        .timestamp = event->message->timestamp
                    };

                    if (logic->callback) {
                        logic->callback(&biz_event, logic->user_data);
                    }
                } else {
                    // 处理其他所有类型的消息（text, subscribe, heartbeat 等）
                    business_event_t biz_event = {
                        .type = BIZ_EVENT_MESSAGE_RECEIVED,
                        .message_type = event->message->type,
                        .message_id = event->message->id,
                        .data = event->message->data,
                        .timestamp = event->message->timestamp
                    };

                    if (logic->callback) {
                        logic->callback(&biz_event, logic->user_data);
                    }
                }
            }
            break;
            
        case MSG_EVENT_SENT:
            // 消息发送成功
            break;
            
        case MSG_EVENT_ERROR:
            {
                business_event_t biz_event = {
                    .type = BIZ_EVENT_ERROR,
                    .error_code = event->error_code,
                    .error_description = event->error_description,
                    .timestamp = get_timestamp_ms()
                };
                
                if (logic->callback) {
                    logic->callback(&biz_event, logic->user_data);
                }
            }
            break;
            
        default:
            break;
    }
    
    pthread_mutex_unlock(&logic->mutex);
}

// 创建业务逻辑处理器
business_logic_t *business_logic_create(const business_config_t *config,
                                       business_event_callback_t callback,
                                       void *user_data) {
    if (!config || !callback) return NULL;
    
    business_logic_t *logic = calloc(1, sizeof(business_logic_t));
    if (!logic) return NULL;
    
    logic->config = *config;
    logic->callback = callback;
    logic->user_data = user_data;
    logic->stats.session_start_time = time(NULL);
    
    // 初始化互斥锁
    if (pthread_mutex_init(&logic->mutex, NULL) != 0) {
        business_logic_destroy(logic);
        return NULL;
    }
    
    return logic;
}

// 销毁业务逻辑处理器
void business_logic_destroy(business_logic_t *logic) {
    if (!logic) return;
    
    // 清理订阅
    subscription_t *sub = logic->subscriptions;
    while (sub) {
        subscription_t *next = sub->next;
        free(sub->topic);
        free(sub);
        sub = next;
    }
    
    // 清理会话信息
    free(logic->session_id);
    
    // 销毁互斥锁
    pthread_mutex_destroy(&logic->mutex);
    
    free(logic);
}

// 设置消息处理器
void business_logic_set_message_handler(business_logic_t *logic, 
                                       message_handler_t *handler) {
    if (!logic) return;
    
    pthread_mutex_lock(&logic->mutex);
    logic->msg_handler = handler;
    pthread_mutex_unlock(&logic->mutex);
}

// 发送业务请求
int business_logic_send_request(business_logic_t *logic,
                               const char *action,
                               const char *params,
                               char **request_id) {
    if (!logic || !action || !logic->msg_handler) return -1;
    
    char *data = build_query_request(action, params);
    if (!data) return -1;
    
    int result = message_handler_send_request(logic->msg_handler, 
                                            "request", 
                                            data, 
                                            logic->config.response_timeout_ms,
                                            request_id);
    
    if (result == 0) {
        pthread_mutex_lock(&logic->mutex);
        logic->stats.requests_sent++;
        pthread_mutex_unlock(&logic->mutex);
    }
    
    free(data);
    return result;
}

// 订阅主题
int business_logic_subscribe_topic(business_logic_t *logic, const char *topic) {
    if (!logic || !topic || !logic->msg_handler) return -1;
    
    // 检查是否已经订阅
    pthread_mutex_lock(&logic->mutex);
    subscription_t *sub = logic->subscriptions;
    while (sub) {
        if (strcmp(sub->topic, topic) == 0) {
            if (sub->active) {
                pthread_mutex_unlock(&logic->mutex);
                return 0; // 已经订阅
            }
            break;
        }
        sub = sub->next;
    }
    
    // 创建新订阅
    if (!sub) {
        sub = calloc(1, sizeof(subscription_t));
        if (!sub) {
            pthread_mutex_unlock(&logic->mutex);
            return -1;
        }
        
        sub->topic = strdup(topic);
        sub->next = logic->subscriptions;
        logic->subscriptions = sub;
    }
    
    sub->active = true;
    sub->subscribed_at = get_timestamp_ms();
    logic->stats.subscriptions_active++;
    
    pthread_mutex_unlock(&logic->mutex);
    
    // 发送订阅请求
    char *data = build_subscribe_request(topic, NULL);
    if (!data) return -1;
    
    int result = message_handler_send_notification(logic->msg_handler, "subscribe", data);
    free(data);
    
    return result;
}

// 取消订阅主题
int business_logic_unsubscribe_topic(business_logic_t *logic, const char *topic) {
    if (!logic || !topic || !logic->msg_handler) return -1;
    
    pthread_mutex_lock(&logic->mutex);
    
    subscription_t *sub = logic->subscriptions;
    while (sub) {
        if (strcmp(sub->topic, topic) == 0 && sub->active) {
            sub->active = false;
            logic->stats.subscriptions_active--;
            break;
        }
        sub = sub->next;
    }
    
    pthread_mutex_unlock(&logic->mutex);
    
    // 发送取消订阅请求
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "topic", topic);
    char *json_str = cJSON_Print(data);
    cJSON_Delete(data);
    
    if (!json_str) return -1;
    
    int result = message_handler_send_notification(logic->msg_handler, "unsubscribe", json_str);
    free(json_str);
    
    return result;
}

// 发送心跳
int business_logic_send_heartbeat(business_logic_t *logic) {
    if (!logic || !logic->msg_handler) return -1;
    
    char *data = build_heartbeat_request(logic->config.client_id, get_timestamp_ms());
    if (!data) return -1;
    
    int result = message_handler_send_notification(logic->msg_handler, "heartbeat", data);
    
    if (result == 0) {
        pthread_mutex_lock(&logic->mutex);
        logic->stats.heartbeats_sent++;
        logic->last_heartbeat = time(NULL);
        pthread_mutex_unlock(&logic->mutex);
    }
    
    free(data);
    return result;
}

// 处理消息事件
void business_logic_on_message_event(business_logic_t *logic,
                                    const message_event_t *event) {
    on_message_event(event, logic);
}

// 获取业务统计信息
const business_stats_t *business_logic_get_stats(const business_logic_t *logic) {
    if (!logic) return NULL;
    return &logic->stats;
}

// 获取活跃订阅列表
const subscription_t *business_logic_get_subscriptions(const business_logic_t *logic) {
    if (!logic) return NULL;
    return logic->subscriptions;
}
