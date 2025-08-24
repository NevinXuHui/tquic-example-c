/**
 * WebSocket 协议层实现
 * 
 * 负责处理底层 WebSocket 协议细节：
 * - 连接管理
 * - 帧解析和构造
 * - 心跳检测
 * - 自动重连
 */

#include "websocket_protocol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

// WebSocket 连接结构体
struct ws_connection {
    // 配置
    ws_config_t config;
    
    // 状态
    ws_connection_state_t state;
    ws_stats_t stats;
    
    // 网络连接
    struct quic_conn_t *quic_conn;
    struct http3_conn_t *h3_conn;
    uint64_t stream_id;
    int sock;
    
    // 事件处理
    ws_event_callback_t callback;
    void *user_data;
    struct ev_loop *loop;
    
    // 定时器
    ev_timer connect_timer;
    ev_timer ping_timer;
    ev_timer pong_timer;
    ev_timer reconnect_timer;
    
    // 缓冲区
    uint8_t *recv_buffer;
    size_t recv_buffer_size;
    size_t recv_buffer_used;
    
    uint8_t *send_buffer;
    size_t send_buffer_size;
    
    // 重连状态
    uint32_t reconnect_attempts;
    bool auto_reconnect_enabled;
    
    // 线程安全
    pthread_mutex_t mutex;
};

// 默认配置
ws_config_t ws_config_default(void) {
    ws_config_t config = {
        .host = "localhost",
        .port = "4433",
        .path = "/websocket",
        .origin = NULL,
        .protocol = NULL,
        .connect_timeout_ms = 10000,
        .ping_interval_ms = 30000,
        .pong_timeout_ms = 5000,
        .auto_reconnect = true,
        .max_reconnect_attempts = 5,
        .reconnect_delay_ms = 1000
    };
    return config;
}

// 创建 WebSocket 连接
ws_connection_t *ws_connection_create(const ws_config_t *config,
                                     ws_event_callback_t callback,
                                     void *user_data) {
    if (!config || !callback) {
        return NULL;
    }
    
    ws_connection_t *conn = calloc(1, sizeof(ws_connection_t));
    if (!conn) {
        return NULL;
    }
    
    // 复制配置
    conn->config = *config;
    if (config->host) conn->config.host = strdup(config->host);
    if (config->port) conn->config.port = strdup(config->port);
    if (config->path) conn->config.path = strdup(config->path);
    if (config->origin) conn->config.origin = strdup(config->origin);
    if (config->protocol) conn->config.protocol = strdup(config->protocol);
    
    // 初始化状态
    conn->state = WS_STATE_CONNECTING;
    conn->callback = callback;
    conn->user_data = user_data;
    conn->sock = -1;
    
    // 分配缓冲区
    conn->recv_buffer_size = 8192;
    conn->recv_buffer = malloc(conn->recv_buffer_size);
    conn->send_buffer_size = 8192;
    conn->send_buffer = malloc(conn->send_buffer_size);
    
    if (!conn->recv_buffer || !conn->send_buffer) {
        ws_connection_destroy(conn);
        return NULL;
    }
    
    // 初始化互斥锁
    if (pthread_mutex_init(&conn->mutex, NULL) != 0) {
        ws_connection_destroy(conn);
        return NULL;
    }
    
    // 初始化统计信息
    conn->stats.connected_at = time(NULL);
    
    return conn;
}

// 销毁 WebSocket 连接
void ws_connection_destroy(ws_connection_t *conn) {
    if (!conn) return;
    
    // 停止所有定时器
    if (conn->loop) {
        ev_timer_stop(conn->loop, &conn->connect_timer);
        ev_timer_stop(conn->loop, &conn->ping_timer);
        ev_timer_stop(conn->loop, &conn->pong_timer);
        ev_timer_stop(conn->loop, &conn->reconnect_timer);
    }
    
    // 关闭连接
    if (conn->h3_conn) {
        // TODO: 关闭 HTTP/3 连接
    }
    
    if (conn->quic_conn) {
        // TODO: 关闭 QUIC 连接
    }
    
    if (conn->sock >= 0) {
        close(conn->sock);
    }
    
    // 释放配置字符串
    free((void*)conn->config.host);
    free((void*)conn->config.port);
    free((void*)conn->config.path);
    free((void*)conn->config.origin);
    free((void*)conn->config.protocol);
    
    // 释放缓冲区
    free(conn->recv_buffer);
    free(conn->send_buffer);
    
    // 销毁互斥锁
    pthread_mutex_destroy(&conn->mutex);
    
    free(conn);
}

// 连接到服务器
int ws_connection_connect(ws_connection_t *conn) {
    if (!conn) return -1;
    
    pthread_mutex_lock(&conn->mutex);
    
    if (conn->state != WS_STATE_CONNECTING && conn->state != WS_STATE_CLOSED) {
        pthread_mutex_unlock(&conn->mutex);
        return -1; // 已连接或正在连接
    }
    
    conn->state = WS_STATE_CONNECTING;
    
    // TODO: 实现 QUIC/HTTP3 连接逻辑
    // 1. 创建 UDP socket
    // 2. 初始化 QUIC 连接
    // 3. 建立 HTTP/3 连接
    // 4. 发送 WebSocket 升级请求
    
    // 临时实现：模拟连接成功
    conn->state = WS_STATE_CONNECTED;
    conn->stats.successful_connections++;
    conn->stats.connected_at = time(NULL);
    
    // 触发连接成功事件
    ws_event_t event = {
        .type = WS_EVENT_CONNECTED,
        .connection = conn
    };
    conn->callback(&event, conn->user_data);
    
    // 启动心跳定时器
    if (conn->loop && conn->config.ping_interval_ms > 0) {
        ev_timer_init(&conn->ping_timer, ping_timer_cb, 
                     conn->config.ping_interval_ms / 1000.0, 
                     conn->config.ping_interval_ms / 1000.0);
        conn->ping_timer.data = conn;
        ev_timer_start(conn->loop, &conn->ping_timer);
    }
    
    pthread_mutex_unlock(&conn->mutex);
    return 0;
}

// 断开连接
void ws_connection_close(ws_connection_t *conn, uint16_t code, const char *reason) {
    if (!conn) return;
    
    pthread_mutex_lock(&conn->mutex);
    
    if (conn->state == WS_STATE_CLOSED) {
        pthread_mutex_unlock(&conn->mutex);
        return;
    }
    
    conn->state = WS_STATE_CLOSING;
    
    // TODO: 发送 WebSocket 关闭帧
    
    conn->state = WS_STATE_CLOSED;
    
    // 触发断开连接事件
    ws_event_t event = {
        .type = WS_EVENT_DISCONNECTED,
        .connection = conn
    };
    conn->callback(&event, conn->user_data);
    
    pthread_mutex_unlock(&conn->mutex);
}

// 发送文本消息
int ws_connection_send_text(ws_connection_t *conn, const char *data, size_t length) {
    if (!conn || !data || conn->state != WS_STATE_CONNECTED) {
        return -1;
    }
    
    // TODO: 实现 WebSocket 文本帧发送
    // 1. 构造 WebSocket 帧
    // 2. 通过 HTTP/3 流发送
    
    // 临时实现：更新统计信息
    pthread_mutex_lock(&conn->mutex);
    conn->stats.messages_sent++;
    conn->stats.bytes_sent += length;
    conn->stats.last_activity = time(NULL);
    pthread_mutex_unlock(&conn->mutex);
    
    return 0;
}

// 发送二进制消息
int ws_connection_send_binary(ws_connection_t *conn, const uint8_t *data, size_t length) {
    if (!conn || !data || conn->state != WS_STATE_CONNECTED) {
        return -1;
    }
    
    // TODO: 实现 WebSocket 二进制帧发送
    
    pthread_mutex_lock(&conn->mutex);
    conn->stats.messages_sent++;
    conn->stats.bytes_sent += length;
    conn->stats.last_activity = time(NULL);
    pthread_mutex_unlock(&conn->mutex);
    
    return 0;
}

// 发送 Ping 帧
int ws_connection_send_ping(ws_connection_t *conn, const uint8_t *data, size_t length) {
    if (!conn || conn->state != WS_STATE_CONNECTED) {
        return -1;
    }
    
    // TODO: 实现 WebSocket Ping 帧发送
    
    pthread_mutex_lock(&conn->mutex);
    conn->stats.ping_count++;
    conn->stats.last_activity = time(NULL);
    pthread_mutex_unlock(&conn->mutex);
    
    return 0;
}

// 获取连接状态
ws_connection_state_t ws_connection_get_state(const ws_connection_t *conn) {
    if (!conn) return WS_STATE_ERROR;
    return conn->state;
}

// 获取连接统计信息
const ws_stats_t *ws_connection_get_stats(const ws_connection_t *conn) {
    if (!conn) return NULL;
    return &conn->stats;
}

// 设置事件循环
void ws_connection_set_event_loop(ws_connection_t *conn, struct ev_loop *loop) {
    if (!conn) return;
    conn->loop = loop;
}

// 处理事件循环中的 WebSocket 事件
void ws_connection_process_events(ws_connection_t *conn) {
    if (!conn) return;
    
    // TODO: 处理接收到的数据
    // TODO: 处理连接状态变化
    // TODO: 处理超时事件
}

// 心跳定时器回调（需要实现）
static void ping_timer_cb(EV_P_ ev_timer *w, int revents) {
    ws_connection_t *conn = (ws_connection_t *)w->data;
    if (conn && conn->state == WS_STATE_CONNECTED) {
        ws_connection_send_ping(conn, NULL, 0);
    }
}

// WebSocket 帧解析（简化实现）
int ws_frame_parse(const uint8_t *data, size_t length, ws_frame_t *frame) {
    if (!data || !frame || length < 2) {
        return -1;
    }
    
    // TODO: 实现完整的 WebSocket 帧解析
    // 这里只是一个骨架实现
    
    memset(frame, 0, sizeof(ws_frame_t));
    
    // 解析基本头部
    frame->fin = (data[0] & 0x80) != 0;
    frame->rsv1 = (data[0] & 0x40) != 0;
    frame->rsv2 = (data[0] & 0x20) != 0;
    frame->rsv3 = (data[0] & 0x10) != 0;
    frame->opcode = data[0] & 0x0F;
    
    frame->mask = (data[1] & 0x80) != 0;
    uint8_t payload_len = data[1] & 0x7F;
    
    size_t header_len = 2;
    
    // 处理扩展长度
    if (payload_len == 126) {
        if (length < 4) return 0; // 需要更多数据
        frame->payload_len = (data[2] << 8) | data[3];
        header_len += 2;
    } else if (payload_len == 127) {
        if (length < 10) return 0; // 需要更多数据
        frame->payload_len = 0;
        for (int i = 0; i < 8; i++) {
            frame->payload_len = (frame->payload_len << 8) | data[2 + i];
        }
        header_len += 8;
    } else {
        frame->payload_len = payload_len;
    }
    
    // 处理掩码
    if (frame->mask) {
        if (length < header_len + 4) return 0; // 需要更多数据
        frame->masking_key = (data[header_len] << 24) | 
                            (data[header_len + 1] << 16) |
                            (data[header_len + 2] << 8) | 
                            data[header_len + 3];
        header_len += 4;
    }
    
    // 检查是否有足够的数据
    if (length < header_len + frame->payload_len) {
        return 0; // 需要更多数据
    }
    
    // 复制载荷数据
    if (frame->payload_len > 0) {
        frame->payload = malloc(frame->payload_len);
        if (!frame->payload) return -1;
        
        memcpy(frame->payload, data + header_len, frame->payload_len);
        
        // 解掩码（修复字节序问题）
        if (frame->mask) {
            uint8_t mask_bytes[4] = {
                (frame->masking_key >> 24) & 0xFF,
                (frame->masking_key >> 16) & 0xFF,
                (frame->masking_key >> 8) & 0xFF,
                frame->masking_key & 0xFF
            };
            
            for (uint64_t i = 0; i < frame->payload_len; i++) {
                frame->payload[i] ^= mask_bytes[i % 4];
            }
        }
    }
    
    return header_len + frame->payload_len;
}

// 创建 WebSocket 帧（简化实现）
int ws_frame_create(ws_frame_type_t frame_type, const uint8_t *data, size_t length,
                   bool mask, uint8_t *output, size_t output_size) {
    if (!output || output_size < 2) {
        return -1;
    }
    
    // TODO: 实现完整的 WebSocket 帧创建
    // 这里只是一个骨架实现
    
    size_t header_len = 2;
    
    // 基本头部
    output[0] = 0x80 | (frame_type & 0x0F); // FIN=1
    
    if (length < 126) {
        output[1] = length;
    } else if (length < 65536) {
        output[1] = 126;
        output[2] = (length >> 8) & 0xFF;
        output[3] = length & 0xFF;
        header_len += 2;
    } else {
        output[1] = 127;
        for (int i = 0; i < 8; i++) {
            output[2 + i] = (length >> (56 - i * 8)) & 0xFF;
        }
        header_len += 8;
    }
    
    // 掩码处理
    if (mask) {
        output[1] |= 0x80;
        uint32_t masking_key = rand(); // 简单的随机掩码
        
        output[header_len] = (masking_key >> 24) & 0xFF;
        output[header_len + 1] = (masking_key >> 16) & 0xFF;
        output[header_len + 2] = (masking_key >> 8) & 0xFF;
        output[header_len + 3] = masking_key & 0xFF;
        header_len += 4;
        
        // 应用掩码（修复字节序问题）
        if (data && length > 0) {
            uint8_t mask_bytes[4] = {
                (masking_key >> 24) & 0xFF,
                (masking_key >> 16) & 0xFF,
                (masking_key >> 8) & 0xFF,
                masking_key & 0xFF
            };
            
            for (size_t i = 0; i < length; i++) {
                output[header_len + i] = data[i] ^ mask_bytes[i % 4];
            }
        }
    } else {
        // 不使用掩码，直接复制数据
        if (data && length > 0) {
            memcpy(output + header_len, data, length);
        }
    }
    
    return header_len + length;
}

// 释放帧资源
void ws_frame_free(ws_frame_t *frame) {
    if (frame && frame->payload) {
        free(frame->payload);
        frame->payload = NULL;
        frame->payload_len = 0;
    }
}
