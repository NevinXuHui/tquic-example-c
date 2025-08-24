#ifndef WEBSOCKET_PROTOCOL_H
#define WEBSOCKET_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <ev.h>
#include "tquic.h"

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket 帧类型
typedef enum {
    WS_FRAME_CONTINUATION = 0x0,
    WS_FRAME_TEXT = 0x1,
    WS_FRAME_BINARY = 0x2,
    WS_FRAME_CLOSE = 0x8,
    WS_FRAME_PING = 0x9,
    WS_FRAME_PONG = 0xA
} ws_frame_type_t;

// WebSocket 连接状态
typedef enum {
    WS_STATE_CONNECTING,
    WS_STATE_CONNECTED,
    WS_STATE_CLOSING,
    WS_STATE_CLOSED,
    WS_STATE_ERROR
} ws_connection_state_t;

// WebSocket 帧结构
typedef struct {
    bool fin;
    bool rsv1, rsv2, rsv3;
    uint8_t opcode;
    bool mask;
    uint32_t masking_key;
    uint64_t payload_len;
    uint8_t *payload;
} ws_frame_t;

// 前向声明
typedef struct ws_connection ws_connection_t;

// WebSocket 事件类型
typedef enum {
    WS_EVENT_CONNECTED,
    WS_EVENT_MESSAGE_RECEIVED,
    WS_EVENT_MESSAGE_SENT,
    WS_EVENT_DISCONNECTED,
    WS_EVENT_ERROR,
    WS_EVENT_PING_RECEIVED,
    WS_EVENT_PONG_RECEIVED
} ws_event_type_t;

// WebSocket 事件数据
typedef struct {
    ws_event_type_t type;
    ws_connection_t *connection;
    union {
        struct {
            uint8_t *data;
            size_t length;
            ws_frame_type_t frame_type;
        } message;
        struct {
            int code;
            const char *description;
        } error;
    };
} ws_event_t;

// WebSocket 事件回调函数类型
typedef void (*ws_event_callback_t)(const ws_event_t *event, void *user_data);

// WebSocket 连接配置
typedef struct {
    const char *host;
    const char *port;
    const char *path;
    const char *origin;
    const char *protocol;
    uint32_t connect_timeout_ms;
    uint32_t ping_interval_ms;
    uint32_t pong_timeout_ms;
    bool auto_reconnect;
    uint32_t max_reconnect_attempts;
    uint32_t reconnect_delay_ms;
} ws_config_t;

// WebSocket 连接统计信息
typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t ping_count;
    uint64_t pong_count;
    uint32_t reconnect_count;
    time_t connected_at;
    time_t last_activity;
} ws_stats_t;

// WebSocket 协议层 API

/**
 * 创建 WebSocket 连接
 */
ws_connection_t *ws_connection_create(const ws_config_t *config,
                                     ws_event_callback_t callback,
                                     void *user_data);

/**
 * 销毁 WebSocket 连接
 */
void ws_connection_destroy(ws_connection_t *conn);

/**
 * 连接到服务器
 */
int ws_connection_connect(ws_connection_t *conn);

/**
 * 断开连接
 */
void ws_connection_close(ws_connection_t *conn, uint16_t code, const char *reason);

/**
 * 发送文本消息
 */
int ws_connection_send_text(ws_connection_t *conn, const char *data, size_t length);

/**
 * 发送二进制消息
 */
int ws_connection_send_binary(ws_connection_t *conn, const uint8_t *data, size_t length);

/**
 * 发送 Ping 帧
 */
int ws_connection_send_ping(ws_connection_t *conn, const uint8_t *data, size_t length);

/**
 * 获取连接状态
 */
ws_connection_state_t ws_connection_get_state(const ws_connection_t *conn);

/**
 * 获取连接统计信息
 */
const ws_stats_t *ws_connection_get_stats(const ws_connection_t *conn);

/**
 * 设置事件循环
 */
void ws_connection_set_event_loop(ws_connection_t *conn, struct ev_loop *loop);

/**
 * 处理事件循环中的 WebSocket 事件
 */
void ws_connection_process_events(ws_connection_t *conn);

/**
 * 获取默认配置
 */
ws_config_t ws_config_default(void);

// 工具函数

/**
 * 解析 WebSocket 帧
 */
int ws_frame_parse(const uint8_t *data, size_t length, ws_frame_t *frame);

/**
 * 创建 WebSocket 帧
 */
int ws_frame_create(ws_frame_type_t frame_type, const uint8_t *data, size_t length,
                   bool mask, uint8_t *output, size_t output_size);

/**
 * 释放帧资源
 */
void ws_frame_free(ws_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_PROTOCOL_H
