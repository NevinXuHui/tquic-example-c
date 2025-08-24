use crate::client::ClientManager;
use crate::handler::MessageHandler;
use crate::message::{ClientId, MessageFrame};
use anyhow::{Context, Result};
use quinn::{Endpoint, ServerConfig};
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use tokio::time;
use tracing::{debug, error, info, warn};
use uuid::Uuid;

/// QUIC WebSocket 服务器
pub struct QuicWebSocketServer {
    endpoint: Endpoint,
    client_manager: Arc<ClientManager>,
    message_handler: Arc<MessageHandler>,
    server_name: String,
}

impl QuicWebSocketServer {
    /// 创建新的服务器实例
    pub fn new(
        endpoint: Endpoint,
        server_name: String,
        max_clients: usize,
    ) -> (Self, tokio::sync::broadcast::Receiver<MessageFrame>) {
        let (client_manager, broadcast_rx) = ClientManager::new(max_clients);
        let client_manager = Arc::new(client_manager);
        let message_handler = Arc::new(MessageHandler::new(
            client_manager.clone(),
            server_name.clone(),
        ));

        let server = Self {
            endpoint,
            client_manager,
            message_handler,
            server_name,
        };

        (server, broadcast_rx)
    }

    /// 启动服务器
    pub async fn run(&self) -> Result<()> {
        info!("Starting QUIC WebSocket server: {}", self.server_name);
        info!("Listening on: {}", self.endpoint.local_addr()?);

        // 启动清理任务
        self.start_cleanup_task().await;

        // 主循环：接受新连接
        while let Some(conn) = self.endpoint.accept().await {
            let client_manager = self.client_manager.clone();
            let message_handler = self.message_handler.clone();

            tokio::spawn(async move {
                if let Err(e) = Self::handle_connection(conn, client_manager, message_handler).await {
                    error!("Connection handling error: {}", e);
                }
            });
        }

        Ok(())
    }

    /// 处理单个客户端连接
    async fn handle_connection(
        connecting: quinn::Connecting,
        client_manager: Arc<ClientManager>,
        message_handler: Arc<MessageHandler>,
    ) -> Result<()> {
        let connection = connecting.await.context("Failed to establish connection")?;
        let remote_addr = connection.remote_address();
        let client_id = Uuid::new_v4();

        info!("New connection from {}, assigned ID: {}", remote_addr, client_id);

        // 添加客户端到管理器
        if !client_manager.add_client(client_id, connection.clone()).await? {
            warn!("Failed to add client {}: server full", client_id);
            connection.close(quinn::VarInt::from_u32(1008), b"Server full");
            return Ok(());
        }

        // 处理客户端流
        loop {
            tokio::select! {
                // 处理新的单向流（客户端发送的消息）
                stream = connection.accept_uni() => {
                    match stream {
                        Ok(recv_stream) => {
                            let client_manager = client_manager.clone();
                            let message_handler = message_handler.clone();
                            tokio::spawn(async move {
                                if let Err(e) = Self::handle_stream(
                                    client_id,
                                    recv_stream,
                                    client_manager,
                                    message_handler,
                                ).await {
                                    error!("Stream handling error for client {}: {}", client_id, e);
                                }
                            });
                        }
                        Err(quinn::ConnectionError::ApplicationClosed { .. }) => {
                            info!("Client {} closed connection", client_id);
                            break;
                        }
                        Err(e) => {
                            error!("Failed to accept stream from client {}: {}", client_id, e);
                            break;
                        }
                    }
                }
                
                // 检查连接是否仍然活跃
                _ = time::sleep(Duration::from_secs(1)) => {
                    if connection.close_reason().is_some() {
                        info!("Client {} connection closed", client_id);
                        break;
                    }
                }
            }
        }

        // 清理客户端
        client_manager.remove_client(&client_id).await;
        Ok(())
    }

    /// 处理单个流（消息）
    async fn handle_stream(
        client_id: ClientId,
        mut recv_stream: quinn::RecvStream,
        _client_manager: Arc<ClientManager>,
        message_handler: Arc<MessageHandler>,
    ) -> Result<()> {
        // 读取消息长度（4字节）
        let mut len_bytes = [0u8; 4];
        recv_stream.read_exact(&mut len_bytes).await?;
        let message_len = u32::from_be_bytes(len_bytes) as usize;

        // 防止过大的消息
        if message_len > 1024 * 1024 {
            // 1MB 限制
            warn!("Message too large from client {}: {} bytes", client_id, message_len);
            return Ok(());
        }

        // 读取消息数据
        let mut message_data = vec![0u8; message_len];
        recv_stream.read_exact(&mut message_data).await?;

        // 反序列化消息
        match MessageFrame::from_bytes(&message_data) {
            Ok(frame) => {
                debug!("Received message from {}: {}", client_id, frame.message_type);
                
                // 处理消息
                if let Err(e) = message_handler.handle_message(&client_id, frame).await {
                    error!("Failed to handle message from client {}: {}", client_id, e);
                }
            }
            Err(e) => {
                warn!("Failed to deserialize message from client {}: {}", client_id, e);
            }
        }

        Ok(())
    }

    /// 启动定期清理任务
    async fn start_cleanup_task(&self) {
        let client_manager = self.client_manager.clone();
        
        tokio::spawn(async move {
            let mut interval = time::interval(Duration::from_secs(30));
            
            loop {
                interval.tick().await;
                
                let cleaned = client_manager.cleanup_disconnected_clients().await;
                if cleaned > 0 {
                    info!("Cleaned up {} disconnected clients", cleaned);
                }
                
                let client_count = client_manager.client_count().await;
                debug!("Active clients: {}", client_count);
            }
        });
    }

    /// 获取服务器统计信息
    pub async fn get_stats(&self) -> ServerStats {
        ServerStats {
            active_clients: self.client_manager.client_count().await,
            server_name: self.server_name.clone(),
            local_addr: self.endpoint.local_addr().unwrap_or_else(|_| "0.0.0.0:0".parse().unwrap()),
        }
    }

    /// 优雅关闭服务器
    pub async fn shutdown(&self) -> Result<()> {
        info!("Shutting down QUIC WebSocket server");
        
        // 关闭端点，这会关闭所有连接
        self.endpoint.close(quinn::VarInt::from_u32(0), b"Server shutdown");
        
        // 等待所有连接关闭
        self.endpoint.wait_idle().await;
        
        info!("Server shutdown complete");
        Ok(())
    }
}

/// 服务器统计信息
#[derive(Debug, Clone)]
pub struct ServerStats {
    pub active_clients: usize,
    pub server_name: String,
    pub local_addr: SocketAddr,
}

/// 创建服务器配置
pub fn create_server_config(cert_path: &str, key_path: &str) -> Result<ServerConfig> {
    use rustls_pemfile::{certs, pkcs8_private_keys};
    use std::fs::File;
    use std::io::BufReader;

    // 读取证书
    let cert_file = File::open(cert_path).context("Failed to open certificate file")?;
    let mut cert_reader = BufReader::new(cert_file);
    let certs: Vec<rustls::Certificate> = certs(&mut cert_reader)?
        .into_iter()
        .map(rustls::Certificate)
        .collect();

    // 读取私钥
    let key_file = File::open(key_path).context("Failed to open private key file")?;
    let mut key_reader = BufReader::new(key_file);
    let mut keys = pkcs8_private_keys(&mut key_reader)?;

    if keys.is_empty() {
        anyhow::bail!("No private keys found in key file");
    }

    let key = rustls::PrivateKey(keys.remove(0));

    // 创建 TLS 配置
    let mut tls_config = rustls::ServerConfig::builder()
        .with_safe_defaults()
        .with_no_client_auth()
        .with_single_cert(certs, key)?;

    // 设置 ALPN 协议
    tls_config.alpn_protocols = vec![b"quic-websocket".to_vec()];

    // 创建 QUIC 服务器配置
    let mut server_config = ServerConfig::with_crypto(Arc::new(tls_config));
    
    // 配置传输参数
    let mut transport_config = quinn::TransportConfig::default();
    transport_config.max_concurrent_uni_streams(1000u32.into());
    transport_config.max_concurrent_bidi_streams(100u32.into());
    transport_config.max_idle_timeout(Some(Duration::from_secs(300).try_into()?));
    
    server_config.transport = Arc::new(transport_config);

    Ok(server_config)
}
