use crate::message::{ClientId, ClientInfo, MessageFrame};
use anyhow::Result;
use quinn::Connection;
use std::collections::HashMap;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::sync::{broadcast, RwLock};
use tracing::{debug, error, info, warn};

/// 客户端连接状态
#[derive(Debug, Clone)]
pub enum ClientState {
    /// 连接中，等待握手
    Connecting,
    /// 已连接并完成握手
    Connected,
    /// 连接断开
    Disconnected,
}

/// 客户端连接信息
#[derive(Debug, Clone)]
pub struct ClientConnection {
    pub id: ClientId,
    pub name: Option<String>,
    pub connection: Connection,
    pub state: ClientState,
    pub connected_at: u64,
    pub last_seen: u64,
    pub message_count: u64,
}

impl ClientConnection {
    pub fn new(id: ClientId, connection: Connection) -> Self {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();

        Self {
            id,
            name: None,
            connection,
            state: ClientState::Connecting,
            connected_at: now,
            last_seen: now,
            message_count: 0,
        }
    }

    /// 更新最后活跃时间
    pub fn update_last_seen(&mut self) {
        self.last_seen = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();
    }

    /// 增加消息计数
    pub fn increment_message_count(&mut self) {
        self.message_count += 1;
    }

    /// 发送消息到客户端
    pub async fn send_message(&self, frame: &MessageFrame) -> Result<()> {
        let data = frame.to_bytes()?;
        
        // 打开新的单向流发送消息
        let mut send_stream = self.connection.open_uni().await?;
        
        // 发送消息长度（4字节）+ 消息数据
        let len = data.len() as u32;
        send_stream.write_all(&len.to_be_bytes()).await?;
        send_stream.write_all(&data).await?;
        send_stream.finish().await?;

        debug!("Sent message to client {}: {}", self.id, frame.message_type);
        Ok(())
    }

    /// 获取客户端信息
    pub fn get_info(&self) -> ClientInfo {
        ClientInfo {
            id: self.id,
            name: self.name.clone(),
            connected_at: self.connected_at,
            last_seen: self.last_seen,
        }
    }
}

/// 客户端管理器
#[derive(Debug)]
pub struct ClientManager {
    clients: Arc<RwLock<HashMap<ClientId, ClientConnection>>>,
    broadcast_tx: broadcast::Sender<MessageFrame>,
    max_clients: usize,
}

impl ClientManager {
    pub fn new(max_clients: usize) -> (Self, broadcast::Receiver<MessageFrame>) {
        let (broadcast_tx, broadcast_rx) = broadcast::channel(1000);
        
        let manager = Self {
            clients: Arc::new(RwLock::new(HashMap::new())),
            broadcast_tx,
            max_clients,
        };

        (manager, broadcast_rx)
    }

    /// 添加新客户端
    pub async fn add_client(&self, id: ClientId, connection: Connection) -> Result<bool> {
        let mut clients = self.clients.write().await;
        
        if clients.len() >= self.max_clients {
            warn!("Maximum client limit reached: {}", self.max_clients);
            return Ok(false);
        }

        let client = ClientConnection::new(id, connection);
        clients.insert(id, client);
        
        info!("Client {} connected. Total clients: {}", id, clients.len());
        Ok(true)
    }

    /// 移除客户端
    pub async fn remove_client(&self, id: &ClientId) -> Option<ClientConnection> {
        let mut clients = self.clients.write().await;
        let client = clients.remove(id);
        
        if client.is_some() {
            info!("Client {} disconnected. Total clients: {}", id, clients.len());
        }
        
        client
    }

    /// 获取客户端
    pub async fn get_client(&self, id: &ClientId) -> Option<ClientConnection> {
        let clients = self.clients.read().await;
        clients.get(id).cloned()
    }

    /// 更新客户端状态
    pub async fn update_client_state(&self, id: &ClientId, state: ClientState) -> Result<()> {
        let mut clients = self.clients.write().await;
        if let Some(client) = clients.get_mut(id) {
            client.state = state;
            client.update_last_seen();
        }
        Ok(())
    }

    /// 设置客户端名称
    pub async fn set_client_name(&self, id: &ClientId, name: String) -> Result<()> {
        let mut clients = self.clients.write().await;
        if let Some(client) = clients.get_mut(id) {
            client.name = Some(name);
            client.update_last_seen();
        }
        Ok(())
    }

    /// 发送消息给特定客户端
    pub async fn send_to_client(&self, id: &ClientId, frame: &MessageFrame) -> Result<()> {
        let clients = self.clients.read().await;
        if let Some(client) = clients.get(id) {
            client.send_message(frame).await?;
        } else {
            warn!("Attempted to send message to non-existent client: {}", id);
        }
        Ok(())
    }

    /// 广播消息给所有客户端
    pub async fn broadcast_message(&self, frame: &MessageFrame) -> Result<usize> {
        let clients = self.clients.read().await;
        let mut sent_count = 0;

        for client in clients.values() {
            if matches!(client.state, ClientState::Connected) {
                if let Err(e) = client.send_message(frame).await {
                    error!("Failed to send broadcast message to client {}: {}", client.id, e);
                } else {
                    sent_count += 1;
                }
            }
        }

        // 也发送到广播通道
        if let Err(e) = self.broadcast_tx.send(frame.clone()) {
            debug!("No broadcast receivers: {}", e);
        }

        info!("Broadcast message sent to {} clients", sent_count);
        Ok(sent_count)
    }

    /// 获取所有客户端信息
    pub async fn get_all_clients(&self) -> Vec<ClientInfo> {
        let clients = self.clients.read().await;
        clients.values().map(|client| client.get_info()).collect()
    }

    /// 获取连接的客户端数量
    pub async fn client_count(&self) -> usize {
        let clients = self.clients.read().await;
        clients.len()
    }

    /// 清理断开的客户端
    pub async fn cleanup_disconnected_clients(&self) -> usize {
        let mut clients = self.clients.write().await;
        let mut to_remove = Vec::new();

        for (id, client) in clients.iter() {
            if client.connection.close_reason().is_some() {
                to_remove.push(*id);
            }
        }

        let removed_count = to_remove.len();
        for id in to_remove {
            clients.remove(&id);
            info!("Cleaned up disconnected client: {}", id);
        }

        removed_count
    }

    /// 获取广播发送器的克隆
    pub fn get_broadcast_sender(&self) -> broadcast::Sender<MessageFrame> {
        self.broadcast_tx.clone()
    }
}
