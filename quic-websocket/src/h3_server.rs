use anyhow::{Context, Result};
use bytes::{Bytes, Buf};
use h3::server::RequestStream;
use h3_quinn::BidiStream;
use quinn::Endpoint;
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::sync::{broadcast, RwLock};
use tracing::{debug, error, info, warn};
use uuid::Uuid;

use crate::websocket::{WebSocketFrame, WebSocketOpcode, WebSocketState, generate_websocket_accept, is_websocket_upgrade_request};

/// HTTP/3 WebSocket 服务器
pub struct H3WebSocketServer {
    endpoint: Endpoint,
    connections: Arc<RwLock<HashMap<Uuid, H3WebSocketConnection>>>,
    broadcast_tx: broadcast::Sender<WebSocketMessage>,
    server_name: String,
}

/// HTTP/3 WebSocket 连接
#[derive(Debug)]
pub struct H3WebSocketConnection {
    pub id: Uuid,
    pub remote_addr: SocketAddr,
    pub state: WebSocketState,
}

/// WebSocket 消息
#[derive(Debug, Clone)]
pub struct WebSocketMessage {
    pub from: Uuid,
    pub opcode: WebSocketOpcode,
    pub payload: Vec<u8>,
}

impl H3WebSocketServer {
    /// 创建新的 HTTP/3 WebSocket 服务器
    pub fn new(endpoint: Endpoint, server_name: String) -> Self {
        let (broadcast_tx, _) = broadcast::channel(1000);
        
        Self {
            endpoint,
            connections: Arc::new(RwLock::new(HashMap::new())),
            broadcast_tx,
            server_name,
        }
    }

    /// 启动服务器
    pub async fn run(&self) -> Result<()> {
        info!("🚀 Starting HTTP/3 WebSocket server: {}", self.server_name);
        info!("📡 Listening on: {}", self.endpoint.local_addr()?);
        info!("🔧 Protocol: WebSocket over HTTP/3 (RFC 9220)");

        // 主循环：接受新连接
        while let Some(conn) = self.endpoint.accept().await {
            let connections = self.connections.clone();
            let broadcast_tx = self.broadcast_tx.clone();
            let server_name = self.server_name.clone();

            tokio::spawn(async move {
                if let Err(e) = Self::handle_connection(conn, connections, broadcast_tx, server_name).await {
                    error!("Connection handling error: {}", e);
                }
            });
        }

        Ok(())
    }

    /// 处理单个客户端连接
    async fn handle_connection(
        connecting: quinn::Connecting,
        connections: Arc<RwLock<HashMap<Uuid, H3WebSocketConnection>>>,
        broadcast_tx: broadcast::Sender<WebSocketMessage>,
        server_name: String,
    ) -> Result<()> {
        let connection = connecting.await.context("Failed to establish QUIC connection")?;
        let remote_addr = connection.remote_address();
        let conn_id = Uuid::new_v4();

        info!("🔗 New QUIC connection from {}, assigned ID: {}", remote_addr, conn_id);

        // 创建 HTTP/3 连接
        let mut h3_conn = h3::server::Connection::new(h3_quinn::Connection::new(connection))
            .await
            .context("Failed to create HTTP/3 connection")?;

        info!("✅ HTTP/3 connection established for {}", conn_id);

        // 处理 HTTP/3 请求
        loop {
            match h3_conn.accept().await {
                Ok(Some((req, stream))) => {
                    let connections = connections.clone();
                    let broadcast_tx = broadcast_tx.clone();
                    let server_name = server_name.clone();

                    tokio::spawn(async move {
                        if let Err(e) = Self::handle_request(
                            req, stream, conn_id, remote_addr, connections, broadcast_tx, server_name
                        ).await {
                            error!("Request handling error: {}", e);
                        }
                    });
                }
                Ok(None) => {
                    debug!("HTTP/3 connection {} closed", conn_id);
                    break;
                }
                Err(e) => {
                    error!("Error accepting HTTP/3 request: {}", e);
                    break;
                }
            }
        }

        // 清理连接
        connections.write().await.remove(&conn_id);
        info!("🧹 Connection {} cleaned up", conn_id);

        Ok(())
    }

    /// 处理 HTTP/3 请求
    async fn handle_request(
        req: http::Request<()>,
        stream: RequestStream<BidiStream<Bytes>, Bytes>,
        conn_id: Uuid,
        remote_addr: SocketAddr,
        connections: Arc<RwLock<HashMap<Uuid, H3WebSocketConnection>>>,
        broadcast_tx: broadcast::Sender<WebSocketMessage>,
        server_name: String,
    ) -> Result<()> {
        debug!("📨 Received HTTP/3 request from {}: {} {}", 
               remote_addr, req.method(), req.uri());

        // 检查是否为 WebSocket 升级请求
        if is_websocket_upgrade_request(req.headers()) {
            info!("🔄 WebSocket upgrade request from {}", remote_addr);
            
            // 处理 WebSocket 升级
            Self::handle_websocket_upgrade(
                req, stream, conn_id, remote_addr, connections, broadcast_tx, server_name
            ).await?;
        } else {
            // 处理普通 HTTP 请求
            Self::handle_http_request(req, stream).await?;
        }

        Ok(())
    }

    /// 处理 WebSocket 升级
    async fn handle_websocket_upgrade(
        req: http::Request<()>,
        mut stream: RequestStream<BidiStream<Bytes>, Bytes>,
        conn_id: Uuid,
        remote_addr: SocketAddr,
        connections: Arc<RwLock<HashMap<Uuid, H3WebSocketConnection>>>,
        broadcast_tx: broadcast::Sender<WebSocketMessage>,
        server_name: String,
    ) -> Result<()> {
        let headers = req.headers();
        
        // 获取 WebSocket 密钥
        let websocket_key = headers
            .get("sec-websocket-key")
            .and_then(|v| v.to_str().ok())
            .context("Missing or invalid WebSocket key")?;

        // 生成 Accept 密钥
        let accept_key = generate_websocket_accept(websocket_key);

        info!("🔐 Generated WebSocket Accept key for {}: {}", conn_id, accept_key);

        // 发送 WebSocket 升级响应
        let response = http::Response::builder()
            .status(101)
            .header("upgrade", "websocket")
            .header("connection", "Upgrade")
            .header("sec-websocket-accept", accept_key)
            .body(())
            .context("Failed to build WebSocket upgrade response")?;

        stream.send_response(response).await
            .context("Failed to send WebSocket upgrade response")?;

        info!("✅ WebSocket upgrade successful for connection {}", conn_id);

        // 创建 WebSocket 连接记录
        let ws_conn = H3WebSocketConnection {
            id: conn_id,
            remote_addr,
            state: WebSocketState::Open,
        };

        connections.write().await.insert(conn_id, ws_conn);

        // 发送欢迎消息
        let welcome_msg = format!("Welcome to {} (HTTP/3 WebSocket)!", server_name);
        Self::send_websocket_message(&mut stream, WebSocketOpcode::Text, welcome_msg.as_bytes()).await?;

        // 处理 WebSocket 消息
        Self::handle_websocket_messages(stream, conn_id, connections, broadcast_tx).await?;

        Ok(())
    }

    /// 处理普通 HTTP 请求
    async fn handle_http_request(
        req: http::Request<()>,
        mut stream: RequestStream<BidiStream<Bytes>, Bytes>,
    ) -> Result<()> {
        info!("📄 Handling HTTP request: {} {}", req.method(), req.uri());

        let html_response = format!(r#"
<!DOCTYPE html>
<html>
<head>
    <title>QUIC WebSocket Server (HTTP/3)</title>
    <style>
        body {{ font-family: Arial, sans-serif; margin: 40px; }}
        .header {{ color: #2c3e50; }}
        .feature {{ margin: 10px 0; padding: 10px; background: #f8f9fa; border-left: 4px solid #007bff; }}
        .code {{ background: #f4f4f4; padding: 10px; border-radius: 4px; font-family: monospace; }}
    </style>
</head>
<body>
    <h1 class="header">🚀 QUIC WebSocket Server (HTTP/3)</h1>
    <p>This server supports <strong>WebSocket over HTTP/3</strong> according to RFC 9220.</p>
    
    <div class="feature">
        <h3>✅ Supported Features:</h3>
        <ul>
            <li>WebSocket over HTTP/3 (RFC 9220)</li>
            <li>Standard WebSocket Protocol (RFC 6455)</li>
            <li>QUIC Transport Protocol</li>
            <li>TLS 1.3 Encryption</li>
            <li>Real-time Bidirectional Communication</li>
        </ul>
    </div>
    
    <div class="feature">
        <h3>🔧 How to Connect:</h3>
        <p>Use a compatible WebSocket client that supports HTTP/3:</p>
        <div class="code">
            ./tquic_websocket_client 127.0.0.1 4433<br>
            ./tquic_websocket_interactive_client 127.0.0.1 4433
        </div>
    </div>
    
    <div class="feature">
        <h3>📊 Server Status:</h3>
        <p>Server is running and ready to accept WebSocket connections.</p>
    </div>
</body>
</html>
        "#);

        let response = http::Response::builder()
            .status(200)
            .header("content-type", "text/html; charset=utf-8")
            .header("server", "quic-websocket-h3/1.0")
            .body(())
            .context("Failed to build HTTP response")?;

        stream.send_response(response).await
            .context("Failed to send HTTP response")?;

        stream.send_data(Bytes::from(html_response)).await
            .context("Failed to send HTTP response body")?;

        stream.finish().await
            .context("Failed to finish HTTP response")?;

        info!("📤 HTTP response sent successfully");
        Ok(())
    }

    /// 发送 WebSocket 消息
    async fn send_websocket_message(
        stream: &mut RequestStream<BidiStream<Bytes>, Bytes>,
        opcode: WebSocketOpcode,
        payload: &[u8],
    ) -> Result<()> {
        let frame = WebSocketFrame::new(opcode, payload.to_vec(), true);
        let frame_bytes = frame.to_bytes();

        stream.send_data(Bytes::from(frame_bytes)).await
            .context("Failed to send WebSocket frame")?;

        debug!("📤 Sent WebSocket frame: {:?}, {} bytes", opcode, payload.len());
        Ok(())
    }

    /// 处理 WebSocket 消息
    async fn handle_websocket_messages(
        mut stream: RequestStream<BidiStream<Bytes>, Bytes>,
        conn_id: Uuid,
        connections: Arc<RwLock<HashMap<Uuid, H3WebSocketConnection>>>,
        broadcast_tx: broadcast::Sender<WebSocketMessage>,
    ) -> Result<()> {
        let mut buffer = Vec::new();

        info!("💬 Starting WebSocket message handling for {}", conn_id);

        loop {
            match stream.recv_data().await {
                Ok(Some(data)) => {
                    let data_bytes = data.chunk();
                    buffer.extend_from_slice(data_bytes);
                    debug!("📨 Received {} bytes from {}", data_bytes.len(), conn_id);
                    debug!("📨 Raw data: {:02x?}", &data_bytes[..std::cmp::min(data_bytes.len(), 50)]);
                    debug!("📨 Buffer now has {} bytes: {:02x?}", buffer.len(), &buffer[..std::cmp::min(buffer.len(), 50)]);
                    
                    // 尝试解析 WebSocket 帧
                    while let Some((frame, consumed)) = WebSocketFrame::parse(&buffer)? {
                        debug!("🔍 Parsed WebSocket frame: opcode={:?}, fin={}, mask={}, payload_len={}",
                               frame.opcode, frame.fin, frame.mask, frame.payload.len());
                        buffer.drain(..consumed);
                        
                        match frame.opcode {
                            WebSocketOpcode::Text => {
                                let text = String::from_utf8_lossy(&frame.payload);
                                info!("💬 Received text from {}: {}", conn_id, text);
                                
                                // 回显消息
                                Self::send_websocket_message(&mut stream, WebSocketOpcode::Text, &frame.payload).await?;

                                // 广播消息（这里简化实现，实际需要更复杂的广播机制）
                                let msg = WebSocketMessage {
                                    from: conn_id,
                                    opcode: WebSocketOpcode::Text,
                                    payload: frame.payload,
                                };
                                let _ = broadcast_tx.send(msg);
                            }
                            WebSocketOpcode::Binary => {
                                info!("📦 Received binary from {}: {} bytes", conn_id, frame.payload.len());

                                // 回显消息
                                Self::send_websocket_message(&mut stream, WebSocketOpcode::Binary, &frame.payload).await?;
                            }
                            WebSocketOpcode::Close => {
                                info!("👋 Received close from {}", conn_id);
                                Self::send_websocket_message(&mut stream, WebSocketOpcode::Close, &[]).await?;
                                break;
                            }
                            WebSocketOpcode::Ping => {
                                debug!("🏓 Received ping from {}", conn_id);
                                Self::send_websocket_message(&mut stream, WebSocketOpcode::Pong, &frame.payload).await?;
                            }
                            WebSocketOpcode::Pong => {
                                debug!("🏓 Received pong from {}", conn_id);
                            }
                            _ => {
                                warn!("❓ Unsupported WebSocket opcode: {:?}", frame.opcode);
                            }
                        }
                    }
                }
                Ok(None) => {
                    debug!("🔚 WebSocket stream ended for {}", conn_id);
                    break;
                }
                Err(e) => {
                    error!("❌ Error receiving WebSocket data from {}: {}", conn_id, e);
                    break;
                }
            }
        }

        // 清理连接
        connections.write().await.remove(&conn_id);
        info!("🧹 WebSocket connection {} closed and cleaned up", conn_id);

        Ok(())
    }
}
