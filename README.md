# tquic-example-c

C examples of using [TQUIC](https://github.com/Tencent/tquic) on Linux.

**simple_server**

A simple http/0.9 server responsing "OK" to any requests.

The certificate and private key are hard coded to "cert.crt" and "cert.key".

The first argument is the listening IP and the second is the listening port.

**simple_client**

A simple http/0.9 client.

The first argument is the destination IP and the second is the destination port.

**tquic_websocket_server**

基于 TQUIC 的 WebSocket 服务器，支持 WebSocket over HTTP/3。

需要证书文件 "cert.crt" 和私钥文件 "cert.key"。

第一个参数是监听 IP，第二个参数是监听端口。

**tquic_websocket_client**

基于 TQUIC 的 WebSocket 客户端，支持 WebSocket over HTTP/3。

第一个参数是服务器 IP，第二个参数是服务器端口。

## Requirements

Refer to the [TQUIC](https://tquic.net/docs/getting_started/installation#prerequisites) prerequisites.

## Build

```shell
make
```

## Run simple_server

```shell
./simple_server 0.0.0.0 4433
```

## Run simple_client

```shell
./simple_client 127.0.0.1 4433
```

## Run tquic_websocket_server

```shell
./tquic_websocket_server 0.0.0.0 4433
```

## Run tquic_websocket_client

```shell
./tquic_websocket_client 127.0.0.1 4433
```

## WebSocket 测试说明

WebSocket 实现特性：
- 基于 TQUIC HTTP/3 的 WebSocket 协议
- 支持文本和二进制消息
- 支持 ping/pong 心跳
- 自动回显服务器功能
- 客户端会发送多条测试消息并自动关闭连接

注意：需要先生成证书文件才能运行：
```shell
openssl req -x509 -newkey rsa:2048 -keyout cert.key -out cert.crt -days 365 -nodes -subj "/CN=localhost"
```