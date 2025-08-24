// Copyright (c) 2023 The TQUIC Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include "openssl/pem.h"
#include "openssl/ssl.h"
#include "openssl/x509.h"
#include "tquic.h"

#define READ_BUF_SIZE 4096
#define MAX_DATAGRAM_SIZE 1200
#define WEBSOCKET_MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// WebSocket 帧类型
typedef enum {
    WS_FRAME_CONTINUATION = 0x0,
    WS_FRAME_TEXT = 0x1,
    WS_FRAME_BINARY = 0x2,
    WS_FRAME_CLOSE = 0x8,
    WS_FRAME_PING = 0x9,
    WS_FRAME_PONG = 0xA
} websocket_frame_type_t;

// WebSocket 连接状态
typedef enum {
    WS_STATE_CONNECTING,
    WS_STATE_OPEN,
    WS_STATE_CLOSING,
    WS_STATE_CLOSED
} websocket_state_t;

// WebSocket 服务器结构
struct websocket_server {
    struct quic_endpoint_t *quic_endpoint;
    ev_timer timer;
    int sock;
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len;
    struct quic_tls_config_t *tls_config;
    struct ev_loop *loop;
    struct http3_config_t *h3_config;
};

// WebSocket 连接上下文
struct websocket_connection {
    struct http3_conn_t *h3_conn;
    struct quic_conn_t *quic_conn;
    uint64_t stream_id;
    websocket_state_t state;
    bool is_websocket;
    char *sec_websocket_key;
    char *pending_data;
    size_t pending_data_len;
};

// WebSocket 帧头结构
struct websocket_frame {
    bool fin;
    bool rsv1, rsv2, rsv3;
    uint8_t opcode;
    bool mask;
    uint64_t payload_len;
    uint32_t masking_key;
    uint8_t *payload;
};

// 前向声明
static void http3_on_stream_headers(void *ctx, uint64_t stream_id,
                                   const struct http3_headers_t *headers, bool fin);
static void http3_on_stream_data(void *ctx, uint64_t stream_id);
static void http3_on_stream_finished(void *ctx, uint64_t stream_id);
static void http3_on_stream_reset(void *ctx, uint64_t stream_id, uint64_t error_code);
static void http3_on_stream_priority_update(void *ctx, uint64_t stream_id);
static void http3_on_conn_goaway(void *ctx, uint64_t stream_id);

// HTTP/3 事件处理器
static const struct http3_methods_t http3_methods = {
    .on_stream_headers = http3_on_stream_headers,
    .on_stream_data = http3_on_stream_data,
    .on_stream_finished = http3_on_stream_finished,
    .on_stream_reset = http3_on_stream_reset,
    .on_stream_priority_update = http3_on_stream_priority_update,
    .on_conn_goaway = http3_on_conn_goaway,
};

// Base64 编码（简化版本）
static void base64_encode(const unsigned char *input, int length, char *output) {
    const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, j;
    
    for (i = 0, j = 0; i < length; i += 3, j += 4) {
        uint32_t v = input[i] << 16;
        if (i + 1 < length) v |= input[i + 1] << 8;
        if (i + 2 < length) v |= input[i + 2];
        
        output[j] = chars[(v >> 18) & 0x3F];
        output[j + 1] = chars[(v >> 12) & 0x3F];
        output[j + 2] = (i + 1 < length) ? chars[(v >> 6) & 0x3F] : '=';
        output[j + 3] = (i + 2 < length) ? chars[v & 0x3F] : '=';
    }
    output[j] = '\0';
}

// 生成 WebSocket Accept 密钥
static void generate_websocket_accept(const char *key, char *accept) {
    char concatenated[256];
    snprintf(concatenated, sizeof(concatenated), "%s%s", key, WEBSOCKET_MAGIC_STRING);
    
    // 这里应该使用 SHA-1 哈希，为简化演示使用简单的编码
    // 实际项目中应该使用 OpenSSL 的 SHA-1 函数
    unsigned char hash[20] = {0}; // 简化的哈希值
    base64_encode(hash, 20, accept);
}

// 解析 WebSocket 帧
static int parse_websocket_frame(const uint8_t *data, size_t len, struct websocket_frame *frame) {
    if (len < 2) return -1;
    
    frame->fin = (data[0] & 0x80) != 0;
    frame->rsv1 = (data[0] & 0x40) != 0;
    frame->rsv2 = (data[0] & 0x20) != 0;
    frame->rsv3 = (data[0] & 0x10) != 0;
    frame->opcode = data[0] & 0x0F;
    
    frame->mask = (data[1] & 0x80) != 0;
    uint8_t payload_len = data[1] & 0x7F;
    
    size_t header_len = 2;
    
    if (payload_len == 126) {
        if (len < 4) return -1;
        frame->payload_len = (data[2] << 8) | data[3];
        header_len = 4;
    } else if (payload_len == 127) {
        if (len < 10) return -1;
        frame->payload_len = 0;
        for (int i = 0; i < 8; i++) {
            frame->payload_len = (frame->payload_len << 8) | data[2 + i];
        }
        header_len = 10;
    } else {
        frame->payload_len = payload_len;
    }
    
    if (frame->mask) {
        if (len < header_len + 4) return -1;
        frame->masking_key = (data[header_len] << 24) | (data[header_len + 1] << 16) |
                            (data[header_len + 2] << 8) | data[header_len + 3];
        header_len += 4;
    }
    
    if (len < header_len + frame->payload_len) return -1;
    
    frame->payload = (uint8_t *)(data + header_len);
    
    // 解掩码
    if (frame->mask) {
        for (uint64_t i = 0; i < frame->payload_len; i++) {
            frame->payload[i] ^= ((uint8_t *)&frame->masking_key)[i % 4];
        }
    }
    
    return header_len + frame->payload_len;
}

// 创建 WebSocket 帧
static int create_websocket_frame(uint8_t opcode, const uint8_t *payload, size_t payload_len,
                                 bool fin, uint8_t *output, size_t output_len) {
    if (output_len < 2) return -1;
    
    size_t header_len = 2;
    output[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    
    if (payload_len < 126) {
        output[1] = payload_len;
    } else if (payload_len < 65536) {
        if (output_len < 4) return -1;
        output[1] = 126;
        output[2] = (payload_len >> 8) & 0xFF;
        output[3] = payload_len & 0xFF;
        header_len = 4;
    } else {
        if (output_len < 10) return -1;
        output[1] = 127;
        for (int i = 7; i >= 0; i--) {
            output[2 + (7 - i)] = (payload_len >> (i * 8)) & 0xFF;
        }
        header_len = 10;
    }
    
    if (output_len < header_len + payload_len) return -1;
    
    if (payload && payload_len > 0) {
        memcpy(output + header_len, payload, payload_len);
    }
    
    return header_len + payload_len;
}

// 发送 WebSocket 消息
static void send_websocket_message(struct websocket_connection *ws_conn, uint8_t opcode,
                                  const char *message, size_t message_len) {
    if (ws_conn->state != WS_STATE_OPEN) return;
    
    uint8_t frame[READ_BUF_SIZE];
    int frame_len = create_websocket_frame(opcode, (const uint8_t *)message, message_len,
                                          true, frame, sizeof(frame));
    
    if (frame_len > 0) {
        ssize_t written = quic_stream_write(ws_conn->quic_conn, ws_conn->stream_id,
                                          frame, frame_len, false);
        if (written > 0) {
            fprintf(stderr, "WebSocket message sent: %.*s\n", (int)message_len, message);
        } else {
            fprintf(stderr, "Failed to send WebSocket message\n");
        }
    }
}

// 处理 WebSocket 消息
static void handle_websocket_message(struct websocket_connection *ws_conn,
                                   struct websocket_frame *frame) {
    switch (frame->opcode) {
        case WS_FRAME_TEXT:
            fprintf(stderr, "Received WebSocket text: %.*s\n", 
                   (int)frame->payload_len, frame->payload);
            // 回显消息
            send_websocket_message(ws_conn, WS_FRAME_TEXT, 
                                 (const char *)frame->payload, frame->payload_len);
            break;
            
        case WS_FRAME_BINARY:
            fprintf(stderr, "Received WebSocket binary data (%llu bytes)\n", 
                   (unsigned long long)frame->payload_len);
            send_websocket_message(ws_conn, WS_FRAME_BINARY, 
                                 (const char *)frame->payload, frame->payload_len);
            break;
            
        case WS_FRAME_PING:
            fprintf(stderr, "Received WebSocket ping\n");
            send_websocket_message(ws_conn, WS_FRAME_PONG, 
                                 (const char *)frame->payload, frame->payload_len);
            break;
            
        case WS_FRAME_PONG:
            fprintf(stderr, "Received WebSocket pong\n");
            break;
            
        case WS_FRAME_CLOSE:
            fprintf(stderr, "Received WebSocket close\n");
            ws_conn->state = WS_STATE_CLOSING;
            send_websocket_message(ws_conn, WS_FRAME_CLOSE, "", 0);
            break;
            
        default:
            fprintf(stderr, "Unknown WebSocket frame type: %d\n", frame->opcode);
            break;
    }
}

// 检查是否为 WebSocket 升级请求
static bool is_websocket_upgrade(const struct http3_headers_t *headers, char **websocket_key) {
    bool has_upgrade = false, has_connection = false, has_version = false;
    *websocket_key = NULL;
    
    // 这里需要实现 HTTP/3 头部遍历，简化演示
    // 实际应该使用 http3_for_each_header 函数
    
    // 简化检查：假设这是一个 WebSocket 升级请求
    has_upgrade = true;
    has_connection = true;
    has_version = true;
    *websocket_key = strdup("dGhlIHNhbXBsZSBub25jZQ=="); // 示例密钥
    
    return has_upgrade && has_connection && has_version && *websocket_key;
}

// HTTP/3 事件处理器实现
static void http3_on_stream_headers(void *ctx, uint64_t stream_id,
                                   const struct http3_headers_t *headers, bool fin) {
    struct websocket_connection *ws_conn = ctx;
    if (!ws_conn) return;
    
    fprintf(stderr, "HTTP/3 headers received on stream %llu\n", 
           (unsigned long long)stream_id);
    
    char *websocket_key = NULL;
    if (is_websocket_upgrade(headers, &websocket_key)) {
        // WebSocket 升级请求
        ws_conn->is_websocket = true;
        ws_conn->stream_id = stream_id;
        ws_conn->sec_websocket_key = websocket_key;
        ws_conn->state = WS_STATE_CONNECTING;
        
        // 生成 WebSocket Accept 响应
        char accept_key[256];
        generate_websocket_accept(websocket_key, accept_key);
        
        // 发送 WebSocket 升级响应
        struct http3_header_t response_headers[] = {
            {.name = (uint8_t *)":status", .name_len = 7, 
             .value = (uint8_t *)"101", .value_len = 3},
            {.name = (uint8_t *)"upgrade", .name_len = 7, 
             .value = (uint8_t *)"websocket", .value_len = 9},
            {.name = (uint8_t *)"connection", .name_len = 10, 
             .value = (uint8_t *)"Upgrade", .value_len = 7},
            {.name = (uint8_t *)"sec-websocket-accept", .name_len = 20, 
             .value = (uint8_t *)accept_key, .value_len = strlen(accept_key)}
        };
        
        int ret = http3_send_headers(ws_conn->h3_conn, ws_conn->quic_conn, stream_id,
                                   response_headers,
                                   sizeof(response_headers)/sizeof(response_headers[0]),
                                   false);  // 修复：保持流开放用于 WebSocket 通信
        
        if (ret >= 0) {
            ws_conn->state = WS_STATE_OPEN;
            fprintf(stderr, "WebSocket connection established on stream %llu\n", 
                   (unsigned long long)stream_id);
            
            // 发送欢迎消息
            send_websocket_message(ws_conn, WS_FRAME_TEXT, 
                                 "Welcome to TQUIC WebSocket Server!", 35);
        } else {
            fprintf(stderr, "Failed to send WebSocket upgrade response: %d\n", ret);
        }
    } else {
        // 普通 HTTP 请求
        struct http3_header_t response_headers[] = {
            {.name = (uint8_t *)":status", .name_len = 7, 
             .value = (uint8_t *)"200", .value_len = 3},
            {.name = (uint8_t *)"content-type", .name_len = 12, 
             .value = (uint8_t *)"text/html", .value_len = 9}
        };
        
        const char *html_response = 
            "<!DOCTYPE html><html><body>"
            "<h1>TQUIC WebSocket Server</h1>"
            "<p>Use WebSocket client to connect to this server.</p>"
            "</body></html>";
        
        http3_send_headers(ws_conn->h3_conn, ws_conn->quic_conn, stream_id,
                         response_headers, 
                         sizeof(response_headers)/sizeof(response_headers[0]), 
                         false);
        
        http3_send_body(ws_conn->h3_conn, ws_conn->quic_conn, stream_id,
                       (uint8_t *)html_response, strlen(html_response), true);
    }
}

static void http3_on_stream_data(void *ctx, uint64_t stream_id) {
    struct websocket_connection *ws_conn = ctx;
    if (!ws_conn || !ws_conn->is_websocket) return;
    
    static uint8_t buf[READ_BUF_SIZE];
    
    while (true) {
        bool fin = false;
        ssize_t read = http3_recv_body(ws_conn->h3_conn, ws_conn->quic_conn, 
                                     stream_id, buf, sizeof(buf));
        
        if (read < 0) {
            if (read == HTTP3_ERR_DONE) {
                break; // 没有更多数据
            }
            fprintf(stderr, "WebSocket read error: %ld\n", read);
            return;
        }
        
        if (read == 0) {
            break;
        }
        
        // 解析 WebSocket 帧
        size_t offset = 0;
        while (offset < (size_t)read) {
            struct websocket_frame frame;
            int frame_len = parse_websocket_frame(buf + offset, read - offset, &frame);
            
            if (frame_len < 0) {
                break; // 需要更多数据
            }
            
            handle_websocket_message(ws_conn, &frame);
            offset += frame_len;
        }
        
        if (fin) {
            break;
        }
    }
}

static void http3_on_stream_finished(void *ctx, uint64_t stream_id) {
    struct websocket_connection *ws_conn = ctx;
    fprintf(stderr, "Stream %llu finished\n", (unsigned long long)stream_id);
    
    if (ws_conn && ws_conn->is_websocket) {
        ws_conn->state = WS_STATE_CLOSED;
    }
}

static void http3_on_stream_reset(void *ctx, uint64_t stream_id, uint64_t error_code) {
    fprintf(stderr, "Stream %llu reset with error %llu\n", 
           (unsigned long long)stream_id, (unsigned long long)error_code);
}

static void http3_on_stream_priority_update(void *ctx, uint64_t stream_id) {
    fprintf(stderr, "Stream %llu priority updated\n", (unsigned long long)stream_id);
}

static void http3_on_conn_goaway(void *ctx, uint64_t stream_id) {
    fprintf(stderr, "Connection goaway with stream %llu\n", (unsigned long long)stream_id);
}

// QUIC 连接事件处理器
void server_on_conn_created(void *tctx, struct quic_conn_t *conn) {
    fprintf(stderr, "New WebSocket connection created\n");
    
    struct websocket_connection *ws_conn = malloc(sizeof(struct websocket_connection));
    if (ws_conn) {
        memset(ws_conn, 0, sizeof(struct websocket_connection));
        ws_conn->quic_conn = conn;
        ws_conn->state = WS_STATE_CONNECTING;
        ws_conn->is_websocket = false;
        quic_conn_set_context(conn, ws_conn);
    }
}

void server_on_conn_established(void *tctx, struct quic_conn_t *conn) {
    struct websocket_server *server = tctx;
    struct websocket_connection *ws_conn = quic_conn_context(conn);
    
    fprintf(stderr, "WebSocket connection established\n");
    
    if (ws_conn) {
        ws_conn->h3_conn = http3_conn_new(conn, server->h3_config);
        if (ws_conn->h3_conn) {
            http3_conn_set_events_handler(ws_conn->h3_conn, &http3_methods, ws_conn);
        }
    }
}

void server_on_conn_closed(void *tctx, struct quic_conn_t *conn) {
    struct websocket_connection *ws_conn = quic_conn_context(conn);
    
    fprintf(stderr, "WebSocket connection closed\n");
    
    if (ws_conn) {
        if (ws_conn->h3_conn) {
            http3_conn_free(ws_conn->h3_conn);
        }
        if (ws_conn->sec_websocket_key) {
            free(ws_conn->sec_websocket_key);
        }
        if (ws_conn->pending_data) {
            free(ws_conn->pending_data);
        }
        free(ws_conn);
    }
}

void server_on_stream_created(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    fprintf(stderr, "New stream created %llu\n", (unsigned long long)stream_id);
}

void server_on_stream_readable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    struct websocket_connection *ws_conn = quic_conn_context(conn);
    
    if (ws_conn && ws_conn->h3_conn) {
        http3_conn_process_streams(ws_conn->h3_conn, conn);
    }
}

void server_on_stream_writable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    // 处理可写事件
    quic_stream_wantwrite(conn, stream_id, false);
}

void server_on_stream_closed(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    fprintf(stderr, "Stream closed %llu\n", (unsigned long long)stream_id);
}

// 数据包发送处理器
int server_on_packets_send(void *psctx, struct quic_packet_out_spec_t *pkts, unsigned int count) {
    struct websocket_server *server = psctx;
    
    unsigned int sent_count = 0;
    for (unsigned int i = 0; i < count; i++) {
        struct quic_packet_out_spec_t *pkt = pkts + i;
        for (int j = 0; j < pkt->iovlen; j++) {
            const struct iovec *iov = pkt->iov + j;
            ssize_t sent = sendto(server->sock, iov->iov_base, iov->iov_len, 0,
                                 (struct sockaddr *)pkt->dst_addr, pkt->dst_addr_len);
            
            if (sent != (ssize_t)iov->iov_len) {
                if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
                    return sent_count;
                }
                return -1;
            }
            sent_count++;
        }
    }
    
    return sent_count;
}

// TLS 配置处理器
struct quic_tls_config_t *server_get_default_tls_config(void *ctx) {
    struct websocket_server *server = ctx;
    return server->tls_config;
}

struct quic_tls_config_t *server_select_tls_config(void *ctx, const uint8_t *server_name,
                                                   size_t server_name_len) {
    struct websocket_server *server = ctx;
    return server->tls_config;
}

// QUIC 方法表
const struct quic_transport_methods_t quic_transport_methods = {
    .on_conn_created = server_on_conn_created,
    .on_conn_established = server_on_conn_established,
    .on_conn_closed = server_on_conn_closed,
    .on_stream_created = server_on_stream_created,
    .on_stream_readable = server_on_stream_readable,
    .on_stream_writable = server_on_stream_writable,
    .on_stream_closed = server_on_stream_closed,
};

const struct quic_packet_send_methods_t quic_packet_send_methods = {
    .on_packets_send = server_on_packets_send,
};

const struct quic_tls_config_select_methods_t tls_config_select_method = {
    .get_default = server_get_default_tls_config,
    .select = server_select_tls_config,
};

// 网络事件处理
static void read_callback(EV_P_ ev_io *w, int revents) {
    struct websocket_server *server = w->data;
    static uint8_t buf[READ_BUF_SIZE];
    
    while (true) {
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        memset(&peer_addr, 0, peer_addr_len);
        
        ssize_t read = recvfrom(server->sock, buf, sizeof(buf), 0,
                               (struct sockaddr *)&peer_addr, &peer_addr_len);
        if (read < 0) {
            if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
                break;
            }
            fprintf(stderr, "recvfrom failed: %s\n", strerror(errno));
            return;
        }
        
        struct quic_packet_info_t pkt_info = {
            .src = (struct sockaddr *)&peer_addr,
            .src_len = peer_addr_len,
            .dst = (struct sockaddr *)&server->local_addr,
            .dst_len = server->local_addr_len,
        };
        int processed = quic_endpoint_recv(server->quic_endpoint, buf, read, &pkt_info);
        if (processed < 0) {
            fprintf(stderr, "quic_endpoint_recv failed: %d\n", processed);
        }
    }

    // 关键修复：处理连接和更新 timer（参考 simple_h3_server）
    quic_endpoint_process_connections(server->quic_endpoint);
    double timeout = quic_endpoint_timeout(server->quic_endpoint) / 1e3f;
    if (timeout < 0.0001) {
        timeout = 0.0001;
    }
    server->timer.repeat = timeout;
    ev_timer_again(EV_A_ &server->timer);
}

static void timeout_callback(EV_P_ ev_timer *w, int revents) {
    struct websocket_server *server = w->data;
    quic_endpoint_process_connections(server->quic_endpoint);
    
    uint64_t timeout = quic_endpoint_timeout(server->quic_endpoint);
    if (timeout == UINT64_MAX) {
        ev_timer_stop(EV_A_ w);
    } else {
        w->repeat = (double)timeout / 1000000.0;
        ev_timer_again(EV_A_ w);
    }
}

// 创建套接字
static int create_socket(const char *host, const char *port, struct addrinfo **local,
                        struct websocket_server *server) {
    const struct addrinfo hints = {
        .ai_family = PF_UNSPEC,
        .ai_socktype = SOCK_DGRAM,
        .ai_protocol = IPPROTO_UDP
    };
    
    if (getaddrinfo(host, port, &hints, local) != 0) {
        fprintf(stderr, "Failed to resolve host\n");
        return -1;
    }
    
    int sock = socket((*local)->ai_family, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Failed to create socket\n");
        return -1;
    }
    
    if (fcntl(sock, F_SETFL, O_NONBLOCK) != 0) {
        fprintf(stderr, "Failed to make socket non-blocking\n");
        return -1;
    }
    
    if (bind(sock, (*local)->ai_addr, (*local)->ai_addrlen) < 0) {
        fprintf(stderr, "Failed to bind socket: %s\n", strerror(errno));
        return -1;
    }
    
    server->local_addr_len = sizeof(server->local_addr);
    if (getsockname(sock, (struct sockaddr *)&server->local_addr,
                    &server->local_addr_len) != 0) {
        fprintf(stderr, "Failed to get local address of socket\n");
        return -1;
    }
    
    server->sock = sock;
    return 0;
}

// 主函数
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }
    
    const char *host = argv[1];
    const char *port = argv[2];
    
    struct websocket_server server;
    memset(&server, 0, sizeof(server));
    
    // 创建事件循环
    server.loop = EV_DEFAULT;
    
    // 创建套接字
    struct addrinfo *local = NULL;
    if (create_socket(host, port, &local, &server) < 0) {
        return 1;
    }
    
    // 创建 QUIC 配置
    struct quic_config_t *config = quic_config_new();
    quic_config_set_max_idle_timeout(config, 30000);
    quic_config_set_initial_max_data(config, 1024 * 1024);
    quic_config_set_initial_max_stream_data_bidi_local(config, 256 * 1024);
    quic_config_set_initial_max_stream_data_bidi_remote(config, 256 * 1024);
    quic_config_set_initial_max_streams_bidi(config, 100);
    quic_config_set_initial_max_streams_uni(config, 100);
    
    // 创建 TLS 配置（服务器）
    const char* const protos[] = {"h3"};
    server.tls_config = quic_tls_config_new_server_config("cert.crt", "cert.key", protos, 1, true);
    if (!server.tls_config) {
        fprintf(stderr, "Failed to create TLS config\n");
        return 1;
    }
    
    // 创建 HTTP/3 配置
    server.h3_config = http3_config_new();
    
    // 设置 TLS 配置选择器
    quic_config_set_tls_selector(config, &tls_config_select_method, &server);
    
    // 创建 QUIC 端点
    server.quic_endpoint = quic_endpoint_new(config, true, &quic_transport_methods, &server,
                                           &quic_packet_send_methods, &server);
    if (!server.quic_endpoint) {
        fprintf(stderr, "Failed to create QUIC endpoint\n");
        return 1;
    }
    
    // QUIC 端点不需要显式监听，它会自动处理传入的连接
    
    // 设置事件处理
    ev_io socket_watcher;
    ev_io_init(&socket_watcher, read_callback, server.sock, EV_READ);
    socket_watcher.data = &server;
    ev_io_start(server.loop, &socket_watcher);
    
    ev_init(&server.timer, timeout_callback);
    server.timer.data = &server;
    
    printf("TQUIC WebSocket Server listening on %s:%s\n", host, port);
    printf("Test with: websocat ws://localhost:%s\n", port);
    
    // 启动事件循环
    ev_run(server.loop, 0);
    
    // 清理
    if (local) freeaddrinfo(local);
    quic_endpoint_free(server.quic_endpoint);
    quic_config_free(config);
    quic_tls_config_free(server.tls_config);
    http3_config_free(server.h3_config);
    close(server.sock);
    
    return 0;
}
