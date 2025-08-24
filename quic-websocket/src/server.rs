use crate::client::ClientManager;
use crate::handler::MessageHandler;
use crate::message::{ClientId, MessageFrame, MessageType};
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

        // 启动服务器主动推送任务 - WebSocket 核心特色
        self.start_push_tasks().await;

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

    /// 启动服务器主动推送任务 - WebSocket 特色功能
    async fn start_push_tasks(&self) {
        // 1. 定期心跳推送
        self.start_heartbeat_task().await;

        // 2. 服务器状态推送
        self.start_status_push_task().await;

        // 3. 系统通知推送
        self.start_notification_task().await;

        // 4. 实时数据推送（模拟）
        self.start_realtime_data_task().await;
    }

    /// 定期心跳推送任务
    async fn start_heartbeat_task(&self) {
        let client_manager = self.client_manager.clone();

        tokio::spawn(async move {
            let mut interval = time::interval(Duration::from_secs(30));

            loop {
                interval.tick().await;

                let heartbeat_frame = MessageFrame::new(MessageType::Ping {
                    timestamp: std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_secs(),
                });

                let sent_count = client_manager.broadcast_message(&heartbeat_frame).await.unwrap_or(0);
                if sent_count > 0 {
                    info!("Sent heartbeat to {} clients", sent_count);
                }
            }
        });
    }

    /// 服务器状态推送任务
    async fn start_status_push_task(&self) {
        let client_manager = self.client_manager.clone();
        let server_name = self.server_name.clone();

        tokio::spawn(async move {
            let mut interval = time::interval(Duration::from_secs(60));

            loop {
                interval.tick().await;

                let client_count = client_manager.client_count().await;
                let status_message = format!(
                    "🔔 Server Status: {} - {} active connections",
                    server_name, client_count
                );

                let status_frame = MessageFrame::new(MessageType::Text {
                    content: status_message,
                    timestamp: std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_secs(),
                });

                let sent_count = client_manager.broadcast_message(&status_frame).await.unwrap_or(0);
                if sent_count > 0 {
                    info!("Pushed server status to {} clients", sent_count);
                }
            }
        });
    }

    /// 系统通知推送任务 - 使用主题订阅
    async fn start_notification_task(&self) {
        let client_manager = self.client_manager.clone();

        tokio::spawn(async move {
            let mut interval = time::interval(Duration::from_secs(90)); // 1.5分钟
            let notifications = vec![
                ("system", "📢 Welcome to QUIC WebSocket Server!"),
                ("tips", "⚡ Enjoying the low-latency experience?"),
                ("tech", "🚀 QUIC protocol provides 0-RTT connection establishment"),
                ("security", "🔒 All connections are secured with TLS 1.3"),
                ("performance", "🌐 Multiplexed streams for better performance"),
                ("news", "📰 Server is running smoothly with active connections"),
            ];
            let mut index = 0;

            loop {
                interval.tick().await;

                let (topic, notification) = notifications[index % notifications.len()];
                let notification_frame = MessageFrame::new(MessageType::ServerPush {
                    topic: topic.to_string(),
                    content: notification.to_string(),
                    timestamp: std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_secs(),
                });

                let sent_count = client_manager.push_to_subscribers(topic, &notification_frame).await.unwrap_or(0);
                if sent_count > 0 {
                    info!("Pushed '{}' notification to {} subscribers of topic '{}'", notification, sent_count, topic);
                }

                index += 1;
            }
        });
    }

    /// 实时数据推送任务 - 多主题数据推送
    async fn start_realtime_data_task(&self) {
        let client_manager = self.client_manager.clone();

        // 传感器数据推送
        let sensor_manager = client_manager.clone();
        tokio::spawn(async move {
            let mut interval = time::interval(Duration::from_secs(15));
            let mut counter = 0;

            loop {
                interval.tick().await;

                let sensor_data = serde_json::json!({
                    "temperature": 20.0 + (counter as f64 * 0.1) % 10.0,
                    "humidity": 45.0 + (counter as f64 * 0.2) % 20.0,
                    "pressure": 1013.25 + (counter as f64 * 0.05) % 5.0,
                    "timestamp": std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_secs(),
                });

                let sensor_frame = MessageFrame::new(MessageType::ServerPush {
                    topic: "sensors".to_string(),
                    content: format!("🌡️ Sensor Data: {}", sensor_data),
                    timestamp: std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_secs(),
                });

                let sent_count = sensor_manager.push_to_subscribers("sensors", &sensor_frame).await.unwrap_or(0);
                if sent_count > 0 {
                    debug!("Pushed sensor data to {} subscribers", sent_count);
                }

                counter += 1;
            }
        });

        // 系统监控数据推送
        let monitor_manager = client_manager.clone();
        tokio::spawn(async move {
            let mut interval = time::interval(Duration::from_secs(20));
            let mut counter = 0;

            loop {
                interval.tick().await;

                let monitor_data = serde_json::json!({
                    "cpu_usage": (counter % 100) as f64,
                    "memory_usage": 30.0 + (counter as f64 * 0.3) % 40.0,
                    "disk_usage": 25.0 + (counter as f64 * 0.1) % 15.0,
                    "network_io": counter % 1000,
                    "active_connections": monitor_manager.client_count().await,
                    "timestamp": std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_secs(),
                });

                let monitor_frame = MessageFrame::new(MessageType::ServerPush {
                    topic: "monitoring".to_string(),
                    content: format!("📊 System Monitor: {}", monitor_data),
                    timestamp: std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_secs(),
                });

                let sent_count = monitor_manager.push_to_subscribers("monitoring", &monitor_frame).await.unwrap_or(0);
                if sent_count > 0 {
                    debug!("Pushed monitoring data to {} subscribers", sent_count);
                }

                counter += 1;
            }
        });

        // 股票价格模拟推送
        let stock_manager = client_manager.clone();
        tokio::spawn(async move {
            let mut interval = time::interval(Duration::from_secs(5));
            let stocks = vec!["AAPL", "GOOGL", "MSFT", "TSLA", "AMZN"];
            let mut prices = vec![150.0, 2800.0, 300.0, 800.0, 3200.0];

            loop {
                interval.tick().await;

                // 模拟价格波动
                for (i, price) in prices.iter_mut().enumerate() {
                    let change = (rand::random::<f64>() - 0.5) * 10.0; // ±5 的随机变化
                    *price = (*price + change).max(1.0); // 确保价格不为负
                }

                let stock_data = serde_json::json!({
                    "stocks": stocks.iter().zip(prices.iter()).map(|(symbol, price)| {
                        serde_json::json!({
                            "symbol": symbol,
                            "price": format!("{:.2}", price),
                            "change": format!("{:+.2}", (rand::random::<f64>() - 0.5) * 5.0)
                        })
                    }).collect::<Vec<_>>(),
                    "timestamp": std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_secs(),
                });

                let stock_frame = MessageFrame::new(MessageType::ServerPush {
                    topic: "stocks".to_string(),
                    content: format!("📈 Stock Prices: {}", stock_data),
                    timestamp: std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_secs(),
                });

                let sent_count = stock_manager.push_to_subscribers("stocks", &stock_frame).await.unwrap_or(0);
                if sent_count > 0 {
                    debug!("Pushed stock data to {} subscribers", sent_count);
                }
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

    // 设置 ALPN 协议 (与 TQUIC 兼容)
    tls_config.alpn_protocols = vec![b"h3".to_vec()];

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
