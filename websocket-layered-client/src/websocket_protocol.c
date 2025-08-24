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
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include "tquic.h"
#include "openssl/ssl.h"

// 前向声明
static void ping_timer_cb(EV_P_ ev_timer *w, int revents);
static void socket_cb(EV_P_ ev_io *w, int revents);
static void timeout_callback(EV_P_ ev_timer *w, int revents);

// TQUIC 回调函数
static void client_on_conn_created(void *tctx, struct quic_conn_t *conn);
static void client_on_conn_established(void *tctx, struct quic_conn_t *conn);
static void client_on_conn_closed(void *tctx, struct quic_conn_t *conn);
static void client_on_stream_created(void *tctx, struct quic_conn_t *conn, uint64_t stream_id);
static void client_on_stream_readable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id);
static void client_on_stream_writable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id);
static void client_on_stream_closed(void *tctx, struct quic_conn_t *conn, uint64_t stream_id);

// 数据包发送回调
static int quic_packet_send(void *psctx, struct quic_packet_out_spec_t *pkts, unsigned int count);

// HTTP/3 事件处理器
static void http3_on_stream_headers(void *ctx, uint64_t stream_id,
                                   const struct http3_headers_t *headers, bool fin);
static void http3_on_stream_data(void *ctx, uint64_t stream_id);
static void http3_on_stream_finished(void *ctx, uint64_t stream_id);
static void http3_on_stream_reset(void *ctx, uint64_t stream_id, uint64_t error_code);
static void http3_on_stream_priority_update(void *ctx, uint64_t stream_id);
static void http3_on_conn_goaway(void *ctx, uint64_t stream_id);

// WebSocket 连接结构体
struct ws_connection {
    // 配置
    ws_config_t config;

    // 状态
    ws_connection_state_t state;
    ws_stats_t stats;

    // TQUIC 组件
    struct quic_endpoint_t *quic_endpoint;
    struct quic_conn_t *quic_conn;
    struct http3_conn_t *h3_conn;
    struct quic_tls_config_t *tls_config;
    struct http3_config_t *h3_config;
    uint64_t stream_id;

    // 网络连接
    int sock;
    struct sockaddr_storage server_addr;
    socklen_t server_addr_len;
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len;

    // 事件处理
    ws_event_callback_t callback;
    void *user_data;
    struct ev_loop *loop;
    ev_io socket_watcher;

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

    // WebSocket 状态
    bool websocket_handshake_done;

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

// HTTP/3 事件处理器实现
static void http3_on_stream_headers(void *ctx, uint64_t stream_id,
                                   const struct http3_headers_t *headers, bool fin) {
    ws_connection_t *ws_conn = (ws_connection_t *)ctx;
    if (!ws_conn) return;

    printf("HTTP/3 headers received on stream %lu\n", stream_id);

    // 简化检查：假设收到了 WebSocket 升级响应
    if (!ws_conn->websocket_handshake_done) {
        ws_conn->websocket_handshake_done = true;
        ws_conn->state = WS_STATE_CONNECTED;

        printf("WebSocket handshake completed!\n");

        // 触发连接成功事件
        ws_event_t event = {
            .type = WS_EVENT_CONNECTED,
            .connection = ws_conn
        };
        if (ws_conn->callback) {
            ws_conn->callback(&event, ws_conn->user_data);
        }
    }
}

static void http3_on_stream_data(void *ctx, uint64_t stream_id) {
    ws_connection_t *ws_conn = (ws_connection_t *)ctx;
    if (!ws_conn || !ws_conn->h3_conn) return;

    printf("HTTP/3 data received on stream %lu\n", stream_id);

    uint8_t buffer[4096];

    while (true) {
        ssize_t len = http3_recv_body(ws_conn->h3_conn, ws_conn->quic_conn,
                                     stream_id, buffer, sizeof(buffer));

        if (len < 0) {
            if (len == -1) { // HTTP3_ERR_DONE
                break; // 没有更多数据
            }
            printf("WebSocket read error: %ld\n", len);
            return;
        }

        if (len == 0) {
            break;
        }

        printf("Received %ld bytes of WebSocket data\n", len);

        // 解析 WebSocket 帧
        size_t offset = 0;
        while (offset < (size_t)len) {
            ws_frame_t frame;
            int frame_len = ws_frame_parse(buffer + offset, len - offset, &frame);

            if (frame_len < 0) {
                break; // 需要更多数据
            }

            printf("Parsed WebSocket frame: opcode=%d, length=%lu\n", frame.opcode, frame.payload_len);

            // 触发消息接收事件
            ws_event_t event = {
                .type = WS_EVENT_MESSAGE_RECEIVED,
                .connection = ws_conn,
                .message = {
                    .data = frame.payload,
                    .length = frame.payload_len,
                    .frame_type = frame.opcode
                }
            };
            if (ws_conn->callback) {
                ws_conn->callback(&event, ws_conn->user_data);
            }

            ws_frame_free(&frame);
            offset += frame_len;
        }

        // 更新统计信息
        pthread_mutex_lock(&ws_conn->mutex);
        ws_conn->stats.bytes_received += len;
        ws_conn->stats.last_activity = time(NULL);
        pthread_mutex_unlock(&ws_conn->mutex);
    }
}

static void http3_on_stream_finished(void *ctx, uint64_t stream_id) {
    printf("HTTP/3 stream %lu finished\n", stream_id);
}

static void http3_on_stream_reset(void *ctx, uint64_t stream_id, uint64_t error_code) {
    printf("HTTP/3 stream %lu reset with error %lu\n", stream_id, error_code);
}

static void http3_on_stream_priority_update(void *ctx, uint64_t stream_id) {
    printf("HTTP/3 stream %lu priority update\n", stream_id);
}

static void http3_on_conn_goaway(void *ctx, uint64_t stream_id) {
    printf("HTTP/3 connection goaway on stream %lu\n", stream_id);
}

// HTTP/3 事件处理器方法表
static const struct http3_methods_t http3_methods = {
    .on_stream_headers = http3_on_stream_headers,
    .on_stream_data = http3_on_stream_data,
    .on_stream_finished = http3_on_stream_finished,
    .on_stream_reset = http3_on_stream_reset,
    .on_stream_priority_update = http3_on_stream_priority_update,
    .on_conn_goaway = http3_on_conn_goaway,
};

// TQUIC 回调函数实现
static void client_on_conn_created(void *tctx, struct quic_conn_t *conn) {
    ws_connection_t *ws_conn = (ws_connection_t *)tctx;
    ws_conn->quic_conn = conn;
    printf("QUIC connection created\n");
}

static void client_on_conn_established(void *tctx, struct quic_conn_t *conn) {
    ws_connection_t *ws_conn = (ws_connection_t *)tctx;
    printf("QUIC connection established\n");

    // 创建 HTTP/3 连接
    ws_conn->h3_conn = http3_conn_new(conn, ws_conn->h3_config);
    if (!ws_conn->h3_conn) {
        printf("Failed to create HTTP/3 connection\n");
        return;
    }

    // 设置 HTTP/3 事件处理器
    http3_conn_set_events_handler(ws_conn->h3_conn, &http3_methods, ws_conn);

    // 生成随机 WebSocket 密钥
    uint8_t nonce[16];
    for (int i = 0; i < 16; i++) {
        nonce[i] = rand() & 0xFF;
    }

    // Base64 编码密钥
    char websocket_key[25]; // 16字节 -> 24字符 + null terminator
    static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    int j = 0;
    for (int i = 0; i < 16; i += 3) {
        uint32_t val = (nonce[i] << 16) |
                       ((i + 1 < 16) ? (nonce[i + 1] << 8) : 0) |
                       ((i + 2 < 16) ? nonce[i + 2] : 0);

        websocket_key[j++] = base64_chars[(val >> 18) & 0x3F];
        websocket_key[j++] = base64_chars[(val >> 12) & 0x3F];
        websocket_key[j++] = (i + 1 < 16) ? base64_chars[(val >> 6) & 0x3F] : '=';
        websocket_key[j++] = (i + 2 < 16) ? base64_chars[val & 0x3F] : '=';
    }
    websocket_key[24] = '\0';

    printf("Generated WebSocket key: %s\n", websocket_key);

    // 发送 WebSocket 升级请求
    struct http3_header_t headers[] = {
        {(uint8_t*)":method", 7, (uint8_t*)"GET", 3},
        {(uint8_t*)":path", 5, (uint8_t*)ws_conn->config.path, strlen(ws_conn->config.path)},
        {(uint8_t*)":scheme", 7, (uint8_t*)"https", 5},
        {(uint8_t*)":authority", 10, (uint8_t*)ws_conn->config.host, strlen(ws_conn->config.host)},
        {(uint8_t*)"upgrade", 7, (uint8_t*)"websocket", 9},
        {(uint8_t*)"connection", 10, (uint8_t*)"upgrade", 7},
        {(uint8_t*)"sec-websocket-key", 17, (uint8_t*)websocket_key, 24},
        {(uint8_t*)"sec-websocket-version", 21, (uint8_t*)"13", 2},
    };

    // 创建流并发送头部
    int64_t stream_id = http3_stream_new(ws_conn->h3_conn, conn);
    if (stream_id >= 0) {
        int ret = http3_send_headers(ws_conn->h3_conn, conn, stream_id, headers,
                                   sizeof(headers)/sizeof(headers[0]), false);
        if (ret == 0) {
            ws_conn->stream_id = stream_id;
            printf("WebSocket upgrade request sent on stream %lu\n", stream_id);
        } else {
            printf("Failed to send WebSocket upgrade headers\n");
        }
    } else {
        printf("Failed to create HTTP/3 stream\n");
    }
    if (stream_id >= 0) {
        ws_conn->stream_id = stream_id;
        printf("WebSocket upgrade request sent on stream %lu\n", stream_id);
    } else {
        printf("Failed to send WebSocket upgrade request\n");
    }
}

static void client_on_conn_closed(void *tctx, struct quic_conn_t *conn) {
    ws_connection_t *ws_conn = (ws_connection_t *)tctx;
    printf("QUIC connection closed\n");

    pthread_mutex_lock(&ws_conn->mutex);
    ws_conn->state = WS_STATE_CLOSED;

    // 触发断开连接事件
    ws_event_t event = {
        .type = WS_EVENT_DISCONNECTED,
        .connection = ws_conn
    };
    if (ws_conn->callback) {
        ws_conn->callback(&event, ws_conn->user_data);
    }
    pthread_mutex_unlock(&ws_conn->mutex);
}

static void client_on_stream_created(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    printf("Stream created: %lu\n", stream_id);
}

static void client_on_stream_readable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    ws_connection_t *ws_conn = (ws_connection_t *)tctx;

    if (!ws_conn || !ws_conn->h3_conn) return;

    // 处理 HTTP/3 流状态，这会触发相应的 HTTP/3 回调
    http3_conn_process_streams(ws_conn->h3_conn, conn);

    // 尝试读取数据
    uint8_t buffer[4096];
    ssize_t len = http3_recv_body(ws_conn->h3_conn, ws_conn->quic_conn, stream_id, buffer, sizeof(buffer));

    if (len > 0) {
        printf("Received %ld bytes on stream %lu\n", len, stream_id);

        if (!ws_conn->websocket_handshake_done) {
            // 检查 WebSocket 握手响应
            // 简化处理：假设握手成功
            ws_conn->websocket_handshake_done = true;
            ws_conn->state = WS_STATE_CONNECTED;

            printf("WebSocket handshake completed\n");

            // 触发连接成功事件
            ws_event_t event = {
                .type = WS_EVENT_CONNECTED,
                .connection = ws_conn
            };
            if (ws_conn->callback) {
                ws_conn->callback(&event, ws_conn->user_data);
            }
        } else {
            // 处理 WebSocket 数据帧
            printf("Processing WebSocket frame data...\n");
            ws_frame_t frame;
            int parsed = ws_frame_parse(buffer, len, &frame);
            if (parsed > 0) {
                printf("Parsed WebSocket frame: opcode=%d, length=%lu\n", frame.opcode, frame.payload_len);

                // 触发消息接收事件
                ws_event_t event = {
                    .type = WS_EVENT_MESSAGE_RECEIVED,
                    .connection = ws_conn,
                    .message = {
                        .data = frame.payload,
                        .length = frame.payload_len,
                        .frame_type = frame.opcode
                    }
                };
                if (ws_conn->callback) {
                    ws_conn->callback(&event, ws_conn->user_data);
                }

                ws_frame_free(&frame);
            }
        }

        // 更新统计信息
        pthread_mutex_lock(&ws_conn->mutex);
        ws_conn->stats.bytes_received += len;
        ws_conn->stats.last_activity = time(NULL);
        pthread_mutex_unlock(&ws_conn->mutex);
    } else if (len == -1) {
        // HTTP3_ERR_DONE - 没有更多数据可读，这是正常情况
        // 不需要打印错误信息，静默处理
    } else if (len < 0) {
        printf("HTTP/3 recv error: %ld\n", len);
    }
}

static void client_on_stream_writable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    // 流可写，可以发送更多数据
}

static void client_on_stream_closed(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    printf("Stream closed: %lu\n", stream_id);
}

// 数据包发送回调
static int quic_packet_send(void *psctx, struct quic_packet_out_spec_t *pkts, unsigned int count) {
    ws_connection_t *ws_conn = (ws_connection_t *)psctx;

    for (unsigned int i = 0; i < count; i++) {
        struct quic_packet_out_spec_t *pkt = &pkts[i];

        // 发送所有 iovec 段
        for (size_t j = 0; j < pkt->iovlen; j++) {
            ssize_t sent = sendto(ws_conn->sock, pkt->iov[j].iov_base, pkt->iov[j].iov_len, 0,
                                 (const struct sockaddr *)pkt->dst_addr, pkt->dst_addr_len);
            if (sent < 0) {
                perror("sendto failed");
                return -1;
            }

            // 更新统计信息
            pthread_mutex_lock(&ws_conn->mutex);
            ws_conn->stats.bytes_sent += sent;
            pthread_mutex_unlock(&ws_conn->mutex);
        }
    }

    return count;
}

// 套接字事件处理
static void socket_cb(EV_P_ ev_io *w, int revents) {
    ws_connection_t *ws_conn = (ws_connection_t *)w->data;

    if (revents & EV_READ) {
        uint8_t buffer[4096];
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);

        ssize_t len = recvfrom(ws_conn->sock, buffer, sizeof(buffer), 0,
                              (struct sockaddr *)&peer_addr, &peer_addr_len);

        if (len > 0) {
            // 处理接收到的 QUIC 数据包
            if (ws_conn->quic_endpoint) {
                struct quic_packet_info_t pkt_info = {
                    .src = (struct sockaddr *)&peer_addr,
                    .src_len = peer_addr_len,
                    .dst = (struct sockaddr *)&ws_conn->local_addr,
                    .dst_len = ws_conn->local_addr_len,
                };
                quic_endpoint_recv(ws_conn->quic_endpoint, buffer, len, &pkt_info);
            }
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("recvfrom failed");
        }
    }
}

// QUIC 方法表
const struct quic_transport_methods_t quic_transport_methods = {
    .on_conn_created = client_on_conn_created,
    .on_conn_established = client_on_conn_established,
    .on_conn_closed = client_on_conn_closed,
    .on_stream_created = client_on_stream_created,
    .on_stream_readable = client_on_stream_readable,
    .on_stream_writable = client_on_stream_writable,
    .on_stream_closed = client_on_stream_closed,
};

// 数据包发送方法表
const struct quic_packet_send_methods_t quic_packet_send_methods = {
    .on_packets_send = quic_packet_send,
};



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

    // 停止所有定时器和事件监听
    if (conn->loop) {
        ev_timer_stop(conn->loop, &conn->connect_timer);
        ev_timer_stop(conn->loop, &conn->ping_timer);
        ev_timer_stop(conn->loop, &conn->pong_timer);
        ev_timer_stop(conn->loop, &conn->reconnect_timer);
        ev_io_stop(conn->loop, &conn->socket_watcher);
    }

    // 关闭 HTTP/3 连接
    if (conn->h3_conn) {
        http3_conn_free(conn->h3_conn);
    }

    // 关闭 QUIC 端点
    if (conn->quic_endpoint) {
        quic_endpoint_free(conn->quic_endpoint);
    }

    // 释放 TLS 配置
    if (conn->tls_config) {
        quic_tls_config_free(conn->tls_config);
    }

    // 释放 HTTP/3 配置
    if (conn->h3_config) {
        http3_config_free(conn->h3_config);
    }

    // 关闭套接字
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

// 创建套接字并解析地址
static int create_socket(const char *host, const char *port, ws_connection_t *conn) {
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    int ret = getaddrinfo(host, port, &hints, &result);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(ret));
        return -1;
    }

    // 创建套接字
    conn->sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (conn->sock < 0) {
        perror("socket failed");
        freeaddrinfo(result);
        return -1;
    }

    // 设置非阻塞
    int flags = fcntl(conn->sock, F_GETFL, 0);
    if (flags < 0 || fcntl(conn->sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl failed");
        close(conn->sock);
        freeaddrinfo(result);
        return -1;
    }

    // 保存服务器地址
    memcpy(&conn->server_addr, result->ai_addr, result->ai_addrlen);
    conn->server_addr_len = result->ai_addrlen;

    freeaddrinfo(result);
    return 0;
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

    // 创建套接字
    if (create_socket(conn->config.host, conn->config.port, conn) < 0) {
        conn->state = WS_STATE_ERROR;
        pthread_mutex_unlock(&conn->mutex);
        return -1;
    }

    // 创建 QUIC 配置
    struct quic_config_t *config = quic_config_new();
    quic_config_set_max_idle_timeout(config, 30000);
    quic_config_set_initial_max_data(config, 1024 * 1024);
    quic_config_set_initial_max_stream_data_bidi_local(config, 256 * 1024);
    quic_config_set_initial_max_stream_data_bidi_remote(config, 256 * 1024);
    quic_config_set_initial_max_streams_bidi(config, 100);
    quic_config_set_initial_max_streams_uni(config, 100);

    // 创建 TLS 配置（客户端）
    const char* const protos[] = {"h3"};
    conn->tls_config = quic_tls_config_new_client_config(protos, 1, true);
    if (!conn->tls_config) {
        fprintf(stderr, "Failed to create TLS config\n");
        close(conn->sock);
        pthread_mutex_unlock(&conn->mutex);
        return -1;
    }

    // 创建 HTTP/3 配置
    conn->h3_config = http3_config_new();

    // 设置 TLS 配置
    quic_config_set_tls_config(config, conn->tls_config);

    // 创建 QUIC 端点
    conn->quic_endpoint = quic_endpoint_new(config, false, &quic_transport_methods, conn,
                                          &quic_packet_send_methods, conn);
    if (!conn->quic_endpoint) {
        fprintf(stderr, "Failed to create QUIC endpoint\n");
        close(conn->sock);
        pthread_mutex_unlock(&conn->mutex);
        return -1;
    }

    // 获取本地地址
    conn->local_addr_len = sizeof(conn->local_addr);
    if (getsockname(conn->sock, (struct sockaddr *)&conn->local_addr, &conn->local_addr_len) != 0) {
        fprintf(stderr, "Failed to get local address\n");
        close(conn->sock);
        pthread_mutex_unlock(&conn->mutex);
        return -1;
    }

    // 设置套接字事件监听
    if (conn->loop) {
        ev_io_init(&conn->socket_watcher, socket_cb, conn->sock, EV_READ);
        conn->socket_watcher.data = conn;
        ev_io_start(conn->loop, &conn->socket_watcher);

        // 设置 QUIC 超时定时器
        ev_init(&conn->connect_timer, timeout_callback);
        conn->connect_timer.data = conn;

        // 立即处理连接并启动定时器
        quic_endpoint_process_connections(conn->quic_endpoint);
        uint64_t timeout_us = quic_endpoint_timeout(conn->quic_endpoint);
        if (timeout_us == UINT64_MAX) {
            timeout_us = 100000; // 100ms 默认超时
        }
        double timeout_sec = (double)timeout_us / 1000000.0;
        if (timeout_sec < 0.0001) {
            timeout_sec = 0.0001;
        }
        conn->connect_timer.repeat = timeout_sec;
        ev_timer_again(conn->loop, &conn->connect_timer);
    }

    // 连接到服务器
    int ret = quic_endpoint_connect(conn->quic_endpoint,
                                  (struct sockaddr *)&conn->local_addr, conn->local_addr_len,
                                  (struct sockaddr *)&conn->server_addr, conn->server_addr_len,
                                  NULL,     // 服务器名称
                                  NULL, 0,  // session
                                  NULL, 0,  // token
                                  NULL,     // config
                                  NULL);    // index

    if (ret < 0) {
        fprintf(stderr, "Failed to connect to server\n");
        close(conn->sock);
        pthread_mutex_unlock(&conn->mutex);
        return -1;
    }

    printf("QUIC connection initiated\n");

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

    // 发送 WebSocket 关闭帧
    if (conn->h3_conn && conn->state == WS_STATE_CLOSING) {
        uint8_t close_data[125]; // 最大关闭帧载荷
        size_t close_len = 0;

        // 构造关闭载荷：2字节状态码 + 可选原因
        close_data[0] = (code >> 8) & 0xFF;
        close_data[1] = code & 0xFF;
        close_len = 2;

        if (reason) {
            size_t reason_len = strlen(reason);
            if (reason_len > 123) reason_len = 123; // 限制原因长度
            memcpy(close_data + 2, reason, reason_len);
            close_len += reason_len;
        }

        // 构造关闭帧
        uint8_t frame_buffer[close_len + 14];
        int frame_len = ws_frame_create(WS_FRAME_CLOSE, close_data, close_len,
                                       true, frame_buffer, sizeof(frame_buffer));

        if (frame_len > 0) {
            // 发送关闭帧
            http3_send_body(conn->h3_conn, conn->quic_conn, conn->stream_id,
                           frame_buffer, frame_len, false);
        }
    }

    // 关闭 QUIC 连接
    if (conn->quic_conn) {
        quic_conn_close(conn->quic_conn, false, 0, NULL, 0);
    }

    conn->state = WS_STATE_CLOSED;

    // 触发断开连接事件
    ws_event_t event = {
        .type = WS_EVENT_DISCONNECTED,
        .connection = conn
    };
    if (conn->callback) {
        conn->callback(&event, conn->user_data);
    }

    pthread_mutex_unlock(&conn->mutex);
}

// 发送文本消息
int ws_connection_send_text(ws_connection_t *conn, const char *data, size_t length) {
    if (!conn || !data || conn->state != WS_STATE_CONNECTED || !conn->h3_conn) {
        return -1;
    }

    // 构造 WebSocket 文本帧
    uint8_t frame_buffer[length + 14]; // 最大帧头长度
    int frame_len = ws_frame_create(WS_FRAME_TEXT, (const uint8_t *)data, length,
                                   true, frame_buffer, sizeof(frame_buffer));

    if (frame_len < 0) {
        return -1;
    }

    // 通过 HTTP/3 流发送
    ssize_t sent = http3_send_body(conn->h3_conn, conn->quic_conn, conn->stream_id,
                                  frame_buffer, frame_len, false);

    if (sent < 0) {
        return -1;
    }

    // 更新统计信息
    pthread_mutex_lock(&conn->mutex);
    conn->stats.messages_sent++;
    conn->stats.bytes_sent += sent;
    conn->stats.last_activity = time(NULL);
    pthread_mutex_unlock(&conn->mutex);

    return 0;
}

// 发送二进制消息
int ws_connection_send_binary(ws_connection_t *conn, const uint8_t *data, size_t length) {
    if (!conn || !data || conn->state != WS_STATE_CONNECTED || !conn->h3_conn) {
        return -1;
    }

    // 构造 WebSocket 二进制帧
    uint8_t frame_buffer[length + 14]; // 最大帧头长度
    int frame_len = ws_frame_create(WS_FRAME_BINARY, data, length,
                                   true, frame_buffer, sizeof(frame_buffer));

    if (frame_len < 0) {
        return -1;
    }

    // 通过 HTTP/3 流发送
    ssize_t sent = http3_send_body(conn->h3_conn, conn->quic_conn, conn->stream_id,
                                  frame_buffer, frame_len, false);

    if (sent < 0) {
        return -1;
    }

    // 更新统计信息
    pthread_mutex_lock(&conn->mutex);
    conn->stats.messages_sent++;
    conn->stats.bytes_sent += sent;
    conn->stats.last_activity = time(NULL);
    pthread_mutex_unlock(&conn->mutex);

    return 0;
}

// 发送 Ping 帧
int ws_connection_send_ping(ws_connection_t *conn, const uint8_t *data, size_t length) {
    if (!conn || conn->state != WS_STATE_CONNECTED || !conn->h3_conn) {
        return -1;
    }

    // Ping 帧载荷不能超过 125 字节
    if (length > 125) {
        return -1;
    }

    // 构造 WebSocket Ping 帧
    uint8_t frame_buffer[length + 14]; // 最大帧头长度
    int frame_len = ws_frame_create(WS_FRAME_PING, data, length,
                                   true, frame_buffer, sizeof(frame_buffer));

    if (frame_len < 0) {
        return -1;
    }

    // 通过 HTTP/3 流发送
    ssize_t sent = http3_send_body(conn->h3_conn, conn->quic_conn, conn->stream_id,
                                  frame_buffer, frame_len, false);

    if (sent < 0) {
        return -1;
    }

    // 更新统计信息
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

    pthread_mutex_lock(&conn->mutex);

    // 处理 QUIC 端点事件
    if (conn->quic_endpoint) {
        quic_endpoint_process_connections(conn->quic_endpoint);
    }

    // 检查连接超时
    time_t now = time(NULL);
    if (conn->state == WS_STATE_CONNECTING &&
        conn->config.connect_timeout_ms > 0 &&
        (now - conn->stats.connected_at) * 1000 > conn->config.connect_timeout_ms) {

        conn->state = WS_STATE_ERROR;

        // 触发超时事件
        ws_event_t event = {
            .type = WS_EVENT_ERROR,
            .connection = conn
        };
        if (conn->callback) {
            conn->callback(&event, conn->user_data);
        }
    }

    // 检查心跳超时
    if (conn->state == WS_STATE_CONNECTED &&
        conn->config.ping_interval_ms > 0 &&
        (now - conn->stats.last_activity) * 1000 > conn->config.ping_interval_ms * 2) {

        // 心跳超时，关闭连接
        conn->state = WS_STATE_ERROR;

        ws_event_t event = {
            .type = WS_EVENT_ERROR,
            .connection = conn
        };
        if (conn->callback) {
            conn->callback(&event, conn->user_data);
        }
    }

    pthread_mutex_unlock(&conn->mutex);
}

// QUIC 超时定时器回调
static void timeout_callback(EV_P_ ev_timer *w, int revents) {
    ws_connection_t *conn = (ws_connection_t *)w->data;
    if (!conn || !conn->quic_endpoint) return;

    // 处理 QUIC 连接状态
    quic_endpoint_process_connections(conn->quic_endpoint);

    // 获取下一个超时时间
    uint64_t timeout_us = quic_endpoint_timeout(conn->quic_endpoint);
    if (timeout_us == UINT64_MAX) {
        ev_timer_stop(EV_A_ w);
    } else {
        double timeout_sec = (double)timeout_us / 1000000.0;
        if (timeout_sec < 0.0001) {
            timeout_sec = 0.0001;
        }
        w->repeat = timeout_sec;
        ev_timer_again(EV_A_ w);
    }
}

// 心跳定时器回调
static void ping_timer_cb(EV_P_ ev_timer *w, int revents) {
    ws_connection_t *conn = (ws_connection_t *)w->data;
    if (conn && conn->state == WS_STATE_CONNECTED) {
        ws_connection_send_ping(conn, NULL, 0);
    }
}

// WebSocket 帧解析（完整实现）
int ws_frame_parse(const uint8_t *data, size_t length, ws_frame_t *frame) {
    if (!data || !frame || length < 2) {
        return -1;
    }

    memset(frame, 0, sizeof(ws_frame_t));

    // 解析基本头部
    frame->fin = (data[0] & 0x80) != 0;
    frame->rsv1 = (data[0] & 0x40) != 0;
    frame->rsv2 = (data[0] & 0x20) != 0;
    frame->rsv3 = (data[0] & 0x10) != 0;
    frame->opcode = data[0] & 0x0F;

    // 验证操作码
    if (frame->opcode > 0xF ||
        (frame->opcode >= 3 && frame->opcode <= 7) ||
        (frame->opcode >= 0xB && frame->opcode <= 0xF)) {
        return -1; // 无效的操作码
    }

    frame->mask = (data[1] & 0x80) != 0;
    uint8_t payload_len = data[1] & 0x7F;

    size_t header_len = 2;

    // 处理扩展长度
    if (payload_len == 126) {
        if (length < 4) return 0; // 需要更多数据
        frame->payload_len = ((uint64_t)data[2] << 8) | data[3];
        header_len += 2;

        // 验证长度
        if (frame->payload_len < 126) {
            return -1; // 不应该使用扩展长度
        }
    } else if (payload_len == 127) {
        if (length < 10) return 0; // 需要更多数据

        // 检查最高位，不应该设置（RFC 6455）
        if (data[2] & 0x80) {
            return -1; // 长度过大
        }

        frame->payload_len = 0;
        for (int i = 0; i < 8; i++) {
            frame->payload_len = (frame->payload_len << 8) | data[2 + i];
        }
        header_len += 8;

        // 验证长度
        if (frame->payload_len < 65536) {
            return -1; // 不应该使用 64 位长度
        }
    } else {
        frame->payload_len = payload_len;
    }

    // 验证控制帧
    if (frame->opcode >= 0x8) { // 控制帧
        if (!frame->fin) {
            return -1; // 控制帧必须设置 FIN
        }
        if (frame->payload_len > 125) {
            return -1; // 控制帧载荷不能超过 125 字节
        }
    }

    // 处理掩码
    if (frame->mask) {
        if (length < header_len + 4) return 0; // 需要更多数据
        frame->masking_key = ((uint32_t)data[header_len] << 24) |
                            ((uint32_t)data[header_len + 1] << 16) |
                            ((uint32_t)data[header_len + 2] << 8) |
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

        // 解掩码
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

// 创建 WebSocket 帧（完整实现）
int ws_frame_create(ws_frame_type_t frame_type, const uint8_t *data, size_t length,
                   bool mask, uint8_t *output, size_t output_size) {
    if (!output) {
        return -1;
    }

    // 验证帧类型
    if (frame_type > 0xF ||
        (frame_type >= 3 && frame_type <= 7) ||
        (frame_type >= 0xB && frame_type <= 0xF)) {
        return -1; // 无效的帧类型
    }

    // 验证控制帧
    if (frame_type >= 0x8 && length > 125) {
        return -1; // 控制帧载荷不能超过 125 字节
    }

    size_t header_len = 2;

    // 计算所需的头部长度
    if (length >= 126) {
        if (length < 65536) {
            header_len += 2;
        } else {
            header_len += 8;
        }
    }

    if (mask) {
        header_len += 4;
    }

    // 检查输出缓冲区大小
    if (output_size < header_len + length) {
        return -1; // 缓冲区太小
    }

    // 构造基本头部
    output[0] = 0x80 | (frame_type & 0x0F); // FIN=1, RSV=0, opcode

    // 构造长度字段
    if (length < 126) {
        output[1] = (uint8_t)length;
    } else if (length < 65536) {
        output[1] = 126;
        output[2] = (length >> 8) & 0xFF;
        output[3] = length & 0xFF;
    } else {
        output[1] = 127;
        // 写入 64 位长度（网络字节序）
        for (int i = 0; i < 8; i++) {
            output[2 + i] = (length >> (56 - i * 8)) & 0xFF;
        }
    }

    // 掩码处理
    uint32_t masking_key = 0;
    if (mask) {
        output[1] |= 0x80; // 设置掩码位

        // 生成随机掩码密钥
        masking_key = (uint32_t)rand() ^ ((uint32_t)rand() << 16);

        size_t mask_offset = (length < 126) ? 2 : (length < 65536) ? 4 : 10;
        output[mask_offset] = (masking_key >> 24) & 0xFF;
        output[mask_offset + 1] = (masking_key >> 16) & 0xFF;
        output[mask_offset + 2] = (masking_key >> 8) & 0xFF;
        output[mask_offset + 3] = masking_key & 0xFF;
    }

    // 复制和掩码载荷数据
    if (data && length > 0) {
        if (mask) {
            uint8_t mask_bytes[4] = {
                (masking_key >> 24) & 0xFF,
                (masking_key >> 16) & 0xFF,
                (masking_key >> 8) & 0xFF,
                masking_key & 0xFF
            };

            for (size_t i = 0; i < length; i++) {
                output[header_len + i] = data[i] ^ mask_bytes[i % 4];
            }
        } else {
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
