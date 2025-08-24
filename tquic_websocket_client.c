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

// WebSocket 客户端结构
struct websocket_client {
    struct quic_endpoint_t *quic_endpoint;
    struct quic_conn_t *quic_conn;
    struct http3_conn_t *h3_conn;
    ev_timer timer;
    ev_timer message_timer;
    int sock;
    struct sockaddr_storage server_addr;
    socklen_t server_addr_len;
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len;
    struct quic_tls_config_t *tls_config;
    struct ev_loop *loop;
    struct http3_config_t *h3_config;
    uint64_t stream_id;
    websocket_state_t state;
    bool is_websocket;
    int message_count;
};

// WebSocket 帧结构
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

// 生成随机掩码
static uint32_t generate_mask(void) {
    return rand();
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

// 创建 WebSocket 帧（客户端需要掩码）
static int create_websocket_frame(uint8_t opcode, const uint8_t *payload, size_t payload_len,
                                 bool fin, bool mask, uint8_t *output, size_t output_len) {
    if (output_len < 2) return -1;
    
    size_t header_len = 2;
    output[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    
    uint8_t mask_bit = mask ? 0x80 : 0x00;
    
    if (payload_len < 126) {
        output[1] = mask_bit | payload_len;
    } else if (payload_len < 65536) {
        if (output_len < 4) return -1;
        output[1] = mask_bit | 126;
        output[2] = (payload_len >> 8) & 0xFF;
        output[3] = payload_len & 0xFF;
        header_len = 4;
    } else {
        if (output_len < 10) return -1;
        output[1] = mask_bit | 127;
        for (int i = 7; i >= 0; i--) {
            output[2 + (7 - i)] = (payload_len >> (i * 8)) & 0xFF;
        }
        header_len = 10;
    }
    
    uint32_t masking_key = 0;
    if (mask) {
        if (output_len < header_len + 4) return -1;
        masking_key = generate_mask();
        output[header_len] = (masking_key >> 24) & 0xFF;
        output[header_len + 1] = (masking_key >> 16) & 0xFF;
        output[header_len + 2] = (masking_key >> 8) & 0xFF;
        output[header_len + 3] = masking_key & 0xFF;
        header_len += 4;
    }
    
    if (output_len < header_len + payload_len) return -1;
    
    if (payload && payload_len > 0) {
        memcpy(output + header_len, payload, payload_len);
        
        // 应用掩码
        if (mask) {
            for (size_t i = 0; i < payload_len; i++) {
                output[header_len + i] ^= ((uint8_t *)&masking_key)[i % 4];
            }
        }
    }
    
    return header_len + payload_len;
}

// 发送 WebSocket 消息
static void send_websocket_message(struct websocket_client *client, uint8_t opcode,
                                  const char *message, size_t message_len) {
    if (client->state != WS_STATE_OPEN) return;
    
    uint8_t frame[READ_BUF_SIZE];
    int frame_len = create_websocket_frame(opcode, (const uint8_t *)message, message_len,
                                          true, true, frame, sizeof(frame));
    
    if (frame_len > 0) {
        ssize_t written = http3_send_body(client->h3_conn, client->quic_conn, 
                                        client->stream_id, frame, frame_len, false);
        if (written > 0) {
            fprintf(stderr, "WebSocket message sent: %.*s\n", (int)message_len, message);
        } else {
            fprintf(stderr, "Failed to send WebSocket message: %ld\n", written);
        }
    }
}

// 处理 WebSocket 消息
static void handle_websocket_message(struct websocket_client *client,
                                   struct websocket_frame *frame) {
    switch (frame->opcode) {
        case WS_FRAME_TEXT:
            fprintf(stderr, "Received WebSocket text: %.*s\n", 
                   (int)frame->payload_len, frame->payload);
            break;
            
        case WS_FRAME_BINARY:
            fprintf(stderr, "Received WebSocket binary data (%llu bytes)\n", 
                   (unsigned long long)frame->payload_len);
            break;
            
        case WS_FRAME_PING:
            fprintf(stderr, "Received WebSocket ping\n");
            send_websocket_message(client, WS_FRAME_PONG, 
                                 (const char *)frame->payload, frame->payload_len);
            break;
            
        case WS_FRAME_PONG:
            fprintf(stderr, "Received WebSocket pong\n");
            break;
            
        case WS_FRAME_CLOSE:
            fprintf(stderr, "Received WebSocket close\n");
            client->state = WS_STATE_CLOSING;
            send_websocket_message(client, WS_FRAME_CLOSE, "", 0);
            break;
            
        default:
            fprintf(stderr, "Unknown WebSocket frame type: %d\n", frame->opcode);
            break;
    }
}

// HTTP/3 事件处理器实现
static void http3_on_stream_headers(void *ctx, uint64_t stream_id,
                                   const struct http3_headers_t *headers, bool fin) {
    struct websocket_client *client = ctx;
    if (!client) return;
    
    fprintf(stderr, "HTTP/3 headers received on stream %llu\n", 
           (unsigned long long)stream_id);
    
    // 简化检查：假设收到了 WebSocket 升级响应
    if (client->is_websocket) {
        client->state = WS_STATE_OPEN;
        fprintf(stderr, "WebSocket connection established!\n");
        
        // 发送第一条消息
        send_websocket_message(client, WS_FRAME_TEXT, "Hello from TQUIC WebSocket client!", 34);
    }
}

static void http3_on_stream_data(void *ctx, uint64_t stream_id) {
    struct websocket_client *client = ctx;
    if (!client || !client->is_websocket) return;

    static uint8_t buf[READ_BUF_SIZE];
    
    while (true) {
        bool fin = false;
        ssize_t read = http3_recv_body(client->h3_conn, client->quic_conn, 
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
            
            handle_websocket_message(client, &frame);
            offset += frame_len;
        }
        
        if (fin) {
            break;
        }
    }
}

static void http3_on_stream_finished(void *ctx, uint64_t stream_id) {
    struct websocket_client *client = ctx;
    fprintf(stderr, "Stream %llu finished\n", (unsigned long long)stream_id);
    
    if (client && client->is_websocket) {
        client->state = WS_STATE_CLOSED;
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
void client_on_conn_created(void *tctx, struct quic_conn_t *conn) {
    struct websocket_client *client = tctx;
    fprintf(stderr, "WebSocket client connection created\n");
    
    // 保存连接引用
    client->quic_conn = conn;
}

void client_on_conn_established(void *tctx, struct quic_conn_t *conn) {
    struct websocket_client *client = tctx;
    
    fprintf(stderr, "WebSocket client connection established\n");
    
    // 创建 HTTP/3 连接
    client->h3_conn = http3_conn_new(conn, client->h3_config);
    if (client->h3_conn) {
        http3_conn_set_events_handler(client->h3_conn, &http3_methods, client);
        
        // 创建流并发送 WebSocket 升级请求
        int64_t stream_id = http3_stream_new(client->h3_conn, client->quic_conn);
        if (stream_id >= 0) {
            client->stream_id = stream_id;
            client->is_websocket = true;
            client->state = WS_STATE_CONNECTING;
            
            // 发送 WebSocket 升级请求
            struct http3_header_t request_headers[] = {
                {.name = (uint8_t *)":method", .name_len = 7, 
                 .value = (uint8_t *)"GET", .value_len = 3},
                {.name = (uint8_t *)":path", .name_len = 5, 
                 .value = (uint8_t *)"/", .value_len = 1},
                {.name = (uint8_t *)":scheme", .name_len = 7, 
                 .value = (uint8_t *)"https", .value_len = 5},
                {.name = (uint8_t *)":authority", .name_len = 10, 
                 .value = (uint8_t *)"localhost", .value_len = 9},
                {.name = (uint8_t *)"upgrade", .name_len = 7, 
                 .value = (uint8_t *)"websocket", .value_len = 9},
                {.name = (uint8_t *)"connection", .name_len = 10, 
                 .value = (uint8_t *)"Upgrade", .value_len = 7},
                {.name = (uint8_t *)"sec-websocket-key", .name_len = 17, 
                 .value = (uint8_t *)"dGhlIHNhbXBsZSBub25jZQ==", .value_len = 24},
                {.name = (uint8_t *)"sec-websocket-version", .name_len = 21, 
                 .value = (uint8_t *)"13", .value_len = 2}
            };
            
            int ret = http3_send_headers(client->h3_conn, client->quic_conn, stream_id,
                                       request_headers,
                                       sizeof(request_headers)/sizeof(request_headers[0]),
                                       false);  // 修复：保持流开放用于 WebSocket 通信
            
            if (ret >= 0) {
                fprintf(stderr, "WebSocket upgrade request sent\n");
            } else {
                fprintf(stderr, "Failed to send WebSocket upgrade request: %d\n", ret);
            }
        } else {
            fprintf(stderr, "Failed to create HTTP/3 stream: %ld\n", stream_id);
        }
    }
}

void client_on_conn_closed(void *tctx, struct quic_conn_t *conn) {
    struct websocket_client *client = tctx;
    
    fprintf(stderr, "WebSocket client connection closed\n");
    
    if (client->h3_conn) {
        http3_conn_free(client->h3_conn);
        client->h3_conn = NULL;
    }
    
    client->state = WS_STATE_CLOSED;
}

void client_on_stream_created(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    fprintf(stderr, "Client stream created %llu\n", (unsigned long long)stream_id);
}

void client_on_stream_readable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    struct websocket_client *client = tctx;
    
    if (client && client->h3_conn) {
        http3_conn_process_streams(client->h3_conn, conn);
    }
}

void client_on_stream_writable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    quic_stream_wantwrite(conn, stream_id, false);
}

void client_on_stream_closed(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    fprintf(stderr, "Client stream closed %llu\n", (unsigned long long)stream_id);
}

// 数据包发送处理器
int client_on_packets_send(void *psctx, struct quic_packet_out_spec_t *pkts, unsigned int count) {
    struct websocket_client *client = psctx;
    
    unsigned int sent_count = 0;
    for (unsigned int i = 0; i < count; i++) {
        struct quic_packet_out_spec_t *pkt = pkts + i;
        for (int j = 0; j < pkt->iovlen; j++) {
            const struct iovec *iov = pkt->iov + j;
            ssize_t sent = sendto(client->sock, iov->iov_base, iov->iov_len, 0,
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

const struct quic_packet_send_methods_t quic_packet_send_methods = {
    .on_packets_send = client_on_packets_send,
};

// 网络事件处理
static void read_callback(EV_P_ ev_io *w, int revents) {
    struct websocket_client *client = w->data;
    static uint8_t buf[READ_BUF_SIZE];
    
    while (true) {
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        memset(&peer_addr, 0, peer_addr_len);
        
        ssize_t read = recvfrom(client->sock, buf, sizeof(buf), 0,
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
            .dst = (struct sockaddr *)&client->local_addr,  // 修复：设置本地地址
            .dst_len = client->local_addr_len,
        };
        int processed = quic_endpoint_recv(client->quic_endpoint, buf, read, &pkt_info);
        if (processed < 0) {
            fprintf(stderr, "quic_endpoint_recv failed: %d\n", processed);
        }
    }
}

static void timeout_callback(EV_P_ ev_timer *w, int revents) {
    struct websocket_client *client = w->data;
    quic_endpoint_process_connections(client->quic_endpoint);
    
    uint64_t timeout = quic_endpoint_timeout(client->quic_endpoint);
    if (timeout == UINT64_MAX) {
        ev_timer_stop(EV_A_ w);
    } else {
        w->repeat = (double)timeout / 1000000.0;
        ev_timer_again(EV_A_ w);
    }
}

// 定期发送消息
static void message_callback(EV_P_ ev_timer *w, int revents) {
    struct websocket_client *client = w->data;
    
    if (client->state == WS_STATE_OPEN) {
        char message[256];
        snprintf(message, sizeof(message), "Test message #%d from client", ++client->message_count);
        send_websocket_message(client, WS_FRAME_TEXT, message, strlen(message));
        
        // 每5秒发送一次消息
        if (client->message_count < 10) {
            w->repeat = 5.0;
            ev_timer_again(EV_A_ w);
        } else {
            // 发送关闭帧
            send_websocket_message(client, WS_FRAME_CLOSE, "", 0);
            client->state = WS_STATE_CLOSING;
        }
    }
}

// 创建套接字
static int create_socket(const char *host, const char *port, struct addrinfo **remote,
                        struct websocket_client *client) {
    const struct addrinfo hints = {
        .ai_family = PF_UNSPEC,
        .ai_socktype = SOCK_DGRAM,
        .ai_protocol = IPPROTO_UDP
    };
    
    if (getaddrinfo(host, port, &hints, remote) != 0) {
        fprintf(stderr, "Failed to resolve host\n");
        return -1;
    }
    
    int sock = socket((*remote)->ai_family, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Failed to create socket\n");
        return -1;
    }
    
    if (fcntl(sock, F_SETFL, O_NONBLOCK) != 0) {
        fprintf(stderr, "Failed to make socket non-blocking\n");
        return -1;
    }
    
    // 保存服务器地址
    memcpy(&client->server_addr, (*remote)->ai_addr, (*remote)->ai_addrlen);
    client->server_addr_len = (*remote)->ai_addrlen;
    client->sock = sock;
    
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
    
    struct websocket_client client;
    memset(&client, 0, sizeof(client));
    
    // 初始化随机数生成器
    srand(time(NULL));
    
    // 创建事件循环
    client.loop = EV_DEFAULT;
    
    // 创建套接字
    struct addrinfo *remote = NULL;
    if (create_socket(host, port, &remote, &client) < 0) {
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
    
    // 创建 TLS 配置（客户端）
    const char* const protos[] = {"h3"};
    client.tls_config = quic_tls_config_new_client_config(protos, 1, true);
    if (!client.tls_config) {
        fprintf(stderr, "Failed to create TLS config\n");
        return 1;
    }
    
    // 创建 HTTP/3 配置
    client.h3_config = http3_config_new();

    // 设置客户端 TLS 配置（在创建端点之前）
    quic_config_set_tls_config(config, client.tls_config);

    // 创建 QUIC 端点
    client.quic_endpoint = quic_endpoint_new(config, false, &quic_transport_methods, &client,
                                           &quic_packet_send_methods, &client);
    if (!client.quic_endpoint) {
        fprintf(stderr, "Failed to create QUIC endpoint\n");
        return 1;
    }
    
    // 获取本地地址
    client.local_addr_len = sizeof(client.local_addr);
    if (getsockname(client.sock, (struct sockaddr *)&client.local_addr, &client.local_addr_len) != 0) {
        fprintf(stderr, "Failed to get local address\n");
        return 1;
    }
    
    // 连接到服务器
    int ret = quic_endpoint_connect(client.quic_endpoint,
                                  (struct sockaddr *)&client.local_addr, client.local_addr_len,
                                  (struct sockaddr *)&client.server_addr,
                                  client.server_addr_len,
                                  NULL,     // 服务器名称（像 simple_client 一样使用 NULL）
                                  NULL, 0,  // session
                                  NULL, 0,  // token
                                  NULL,     // config
                                  NULL);    // index
    if (ret < 0) {
        fprintf(stderr, "Failed to create QUIC connection: %d\n", ret);
        return 1;
    }

    // 设置事件处理
    ev_io socket_watcher;
    ev_io_init(&socket_watcher, read_callback, client.sock, EV_READ);
    socket_watcher.data = &client;
    ev_io_start(client.loop, &socket_watcher);

    ev_init(&client.timer, timeout_callback);
    client.timer.data = &client;

    // 立即处理连接并启动 timer（关键修复）
    quic_endpoint_process_connections(client.quic_endpoint);
    double timeout = quic_endpoint_timeout(client.quic_endpoint) / 1e3f;
    if (timeout < 0.0001) {
        timeout = 0.0001;
    }
    client.timer.repeat = timeout;
    ev_timer_again(client.loop, &client.timer);
    
    ev_timer_init(&client.message_timer, message_callback, 2.0, 0.0);
    client.message_timer.data = &client;
    ev_timer_start(client.loop, &client.message_timer);
    
    printf("TQUIC WebSocket Client connecting to %s:%s\n", host, port);
    
    // 启动事件循环
    ev_run(client.loop, 0);
    
    // 清理
    if (remote) freeaddrinfo(remote);
    quic_endpoint_free(client.quic_endpoint);
    quic_config_free(config);
    quic_tls_config_free(client.tls_config);
    http3_config_free(client.h3_config);
    close(client.sock);
    
    return 0;
}
