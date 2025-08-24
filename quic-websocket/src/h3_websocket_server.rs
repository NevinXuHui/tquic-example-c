use anyhow::{Context, Result};
use bytes::Bytes;
use h3::server::RequestStream;
use h3_quinn::BidiStream;
use quinn::{Endpoint, ServerConfig};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::sync::{broadcast, RwLock};
use tracing::{debug, error, info, warn};
use uuid::Uuid;

use crate::websocket::{WebSocketFrame, WebSocketOpcode, generate_websocket_accept};

/// HTTP/3 WebSocket 服务器
pub struct H3WebSocketServer {
    endpoint: Endpoint,
    connections: Arc<RwLock<HashMap<Uuid, WebSocketConnection>>>,
    broadcast_tx: broadcast::Sender<WebSocketMessage>,
    server_name: String,
}

/// WebSocket 连接状态
#[derive(Debug, Clone)]
pub struct WebSocketConnection {
    pub id: Uuid,
    pub remote_addr: SocketAddr,
    pub state: WebSocketState,
    pub request_stream: Option<RequestStream<BidiStream<Bytes>, Bytes>>,
}

/// WebSocket 连接状态
#[derive(Debug, Clone, PartialEq)]
pub enum WebSocketState {
    Connecting,
    Open,
    Closing,
    Closed,
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
        info!("Starting HTTP/3 WebSocket server: {}", self.server_name);
        info!("Listening on: {}", self.endpoint.local_addr()?);

        // 启动广播任务
        self.start_broadcast_task().await;

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
        connections: Arc<RwLock<HashMap<Uuid, WebSocketConnection>>>,
        broadcast_tx: broadcast::Sender<WebSocketMessage>,
        server_name: String,
    ) -> Result<()> {
        let connection = connecting.await.context("Failed to establish connection")?;
        let remote_addr = connection.remote_address();
        let conn_id = Uuid::new_v4();

        info!("New HTTP/3 connection from {}, assigned ID: {}", remote_addr, conn_id);

        // 创建 HTTP/3 连接
        let mut h3_conn = h3::server::Connection::new(h3_quinn::Connection::new(connection))
            .await
            .context("Failed to create HTTP/3 connection")?;

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
                    debug!("Connection {} closed", conn_id);
                    break;
                }
                Err(e) => {
                    error!("Error accepting request: {}", e);
                    break;
                }
            }
        }

        // 清理连接
        connections.write().await.remove(&conn_id);
        info!("Connection {} cleaned up", conn_id);

        Ok(())
    }

    /// 处理 HTTP/3 请求
    async fn handle_request(
        req: http::Request<()>,
        mut stream: RequestStream<BidiStream<Bytes>, Bytes>,
        conn_id: Uuid,
        remote_addr: SocketAddr,
        connections: Arc<RwLock<HashMap<Uuid, WebSocketConnection>>>,
        broadcast_tx: broadcast::Sender<WebSocketMessage>,
        server_name: String,
    ) -> Result<()> {
        debug!("Received request: {:?}", req);

        // 检查是否为 WebSocket 升级请求
        if Self::is_websocket_upgrade(&req) {
            info!("WebSocket upgrade request from {}", remote_addr);
            
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

    /// 检查是否为 WebSocket 升级请求
    fn is_websocket_upgrade(req: &http::Request<()>) -> bool {
        let headers = req.headers();
        
        // 检查必要的 WebSocket 头部
        headers.get("upgrade").map_or(false, |v| v == "websocket") &&
        headers.get("connection").map_or(false, |v| {
            v.to_str().unwrap_or("").to_lowercase().contains("upgrade")
        }) &&
        headers.get("sec-websocket-version").map_or(false, |v| v == "13") &&
        headers.get("sec-websocket-key").is_some()
    }

    /// 处理 WebSocket 升级
    async fn handle_websocket_upgrade(
        req: http::Request<()>,
        mut stream: RequestStream<BidiStream<Bytes>, Bytes>,
        conn_id: Uuid,
        remote_addr: SocketAddr,
        connections: Arc<RwLock<HashMap<Uuid, WebSocketConnection>>>,
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

        info!("WebSocket upgrade successful for connection {}", conn_id);

        // 创建 WebSocket 连接记录
        let ws_conn = WebSocketConnection {
            id: conn_id,
            remote_addr,
            state: WebSocketState::Open,
            request_stream: Some(stream.clone()),
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
        _req: http::Request<()>,
        mut stream: RequestStream<BidiStream<Bytes>, Bytes>,
    ) -> Result<()> {
        let html_response = r#"
<!DOCTYPE html>
<html>
<head>
    <title>QUIC WebSocket Server (HTTP/3)</title>
</head>
<body>
    <h1>QUIC WebSocket Server (HTTP/3)</h1>
    <p>This server supports WebSocket over HTTP/3.</p>
    <p>Use a compatible WebSocket client to connect.</p>
</body>
</html>
        "#;

        let response = http::Response::builder()
            .status(200)
            .header("content-type", "text/html")
            .body(())
            .context("Failed to build HTTP response")?;

        stream.send_response(response).await
            .context("Failed to send HTTP response")?;

        stream.send_data(Bytes::from(html_response)).await
            .context("Failed to send HTTP response body")?;

        stream.finish().await
            .context("Failed to finish HTTP response")?;

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

        Ok(())
    }

    /// 处理 WebSocket 消息
    async fn handle_websocket_messages(
        mut stream: RequestStream<BidiStream<Bytes>, Bytes>,
        conn_id: Uuid,
        connections: Arc<RwLock<HashMap<Uuid, WebSocketConnection>>>,
        broadcast_tx: broadcast::Sender<WebSocketMessage>,
    ) -> Result<()> {
        let mut buffer = Vec::new();

        loop {
            match stream.recv_data().await {
                Ok(Some(data)) => {
                    buffer.extend_from_slice(&data);
                    
                    // 尝试解析 WebSocket 帧
                    while let Some((frame, consumed)) = WebSocketFrame::parse(&buffer)? {
                        buffer.drain(..consumed);
                        
                        match frame.opcode {
                            WebSocketOpcode::Text => {
                                let text = String::from_utf8_lossy(&frame.payload);
                                info!("Received text from {}: {}", conn_id, text);
                                
                                // 回显消息
                                Self::send_websocket_message(&mut stream, WebSocketOpcode::Text, &frame.payload).await?;
                                
                                // 广播消息
                                let msg = WebSocketMessage {
                                    from: conn_id,
                                    opcode: WebSocketOpcode::Text,
                                    payload: frame.payload,
                                };
                                let _ = broadcast_tx.send(msg);
                            }
                            WebSocketOpcode::Binary => {
                                info!("Received binary from {}: {} bytes", conn_id, frame.payload.len());
                                
                                // 回显消息
                                Self::send_websocket_message(&mut stream, WebSocketOpcode::Binary, &frame.payload).await?;
                            }
                            WebSocketOpcode::Close => {
                                info!("Received close from {}", conn_id);
                                Self::send_websocket_message(&mut stream, WebSocketOpcode::Close, &[]).await?;
                                break;
                            }
                            WebSocketOpcode::Ping => {
                                debug!("Received ping from {}", conn_id);
                                Self::send_websocket_message(&mut stream, WebSocketOpcode::Pong, &frame.payload).await?;
                            }
                            WebSocketOpcode::Pong => {
                                debug!("Received pong from {}", conn_id);
                            }
                            _ => {
                                warn!("Unsupported WebSocket opcode: {:?}", frame.opcode);
                            }
                        }
                    }
                }
                Ok(None) => {
                    debug!("WebSocket stream ended for {}", conn_id);
                    break;
                }
                Err(e) => {
                    error!("Error receiving WebSocket data from {}: {}", conn_id, e);
                    break;
                }
            }
        }

        // 清理连接
        connections.write().await.remove(&conn_id);
        info!("WebSocket connection {} closed", conn_id);

        Ok(())
    }

    /// 启动广播任务
    async fn start_broadcast_task(&self) {
        let connections = self.connections.clone();
        let mut broadcast_rx = self.broadcast_tx.subscribe();

        tokio::spawn(async move {
            while let Ok(msg) = broadcast_rx.recv().await {
                let connections_read = connections.read().await;
                
                for (conn_id, conn) in connections_read.iter() {
                    if *conn_id != msg.from && conn.state == WebSocketState::Open {
                        // 这里需要实现向其他连接广播消息的逻辑
                        // 由于 RequestStream 的限制，这部分需要重新设计
                        debug!("Would broadcast message from {} to {}", msg.from, conn_id);
                    }
                }
            }
        });
    }
}
