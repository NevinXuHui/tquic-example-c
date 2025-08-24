// 交互式 TQUIC WebSocket 客户端
// 支持用户输入消息进行双向通信

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

#include "tquic.h"

#define READ_BUF_SIZE 4096
#define MAX_DATAGRAM_SIZE 1200

// WebSocket 帧类型
#define WS_FRAME_CONTINUATION 0x0
#define WS_FRAME_TEXT         0x1
#define WS_FRAME_BINARY       0x2
#define WS_FRAME_CLOSE        0x8
#define WS_FRAME_PING         0x9
#define WS_FRAME_PONG         0xA

// WebSocket 状态
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
    bool connected;
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
static void send_websocket_message(struct websocket_client *client, uint8_t opcode,
                                  const char *message, size_t message_len);
static void handle_websocket_message(struct websocket_client *client,
                                   struct websocket_frame *frame);
static int parse_websocket_frame(const uint8_t *data, size_t len, struct websocket_frame *frame);
static int create_websocket_frame(uint8_t opcode, const uint8_t *payload, size_t payload_len,
                                 bool mask, bool fin, uint8_t *frame, size_t frame_size);

// HTTP/3 事件处理器前向声明
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

// 创建 WebSocket 帧
static int create_websocket_frame(uint8_t opcode, const uint8_t *payload, size_t payload_len,
                                 bool mask, bool fin, uint8_t *frame, size_t frame_size) {
    size_t header_len = 2;
    
    if (payload_len > 65535) {
        header_len = 10;
    } else if (payload_len > 125) {
        header_len = 4;
    }
    
    if (mask) {
        header_len += 4;
    }
    
    if (header_len + payload_len > frame_size) {
        return -1;
    }
    
    frame[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    
    if (payload_len > 65535) {
        frame[1] = (mask ? 0x80 : 0x00) | 127;
        for (int i = 7; i >= 0; i--) {
            frame[2 + (7 - i)] = (payload_len >> (i * 8)) & 0xFF;
        }
    } else if (payload_len > 125) {
        frame[1] = (mask ? 0x80 : 0x00) | 126;
        frame[2] = (payload_len >> 8) & 0xFF;
        frame[3] = payload_len & 0xFF;
    } else {
        frame[1] = (mask ? 0x80 : 0x00) | payload_len;
    }
    
    size_t payload_offset = header_len;
    if (mask) {
        uint32_t masking_key = generate_mask();
        payload_offset -= 4;
        frame[payload_offset] = (masking_key >> 24) & 0xFF;
        frame[payload_offset + 1] = (masking_key >> 16) & 0xFF;
        frame[payload_offset + 2] = (masking_key >> 8) & 0xFF;
        frame[payload_offset + 3] = masking_key & 0xFF;
        payload_offset += 4;
        
        // 复制并掩码载荷
        for (size_t i = 0; i < payload_len; i++) {
            frame[payload_offset + i] = payload[i] ^ ((uint8_t *)&masking_key)[i % 4];
        }
    } else {
        memcpy(frame + payload_offset, payload, payload_len);
    }
    
    return header_len + payload_len;
}

// 发送 WebSocket 消息
static void send_websocket_message(struct websocket_client *client, uint8_t opcode,
                                  const char *message, size_t message_len) {
    if (client->state != WS_STATE_OPEN) {
        printf("WebSocket not connected. Please wait for connection.\n");
        return;
    }
    
    uint8_t frame[READ_BUF_SIZE];
    int frame_len = create_websocket_frame(opcode, (const uint8_t *)message, message_len,
                                          true, true, frame, sizeof(frame));
    
    if (frame_len > 0) {
        ssize_t written = http3_send_body(client->h3_conn, client->quic_conn, 
                                        client->stream_id, frame, frame_len, false);
        if (written > 0) {
            printf("Sent: %.*s\n", (int)message_len, message);
        } else {
            printf("Failed to send message: %ld\n", written);
        }
    }
}

// 处理 WebSocket 消息
static void handle_websocket_message(struct websocket_client *client,
                                   struct websocket_frame *frame) {
    switch (frame->opcode) {
        case WS_FRAME_TEXT:
            printf("Received: %.*s\n", (int)frame->payload_len, frame->payload);
            break;
            
        case WS_FRAME_BINARY:
            printf("Received binary data (%llu bytes)\n", 
                   (unsigned long long)frame->payload_len);
            break;
            
        case WS_FRAME_PING:
            printf("Received ping\n");
            send_websocket_message(client, WS_FRAME_PONG, 
                                 (const char *)frame->payload, frame->payload_len);
            break;
            
        case WS_FRAME_PONG:
            printf("Received pong\n");
            break;
            
        case WS_FRAME_CLOSE:
            printf("Server closed connection\n");
            client->state = WS_STATE_CLOSING;
            send_websocket_message(client, WS_FRAME_CLOSE, "", 0);
            break;
            
        default:
            printf("Unknown frame type: %d\n", frame->opcode);
            break;
    }
}

// HTTP/3 事件处理器实现
static void http3_on_stream_headers(void *ctx, uint64_t stream_id,
                                   const struct http3_headers_t *headers, bool fin) {
    struct websocket_client *client = ctx;
    if (!client) return;

    printf("WebSocket upgrade successful!\n");

    if (client->is_websocket) {
        client->state = WS_STATE_OPEN;
        printf("WebSocket connection established! Type messages to send (or 'quit' to exit):\n");
    }
}

static void http3_on_stream_data(void *ctx, uint64_t stream_id) {
    struct websocket_client *client = ctx;
    if (!client || !client->is_websocket) return;

    static uint8_t buf[READ_BUF_SIZE];

    while (true) {
        ssize_t read = http3_recv_body(client->h3_conn, client->quic_conn,
                                     stream_id, buf, sizeof(buf));

        if (read < 0) {
            if (read == HTTP3_ERR_DONE) {
                break;
            }
            printf("WebSocket read error: %ld\n", read);
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
                break;
            }

            handle_websocket_message(client, &frame);
            offset += frame_len;
        }
    }
}

static void http3_on_stream_finished(void *ctx, uint64_t stream_id) {
    printf("Stream %llu finished\n", (unsigned long long)stream_id);
}

static void http3_on_stream_reset(void *ctx, uint64_t stream_id, uint64_t error_code) {
    printf("Stream %llu reset with error %llu\n",
           (unsigned long long)stream_id, (unsigned long long)error_code);
}

static void http3_on_stream_priority_update(void *ctx, uint64_t stream_id) {
    // 优先级更新处理
}

static void http3_on_conn_goaway(void *ctx, uint64_t stream_id) {
    printf("Connection goaway with stream %llu\n", (unsigned long long)stream_id);
}

// QUIC 连接事件处理器
void client_on_conn_created(void *tctx, struct quic_conn_t *conn) {
    struct websocket_client *client = tctx;
    client->quic_conn = conn;
}

void client_on_conn_established(void *tctx, struct quic_conn_t *conn) {
    struct websocket_client *client = tctx;

    printf("QUIC connection established\n");

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
                                       false);

            if (ret >= 0) {
                printf("WebSocket upgrade request sent\n");
            } else {
                printf("Failed to send WebSocket upgrade request: %d\n", ret);
            }
        }
    }
}

void client_on_conn_closed(void *tctx, struct quic_conn_t *conn) {
    struct websocket_client *client = tctx;

    printf("Connection closed\n");
    client->connected = false;

    if (client->h3_conn) {
        http3_conn_free(client->h3_conn);
        client->h3_conn = NULL;
    }
}

void client_on_stream_created(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    // Stream created
}

void client_on_stream_readable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    struct websocket_client *client = tctx;

    if (client && client->h3_conn) {
        http3_conn_process_streams(client->h3_conn, conn);
    }
}

void client_on_stream_writable(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    // Stream writable
}

void client_on_stream_closed(void *tctx, struct quic_conn_t *conn, uint64_t stream_id) {
    // Stream closed
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

            if (sent != iov->iov_len) {
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
            printf("recvfrom failed: %s\n", strerror(errno));
            return;
        }

        struct quic_packet_info_t pkt_info = {
            .src = (struct sockaddr *)&peer_addr,
            .src_len = peer_addr_len,
            .dst = (struct sockaddr *)&client->local_addr,
            .dst_len = client->local_addr_len,
        };
        int processed = quic_endpoint_recv(client->quic_endpoint, buf, read, &pkt_info);
        if (processed < 0) {
            printf("quic_endpoint_recv failed: %d\n", processed);
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

// 标准输入处理
static void stdin_callback(EV_P_ ev_io *w, int revents) {
    struct websocket_client *client = w->data;
    char line[1024];

    if (fgets(line, sizeof(line), stdin)) {
        // 移除换行符
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
            len--;
        }

        if (len == 0) return;

        // 检查退出命令
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            printf("Closing connection...\n");
            if (client->state == WS_STATE_OPEN) {
                send_websocket_message(client, WS_FRAME_CLOSE, "", 0);
            }
            ev_break(EV_A_ EVBREAK_ALL);
            return;
        }

        // 发送消息
        if (client->state == WS_STATE_OPEN) {
            send_websocket_message(client, WS_FRAME_TEXT, line, len);
        } else {
            printf("WebSocket not connected yet. Please wait...\n");
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
        printf("Failed to resolve host\n");
        return -1;
    }

    int sock = socket((*remote)->ai_family, SOCK_DGRAM, 0);
    if (sock < 0) {
        printf("Failed to create socket\n");
        return -1;
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK) != 0) {
        printf("Failed to set socket non-blocking\n");
        close(sock);
        return -1;
    }

    // 保存服务器地址
    memcpy(&client->server_addr, (*remote)->ai_addr, (*remote)->ai_addrlen);
    client->server_addr_len = (*remote)->ai_addrlen;

    return sock;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    const char *port = argv[2];

    struct websocket_client client;
    memset(&client, 0, sizeof(client));
    client.connected = true;

    srand(time(NULL));

    struct addrinfo *remote = NULL;
    client.sock = create_socket(host, port, &remote, &client);
    if (client.sock < 0) {
        return 1;
    }

    // 创建 TLS 配置
    const char* const protos[] = {"h3"};
    client.tls_config = quic_tls_config_new_client_config(protos, 1, true);
    if (!client.tls_config) {
        printf("Failed to create TLS config\n");
        return 1;
    }

    // 创建 HTTP/3 配置
    client.h3_config = http3_config_new();

    // 创建 QUIC 配置
    quic_config_t *config = quic_config_new();
    quic_config_set_max_idle_timeout(config, 30000);
    quic_config_set_tls_config(config, client.tls_config);

    // 创建 QUIC 端点
    client.quic_endpoint = quic_endpoint_new(config, false, &quic_transport_methods, &client,
                                           &quic_packet_send_methods, &client);
    if (!client.quic_endpoint) {
        printf("Failed to create QUIC endpoint\n");
        return 1;
    }

    // 获取本地地址
    client.local_addr_len = sizeof(client.local_addr);
    if (getsockname(client.sock, (struct sockaddr *)&client.local_addr, &client.local_addr_len) != 0) {
        printf("Failed to get local address\n");
        return 1;
    }

    // 连接到服务器
    int ret = quic_endpoint_connect(client.quic_endpoint,
                                  (struct sockaddr *)&client.local_addr, client.local_addr_len,
                                  (struct sockaddr *)&client.server_addr,
                                  client.server_addr_len,
                                  NULL, NULL, 0, NULL, 0, NULL, NULL);
    if (ret < 0) {
        printf("Failed to create QUIC connection: %d\n", ret);
        return 1;
    }

    // 设置事件处理
    client.loop = ev_default_loop(0);

    ev_io socket_watcher;
    ev_io_init(&socket_watcher, read_callback, client.sock, EV_READ);
    socket_watcher.data = &client;
    ev_io_start(client.loop, &socket_watcher);

    ev_init(&client.timer, timeout_callback);
    client.timer.data = &client;

    // 立即处理连接并启动 timer
    quic_endpoint_process_connections(client.quic_endpoint);
    double timeout = quic_endpoint_timeout(client.quic_endpoint) / 1e3f;
    if (timeout < 0.0001) {
        timeout = 0.0001;
    }
    client.timer.repeat = timeout;
    ev_timer_again(client.loop, &client.timer);

    // 设置标准输入监听
    ev_io stdin_watcher;
    ev_io_init(&stdin_watcher, stdin_callback, STDIN_FILENO, EV_READ);
    stdin_watcher.data = &client;
    ev_io_start(client.loop, &stdin_watcher);

    printf("Connecting to %s:%s...\n", host, port);

    // 启动事件循环
    ev_run(client.loop, 0);

    // 清理
    if (remote) freeaddrinfo(remote);
    if (client.tls_config) quic_tls_config_free(client.tls_config);
    if (client.h3_config) http3_config_free(client.h3_config);
    if (client.quic_endpoint) quic_endpoint_free(client.quic_endpoint);
    if (client.sock > 0) close(client.sock);
    if (config) quic_config_free(config);

    printf("Goodbye!\n");
    return 0;
}
