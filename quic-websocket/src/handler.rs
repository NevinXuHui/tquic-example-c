use crate::client::{ClientManager, ClientState};
use crate::message::{error_codes, ClientId, MessageFrame, MessageType};
use anyhow::Result;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use tracing::{debug, info, warn};

/// 消息处理器
pub struct MessageHandler {
    client_manager: Arc<ClientManager>,
    server_name: String,
    protocol_version: String,
}

impl MessageHandler {
    pub fn new(client_manager: Arc<ClientManager>, server_name: String) -> Self {
        Self {
            client_manager,
            server_name,
            protocol_version: "1.0".to_string(),
        }
    }

    /// 处理来自客户端的消息
    pub async fn handle_message(&self, client_id: &ClientId, frame: MessageFrame) -> Result<()> {
        debug!("Handling message from {}: {}", client_id, frame.message_type);

        // 更新客户端最后活跃时间
        if let Some(mut client) = self.client_manager.get_client(client_id).await {
            client.update_last_seen();
            client.increment_message_count();
        }

        match frame.message_type {
            MessageType::Handshake { client_name, protocol_version } => {
                self.handle_handshake(client_id, client_name, protocol_version).await
            }
            MessageType::Text { content, .. } => {
                self.handle_text_message(client_id, content).await
            }
            MessageType::Binary { data, .. } => {
                self.handle_binary_message(client_id, data).await
            }
            MessageType::Broadcast { content, .. } => {
                self.handle_broadcast_message(client_id, content).await
            }
            MessageType::DirectMessage { to, content, .. } => {
                self.handle_direct_message(client_id, &to, content).await
            }
            MessageType::Ping { timestamp } => {
                self.handle_ping(client_id, timestamp).await
            }
            MessageType::ListClients => {
                self.handle_list_clients(client_id).await
            }
            MessageType::Close { code, reason } => {
                self.handle_close(client_id, code, reason).await
            }
            MessageType::Subscribe { topics } => {
                self.handle_subscribe(client_id, topics).await
            }
            MessageType::Unsubscribe { topics } => {
                self.handle_unsubscribe(client_id, topics).await
            }
            _ => {
                warn!("Unhandled message type from client {}: {:?}", client_id, frame.message_type);
                self.send_error(client_id, error_codes::INVALID_MESSAGE, "Unsupported message type").await
            }
        }
    }

    /// 处理握手消息
    async fn handle_handshake(&self, client_id: &ClientId, client_name: Option<String>, protocol_version: String) -> Result<()> {
        info!("Handshake from client {}: name={:?}, version={}", client_id, client_name, protocol_version);

        // 检查协议版本
        let accepted = protocol_version == self.protocol_version;
        let reason = if !accepted {
            Some(format!("Unsupported protocol version: {}. Expected: {}", protocol_version, self.protocol_version))
        } else {
            None
        };

        // 发送握手响应
        let response = MessageFrame::new(MessageType::HandshakeResponse {
            client_id: *client_id,
            server_name: self.server_name.clone(),
            accepted,
            reason: reason.clone(),
        });

        self.client_manager.send_to_client(client_id, &response).await?;

        if accepted {
            // 设置客户端名称和状态
            if let Some(name) = client_name {
                self.client_manager.set_client_name(client_id, name).await?;
            }
            self.client_manager.update_client_state(client_id, ClientState::Connected).await?;
            
            info!("Client {} handshake completed successfully", client_id);
        } else {
            warn!("Client {} handshake failed: {:?}", client_id, reason);
        }

        Ok(())
    }

    /// 处理文本消息
    async fn handle_text_message(&self, client_id: &ClientId, content: String) -> Result<()> {
        info!("Text message from {}: {}", client_id, content);

        // 简单的回显处理
        let response = MessageFrame::new(MessageType::Text {
            content: format!("Echo: {}", content),
            timestamp: current_timestamp(),
        });

        self.client_manager.send_to_client(client_id, &response).await?;
        Ok(())
    }

    /// 处理二进制消息
    async fn handle_binary_message(&self, client_id: &ClientId, data: Vec<u8>) -> Result<()> {
        info!("Binary message from {}: {} bytes", client_id, data.len());

        // 简单的回显处理
        let response = MessageFrame::new(MessageType::Binary {
            data: data.clone(),
            timestamp: current_timestamp(),
        });

        self.client_manager.send_to_client(client_id, &response).await?;
        Ok(())
    }

    /// 处理广播消息
    async fn handle_broadcast_message(&self, client_id: &ClientId, content: String) -> Result<()> {
        info!("Broadcast message from {}: {}", client_id, content);

        let broadcast_frame = MessageFrame::new(MessageType::Broadcast {
            from: *client_id,
            content,
            timestamp: current_timestamp(),
        });

        let sent_count = self.client_manager.broadcast_message(&broadcast_frame).await?;
        
        // 发送确认给发送者
        let ack_frame = MessageFrame::new(MessageType::Text {
            content: format!("Broadcast sent to {} clients", sent_count),
            timestamp: current_timestamp(),
        });
        
        self.client_manager.send_to_client(client_id, &ack_frame).await?;
        Ok(())
    }

    /// 处理私聊消息
    async fn handle_direct_message(&self, from: &ClientId, to: &ClientId, content: String) -> Result<()> {
        info!("Direct message from {} to {}: {}", from, to, content);

        let message_frame = MessageFrame::new(MessageType::DirectMessage {
            from: *from,
            to: *to,
            content: content.clone(),
            timestamp: current_timestamp(),
        });

        // 发送给目标客户端
        if self.client_manager.get_client(to).await.is_some() {
            self.client_manager.send_to_client(to, &message_frame).await?;
            
            // 发送确认给发送者
            let ack_frame = MessageFrame::new(MessageType::Text {
                content: "Direct message sent".to_string(),
                timestamp: current_timestamp(),
            });
            self.client_manager.send_to_client(from, &ack_frame).await?;
        } else {
            // 目标客户端不存在
            self.send_error(from, error_codes::CLIENT_NOT_FOUND, "Target client not found").await?;
        }

        Ok(())
    }

    /// 处理心跳消息
    async fn handle_ping(&self, client_id: &ClientId, _timestamp: u64) -> Result<()> {
        debug!("Ping from client {}", client_id);

        let pong_frame = MessageFrame::new(MessageType::Pong {
            timestamp: current_timestamp(),
        });

        self.client_manager.send_to_client(client_id, &pong_frame).await?;
        Ok(())
    }

    /// 处理客户端列表请求
    async fn handle_list_clients(&self, client_id: &ClientId) -> Result<()> {
        debug!("Client list request from {}", client_id);

        let clients = self.client_manager.get_all_clients().await;
        let response = MessageFrame::new(MessageType::ClientList { clients });

        self.client_manager.send_to_client(client_id, &response).await?;
        Ok(())
    }

    /// 处理连接关闭
    async fn handle_close(&self, client_id: &ClientId, code: u16, reason: String) -> Result<()> {
        info!("Client {} closing connection: {} - {}", client_id, code, reason);

        // 移除客户端
        self.client_manager.remove_client(client_id).await;
        Ok(())
    }

    /// 处理订阅请求
    async fn handle_subscribe(&self, client_id: &ClientId, topics: Vec<String>) -> Result<()> {
        info!("Client {} subscribing to topics: {:?}", client_id, topics);

        self.client_manager.subscribe_topics(client_id, topics.clone()).await?;

        // 发送订阅确认
        let confirmation = MessageFrame::new(MessageType::Text {
            content: format!("✅ Subscribed to topics: {}", topics.join(", ")),
            timestamp: current_timestamp(),
        });

        self.client_manager.send_to_client(client_id, &confirmation).await?;

        // 发送欢迎消息到新订阅者
        for topic in &topics {
            let welcome_frame = MessageFrame::new(MessageType::ServerPush {
                topic: topic.clone(),
                content: format!("Welcome to topic '{}'! You will receive real-time updates.", topic),
                timestamp: current_timestamp(),
            });

            self.client_manager.send_to_client(client_id, &welcome_frame).await?;
        }

        Ok(())
    }

    /// 处理取消订阅请求
    async fn handle_unsubscribe(&self, client_id: &ClientId, topics: Vec<String>) -> Result<()> {
        info!("Client {} unsubscribing from topics: {:?}", client_id, topics);

        self.client_manager.unsubscribe_topics(client_id, topics.clone()).await?;

        // 发送取消订阅确认
        let confirmation = MessageFrame::new(MessageType::Text {
            content: format!("✅ Unsubscribed from topics: {}", topics.join(", ")),
            timestamp: current_timestamp(),
        });

        self.client_manager.send_to_client(client_id, &confirmation).await?;
        Ok(())
    }

    /// 发送错误消息
    async fn send_error(&self, client_id: &ClientId, code: u16, message: &str) -> Result<()> {
        let error_frame = MessageFrame::new(MessageType::Error {
            code,
            message: message.to_string(),
        });

        self.client_manager.send_to_client(client_id, &error_frame).await?;
        Ok(())
    }
}

/// 获取当前时间戳
fn current_timestamp() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs()
}
