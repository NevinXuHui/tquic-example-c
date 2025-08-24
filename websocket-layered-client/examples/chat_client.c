/**
 * 聊天客户端示例 - 展示分层 WebSocket 客户端的使用
 * 
 * 功能：
 * - 连接到聊天服务器
 * - 发送和接收聊天消息
 * - 订阅聊天频道
 * - 实时显示在线用户
 * - 自动重连和心跳
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <inttypes.h>
#include <time.h>
#include "layered_websocket_client.h"

// 全局变量
static layered_websocket_client_t *g_client = NULL;
static bool g_running = true;
static pthread_mutex_t g_output_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char *g_username = "anonymous";

// 用户输入线程
static pthread_t g_input_thread;

// 安全打印函数
void safe_printf(const char *format, ...) {
    pthread_mutex_lock(&g_output_mutex);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&g_output_mutex);
}

// 客户端事件处理器
void on_client_event(const client_event_t *event, void *user_data) {
    switch (event->type) {
        case CLIENT_EVENT_STATE_CHANGED:
            safe_printf("[状态] %s -> %s\n",
                       client_state_to_string(event->old_state),
                       client_state_to_string(event->new_state));
            
            if (event->new_state == CLIENT_STATE_CONNECTED) {
                safe_printf("[系统] 连接成功！输入 /help 查看命令帮助\n");
                // 自动订阅默认频道
                layered_client_subscribe(g_client, "general");
            }
            break;
            
        case CLIENT_EVENT_MESSAGE_RECEIVED:
            if (event->message_type && event->message_data) {
                if (strcmp(event->message_type, "chat_message") == 0) {
                    // 解析聊天消息
                    safe_printf("[聊天] %s\n", event->message_data);
                } else if (strcmp(event->message_type, "user_joined") == 0) {
                    safe_printf("[系统] 用户加入: %s\n", event->message_data);
                } else if (strcmp(event->message_type, "user_left") == 0) {
                    safe_printf("[系统] 用户离开: %s\n", event->message_data);
                } else if (strcmp(event->message_type, "channel_subscribed") == 0) {
                    safe_printf("[系统] 已订阅频道: %s\n", event->message_data);
                } else {
                    // 显示原始消息用于调试
                    safe_printf("[消息] 类型: %s, 内容: %s\n", event->message_type, event->message_data);
                }
            } else {
                safe_printf("[消息] 收到空消息\n");
            }
            break;
            
        case CLIENT_EVENT_ERROR:
            safe_printf("[错误] %s (代码: %d)\n",
                       event->error_description ? event->error_description : "未知错误",
                       event->error_code);

            // 如果是严重错误，退出程序
            if (event->error_code < 0) {
                safe_printf("[系统] 遇到严重错误，程序将退出\n");
                g_running = false;
                layered_client_stop(g_client);
            }
            break;
            
        case CLIENT_EVENT_RECONNECTED:
            safe_printf("[系统] 重连成功\n");
            break;
            
        default:
            break;
    }
}

// 处理用户命令
void process_command(const char *input) {
    if (strncmp(input, "/help", 5) == 0) {
        safe_printf("可用命令:\n");
        safe_printf("  /join <频道>     - 加入频道\n");
        safe_printf("  /leave <频道>    - 离开频道\n");
        safe_printf("  /list            - 列出已订阅频道\n");
        safe_printf("  /stats           - 显示统计信息\n");
        safe_printf("  /ping            - 发送心跳\n");
        safe_printf("  /quit            - 退出程序\n");
        safe_printf("  其他输入将作为聊天消息发送\n");
    } else if (strncmp(input, "/join ", 6) == 0) {
        const char *channel = input + 6;
        if (layered_client_subscribe(g_client, channel) == 0) {
            safe_printf("[系统] 正在加入频道: %s\n", channel);
        } else {
            safe_printf("[错误] 无法加入频道: %s\n", channel);
        }
    } else if (strncmp(input, "/leave ", 7) == 0) {
        const char *channel = input + 7;
        if (layered_client_unsubscribe(g_client, channel) == 0) {
            safe_printf("[系统] 正在离开频道: %s\n", channel);
        } else {
            safe_printf("[错误] 无法离开频道: %s\n", channel);
        }
    } else if (strncmp(input, "/list", 5) == 0) {
        const subscription_t *subs = layered_client_get_subscriptions(g_client);
        safe_printf("[系统] 已订阅频道:\n");
        while (subs) {
            if (subs->active) {
                safe_printf("  - %s (消息数: %llu)\n", subs->topic, subs->message_count);
            }
            subs = subs->next;
        }
    } else if (strncmp(input, "/stats", 6) == 0) {
        const client_stats_t *stats = layered_client_get_stats(g_client);
        print_client_stats(stats);
    } else if (strncmp(input, "/ping", 5) == 0) {
        if (layered_client_send_heartbeat(g_client) == 0) {
            safe_printf("[系统] 心跳已发送\n");
        } else {
            safe_printf("[错误] 无法发送心跳\n");
        }
    } else if (strncmp(input, "/quit", 5) == 0) {
        g_running = false;
        layered_client_stop(g_client);
    } else {
        // 发送聊天消息 - 兼容原服务器的简单文本格式
        if (layered_client_send_notification(g_client, "text", input) == 0) {
            safe_printf("[我] %s\n", input);
        } else {
            // 尝试发送原始文本消息
            char message_data[1024];
            snprintf(message_data, sizeof(message_data),
                    "{\"user\": \"%s\", \"message\": \"%s\", \"timestamp\": %lu}",
                    g_username, input, time(NULL));

            if (layered_client_send_notification(g_client, "chat_message", message_data) == 0) {
                safe_printf("[我] %s\n", input);
            } else {
                safe_printf("[错误] 无法发送消息 - 请检查连接状态\n");
            }
        }
    }
}

// 用户输入线程函数
void *input_thread_func(void *arg) {
    char input[1024];
    
    while (g_running) {
        if (fgets(input, sizeof(input), stdin) != NULL) {
            // 移除换行符
            size_t len = strlen(input);
            if (len > 0 && input[len - 1] == '\n') {
                input[len - 1] = '\0';
            }
            
            if (strlen(input) > 0) {
                process_command(input);
            }
        }
        
        if (feof(stdin)) {
            break;
        }
    }
    
    return NULL;
}

// 信号处理器
void signal_handler(int sig) {
    safe_printf("\n[系统] 收到信号 %d，正在退出...\n", sig);
    g_running = false;
    if (g_client) {
        layered_client_stop(g_client);
    }
}

int main(int argc, char *argv[]) {
    // 解析命令行参数
    const char *host = "127.0.0.1";
    const char *port = "4433";
    
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = argv[2];
    if (argc >= 4) g_username = argv[3];
    
    printf("分层 WebSocket 聊天客户端\n");
    printf("连接到: %s:%s (用户名: %s)\n", host, port, g_username);
    printf("输入 /help 查看命令帮助\n\n");
    
    // 设置信号处理器
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 创建客户端配置
    client_config_t config = layered_client_config_default();
    config.host = host;
    config.port = port;
    config.path = "/chat";
    config.client_id = g_username;
    config.auto_reconnect = true;
    config.max_reconnect_attempts = 5;
    config.heartbeat_interval_ms = 30000; // 30秒心跳
    config.enable_logging = true;
    
    // 验证配置
    char *error_msg = NULL;
    if (!validate_client_config(&config, &error_msg)) {
        fprintf(stderr, "配置验证失败: %s\n", error_msg ? error_msg : "未知错误");
        if (error_msg) free(error_msg);
        return 1;
    }

    printf("✅ 配置验证通过\n");
    printf("正在创建客户端...\n");

    // 创建客户端
    g_client = layered_client_create(&config, on_client_event, NULL);
    if (!g_client) {
        fprintf(stderr, "❌ 无法创建客户端 - 可能是内存不足或依赖库问题\n");
        return 1;
    }

    printf("✅ 客户端创建成功\n");
    
    // 连接到服务器
    printf("🔗 正在连接到服务器 %s:%s...\n", host, port);
    if (layered_client_connect(g_client) != 0) {
        fprintf(stderr, "❌ 无法连接到服务器 %s:%s\n", host, port);
        fprintf(stderr, "请确保:\n");
        fprintf(stderr, "  1. 服务器正在运行\n");
        fprintf(stderr, "  2. 地址和端口正确\n");
        fprintf(stderr, "  3. 网络连接正常\n");
        layered_client_destroy(g_client);
        return 1;
    }

    printf("✅ 连接请求已发送，等待服务器响应...\n");
    
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
    
    printf("客户端已退出\n");
    return 0;
}
