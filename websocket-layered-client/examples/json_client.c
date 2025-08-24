/**
 * JSON 数据交换示例客户端
 * 
 * 这个示例展示如何使用分层 WebSocket 客户端发送和接收结构化的 JSON 数据：
 * - 发送各种类型的 JSON 消息
 * - 接收并解析服务器的 JSON 响应
 * - 处理不同的消息类型（文本、通知、请求/响应等）
 * - 展示完整的 JSON 通信流程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <cjson/cJSON.h>
#include "layered_websocket_client.h"

// 全局变量
static layered_websocket_client_t *g_client = NULL;
static pthread_t g_input_thread;
static pthread_mutex_t g_output_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_running = true;
static uint32_t g_message_counter = 1;
static const char *g_client_id = "json_client";

// JSON 消息类型定义（避免与库中的枚举冲突）
typedef enum {
    JSON_MSG_TEXT,
    JSON_MSG_NOTIFICATION,
    JSON_MSG_REQUEST,
    JSON_MSG_RESPONSE,
    JSON_MSG_HEARTBEAT,
    JSON_MSG_SUBSCRIBE,
    JSON_MSG_PUBLISH,
    JSON_MSG_CUSTOM
} json_msg_type_t;

// 消息结构
typedef struct {
    json_msg_type_t type;
    const char *type_str;
    const char *description;
} message_type_info_t;

// 线程安全的输出函数
static void safe_printf(const char *format, ...) {
    pthread_mutex_lock(&g_output_mutex);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&g_output_mutex);
}

// 信号处理器
static void signal_handler(int sig) {
    safe_printf("\n[系统] 收到信号 %d，正在退出...\n", sig);
    g_running = false;
    if (g_client) {
        layered_client_stop(g_client);
    }
}

// 生成消息 ID
static char *generate_message_id(void) {
    static char msg_id[64];
    time_t now = time(NULL);
    snprintf(msg_id, sizeof(msg_id), "msg_%ld_%u", now, g_message_counter++);
    return msg_id;
}

// 获取当前时间戳（毫秒）- 使用不同的函数名避免冲突
static uint64_t get_current_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 创建 JSON 消息
static cJSON *create_json_message(const char *type, const char *data, int priority) {
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    
    cJSON_AddStringToObject(json, "type", type);
    cJSON_AddStringToObject(json, "id", generate_message_id());
    cJSON_AddNumberToObject(json, "timestamp", get_current_timestamp_ms());
    cJSON_AddNumberToObject(json, "priority", priority);
    
    if (data) {
        // 尝试解析 data 为 JSON，如果失败则作为字符串
        cJSON *data_json = cJSON_Parse(data);
        if (data_json) {
            cJSON_AddItemToObject(json, "data", data_json);
        } else {
            cJSON_AddStringToObject(json, "data", data);
        }
    }
    
    return json;
}

// 发送 JSON 消息
static int send_json_message(const char *type, const char *data, int priority) {
    cJSON *json = create_json_message(type, data, priority);
    if (!json) {
        safe_printf("[错误] 创建 JSON 消息失败\n");
        return -1;
    }

    char *json_string = cJSON_Print(json);
    cJSON_Delete(json);

    if (!json_string) {
        safe_printf("[错误] 序列化 JSON 消息失败\n");
        return -1;
    }

    int result;

    // 根据消息类型选择合适的发送函数
    if (strcmp(type, "notification") == 0) {
        result = layered_client_send_notification(g_client, type, json_string);
    } else if (strcmp(type, "request") == 0) {
        char *request_id = NULL;
        result = layered_client_send_request(g_client, "json_request", json_string, &request_id);
        if (request_id) {
            safe_printf("[请求ID] %s\n", request_id);
            free(request_id);
        }
    } else if (strcmp(type, "heartbeat") == 0) {
        result = layered_client_send_heartbeat(g_client);
    } else {
        // 对于其他类型，使用通知方式发送
        result = layered_client_send_notification(g_client, type, json_string);
    }

    if (result == 0) {
        safe_printf("[发送] %s\n", json_string);
    } else {
        safe_printf("[错误] 发送消息失败: %d\n", result);
    }

    free(json_string);
    return result;
}

// 解析并显示接收到的 JSON 消息
static void display_received_message(const char *message_type, const char *message_data) {
    if (!message_data) {
        safe_printf("[接收] 空消息\n");
        return;
    }
    
    // 尝试解析为 JSON
    cJSON *json = cJSON_Parse(message_data);
    if (!json) {
        safe_printf("[接收] 非 JSON 消息: %s\n", message_data);
        return;
    }
    
    // 提取基本字段
    cJSON *type = cJSON_GetObjectItem(json, "type");
    cJSON *id = cJSON_GetObjectItem(json, "id");
    cJSON *timestamp = cJSON_GetObjectItem(json, "timestamp");
    cJSON *priority = cJSON_GetObjectItem(json, "priority");
    cJSON *data = cJSON_GetObjectItem(json, "data");
    
    safe_printf("\n=== 接收到 JSON 消息 ===\n");
    safe_printf("类型: %s\n", type && cJSON_IsString(type) ? type->valuestring : "未知");
    safe_printf("ID: %s\n", id && cJSON_IsString(id) ? id->valuestring : "无");
    
    if (timestamp && cJSON_IsNumber(timestamp)) {
        time_t ts = (time_t)(timestamp->valuedouble / 1000);
        safe_printf("时间: %s", ctime(&ts));
    }
    
    if (priority && cJSON_IsNumber(priority)) {
        safe_printf("优先级: %d\n", (int)priority->valuedouble);
    }
    
    if (data) {
        char *data_str = cJSON_Print(data);
        if (data_str) {
            safe_printf("数据: %s\n", data_str);
            free(data_str);
        }
    }
    
    safe_printf("原始消息: %s\n", message_data);
    safe_printf("========================\n\n");
    
    cJSON_Delete(json);
}

// 客户端事件处理器
static void on_client_event(const client_event_t *event, void *user_data) {
    switch (event->type) {
        case CLIENT_EVENT_STATE_CHANGED:
            safe_printf("[状态] %s -> %s\n",
                       client_state_to_string(event->old_state),
                       client_state_to_string(event->new_state));
            
            if (event->new_state == CLIENT_STATE_CONNECTED) {
                safe_printf("[系统] 连接成功！开始 JSON 数据交换演示\n");
                safe_printf("[系统] 输入 'help' 查看可用命令\n\n");
                
                // 自动发送欢迎消息
                send_json_message("text", "Hello from JSON client!", 1);
                
                // 自动订阅默认主题
                send_json_message("subscribe", "{\"topic\": \"general\"}", 1);
            }
            break;
            
        case CLIENT_EVENT_MESSAGE_RECEIVED:
            display_received_message(event->message_type, event->message_data);
            break;
            
        case CLIENT_EVENT_ERROR:
            safe_printf("[错误] 客户端错误: %s (代码: %d)\n", 
                       event->error_description ? event->error_description : "未知错误",
                       event->error_code);
            break;
            
        case CLIENT_EVENT_SHUTDOWN_COMPLETE:
            safe_printf("[系统] 客户端已关闭\n");
            break;
            
        default:
            break;
    }
}

// 显示帮助信息
static void show_help(void) {
    safe_printf("\n=== JSON 客户端命令帮助 ===\n");
    safe_printf("基本命令:\n");
    safe_printf("  help                    - 显示此帮助信息\n");
    safe_printf("  quit/exit              - 退出程序\n");
    safe_printf("  status                 - 显示客户端状态\n");
    safe_printf("\n消息发送命令:\n");
    safe_printf("  text <内容>            - 发送文本消息\n");
    safe_printf("  notify <内容>          - 发送通知消息\n");
    safe_printf("  request <内容>         - 发送请求消息\n");
    safe_printf("  heartbeat              - 发送心跳消息\n");
    safe_printf("  subscribe <主题>       - 订阅主题\n");
    safe_printf("  publish <主题> <内容>  - 发布消息到主题\n");
    safe_printf("  json <JSON字符串>      - 发送自定义JSON消息\n");
    safe_printf("\n示例:\n");
    safe_printf("  text Hello World!\n");
    safe_printf("  notify 系统维护通知\n");
    safe_printf("  subscribe news\n");
    safe_printf("  publish chat 大家好！\n");
    safe_printf("  json {\"custom_field\": \"custom_value\"}\n");
    safe_printf("=============================\n\n");
}

// 处理用户输入命令
static void process_command(const char *input) {
    if (!input || strlen(input) == 0) return;

    // 移除换行符
    char *cmd = strdup(input);
    char *newline = strchr(cmd, '\n');
    if (newline) *newline = '\0';

    // 解析命令
    char *token = strtok(cmd, " ");
    if (!token) {
        free(cmd);
        return;
    }

    if (strcmp(token, "help") == 0) {
        show_help();
    }
    else if (strcmp(token, "quit") == 0 || strcmp(token, "exit") == 0) {
        safe_printf("[系统] 正在退出...\n");
        g_running = false;
        layered_client_stop(g_client);
    }
    else if (strcmp(token, "status") == 0) {
        client_state_t state = layered_client_get_state(g_client);
        safe_printf("[状态] 当前状态: %s\n", client_state_to_string(state));
    }
    else if (strcmp(token, "text") == 0) {
        char *content = strtok(NULL, "");
        if (content) {
            send_json_message("text", content, 1);
        } else {
            safe_printf("[错误] 请提供文本内容\n");
        }
    }
    else if (strcmp(token, "notify") == 0) {
        char *content = strtok(NULL, "");
        if (content) {
            send_json_message("notification", content, 2);
        } else {
            safe_printf("[错误] 请提供通知内容\n");
        }
    }
    else if (strcmp(token, "request") == 0) {
        char *content = strtok(NULL, "");
        if (content) {
            send_json_message("request", content, 2);
        } else {
            safe_printf("[错误] 请提供请求内容\n");
        }
    }
    else if (strcmp(token, "heartbeat") == 0) {
        char heartbeat_data[256];
        snprintf(heartbeat_data, sizeof(heartbeat_data),
                "{\"client_id\": \"%s\", \"timestamp\": %lu, \"status\": \"alive\"}",
                g_client_id, get_current_timestamp_ms());
        send_json_message("heartbeat", heartbeat_data, 1);
    }
    else if (strcmp(token, "subscribe") == 0) {
        char *topic = strtok(NULL, " ");
        if (topic) {
            char subscribe_data[256];
            snprintf(subscribe_data, sizeof(subscribe_data), "{\"topic\": \"%s\"}", topic);
            send_json_message("subscribe", subscribe_data, 1);
        } else {
            safe_printf("[错误] 请提供订阅主题\n");
        }
    }
    else if (strcmp(token, "publish") == 0) {
        char *topic = strtok(NULL, " ");
        char *content = strtok(NULL, "");
        if (topic && content) {
            char publish_data[512];
            snprintf(publish_data, sizeof(publish_data),
                    "{\"topic\": \"%s\", \"message\": \"%s\"}", topic, content);
            send_json_message("publish", publish_data, 1);
        } else {
            safe_printf("[错误] 请提供主题和内容\n");
        }
    }
    else if (strcmp(token, "json") == 0) {
        char *json_str = strtok(NULL, "");
        if (json_str) {
            // 验证 JSON 格式
            cJSON *test_json = cJSON_Parse(json_str);
            if (test_json) {
                cJSON_Delete(test_json);
                send_json_message("custom", json_str, 1);
            } else {
                safe_printf("[错误] 无效的 JSON 格式\n");
            }
        } else {
            safe_printf("[错误] 请提供 JSON 字符串\n");
        }
    }
    else {
        safe_printf("[错误] 未知命令: %s\n", token);
        safe_printf("输入 'help' 查看可用命令\n");
    }

    free(cmd);
}

// 用户输入线程
static void *input_thread_func(void *arg) {
    char input[1024];

    while (g_running) {
        printf("> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin)) {
            if (strlen(input) > 1) { // 忽略空行
                process_command(input);
            }
        } else {
            break;
        }
    }

    return NULL;
}

// 主函数
int main(int argc, char *argv[]) {
    // 解析命令行参数
    const char *host = "127.0.0.1";
    const char *port = "4433";

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = argv[2];
    if (argc >= 4) g_client_id = argv[3];

    printf("JSON WebSocket 客户端示例\n");
    printf("连接到: %s:%s (客户端ID: %s)\n", host, port, g_client_id);
    printf("这个示例展示如何发送和接收结构化的 JSON 数据\n\n");

    // 设置信号处理器
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 创建客户端配置
    client_config_t config = layered_client_config_default();
    config.host = host;
    config.port = port;
    config.path = "/";
    config.client_id = g_client_id;
    config.auto_reconnect = true;
    config.max_reconnect_attempts = 3;
    config.heartbeat_interval_ms = 30000; // 30秒心跳
    config.enable_logging = false; // 减少日志输出，专注于 JSON 数据

    // 验证配置
    char *error_msg = NULL;
    if (!validate_client_config(&config, &error_msg)) {
        fprintf(stderr, "配置验证失败: %s\n", error_msg ? error_msg : "未知错误");
        if (error_msg) free(error_msg);
        return 1;
    }

    printf("✅ 配置验证通过\n");

    // 创建客户端
    g_client = layered_client_create(&config, on_client_event, NULL);
    if (!g_client) {
        fprintf(stderr, "❌ 无法创建客户端\n");
        return 1;
    }

    printf("✅ 客户端创建成功\n");

    // 连接到服务器
    printf("🔗 正在连接到服务器 %s:%s...\n", host, port);
    if (layered_client_connect(g_client) != 0) {
        fprintf(stderr, "❌ 无法连接到服务器\n");
        layered_client_destroy(g_client);
        return 1;
    }

    printf("✅ 连接请求已发送\n\n");

    // 启动用户输入线程
    if (pthread_create(&g_input_thread, NULL, input_thread_func, NULL) != 0) {
        fprintf(stderr, "无法创建输入线程\n");
        layered_client_destroy(g_client);
        return 1;
    }

    // 运行客户端事件循环
    layered_client_run(g_client);

    // 等待输入线程结束
    pthread_cancel(g_input_thread);
    pthread_join(g_input_thread, NULL);

    // 清理资源
    layered_client_destroy(g_client);
    pthread_mutex_destroy(&g_output_mutex);

    printf("JSON 客户端已退出\n");
    return 0;
}
