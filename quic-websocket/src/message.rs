use serde::{Deserialize, Serialize};
use std::fmt;
use uuid::Uuid;

/// 客户端唯一标识符
pub type ClientId = Uuid;

/// WebSocket-like 消息类型
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum MessageType {
    /// 连接握手消息
    Handshake {
        client_name: Option<String>,
        protocol_version: String,
    },
    /// 握手响应
    HandshakeResponse {
        client_id: ClientId,
        server_name: String,
        accepted: bool,
        reason: Option<String>,
    },
    /// 文本消息
    Text {
        content: String,
        timestamp: u64,
    },
    /// 二进制消息
    Binary {
        data: Vec<u8>,
        timestamp: u64,
    },
    /// 广播消息
    Broadcast {
        from: ClientId,
        content: String,
        timestamp: u64,
    },
    /// 私聊消息
    DirectMessage {
        from: ClientId,
        to: ClientId,
        content: String,
        timestamp: u64,
    },
    /// 心跳消息
    Ping {
        timestamp: u64,
    },
    /// 心跳响应
    Pong {
        timestamp: u64,
    },
    /// 客户端列表请求
    ListClients,
    /// 客户端列表响应
    ClientList {
        clients: Vec<ClientInfo>,
    },
    /// 连接关闭
    Close {
        code: u16,
        reason: String,
    },
    /// 错误消息
    Error {
        code: u16,
        message: String,
    },
}

/// 客户端信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClientInfo {
    pub id: ClientId,
    pub name: Option<String>,
    pub connected_at: u64,
    pub last_seen: u64,
}

/// 消息帧结构
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MessageFrame {
    /// 消息ID，用于确认和去重
    pub id: Uuid,
    /// 消息类型
    pub message_type: MessageType,
    /// 消息优先级 (0-255, 255最高)
    pub priority: u8,
    /// 是否需要确认
    pub require_ack: bool,
}

impl MessageFrame {
    /// 创建新的消息帧
    pub fn new(message_type: MessageType) -> Self {
        Self {
            id: Uuid::new_v4(),
            message_type,
            priority: 128, // 默认中等优先级
            require_ack: false,
        }
    }

    /// 创建需要确认的消息帧
    pub fn new_with_ack(message_type: MessageType) -> Self {
        Self {
            id: Uuid::new_v4(),
            message_type,
            priority: 128,
            require_ack: true,
        }
    }

    /// 设置优先级
    pub fn with_priority(mut self, priority: u8) -> Self {
        self.priority = priority;
        self
    }

    /// 序列化为字节
    pub fn to_bytes(&self) -> anyhow::Result<Vec<u8>> {
        bincode::serialize(self).map_err(Into::into)
    }

    /// 从字节反序列化
    pub fn from_bytes(data: &[u8]) -> anyhow::Result<Self> {
        bincode::deserialize(data).map_err(Into::into)
    }
}

impl fmt::Display for MessageType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MessageType::Handshake { client_name, .. } => {
                write!(f, "Handshake({})", client_name.as_deref().unwrap_or("Anonymous"))
            }
            MessageType::HandshakeResponse { accepted, .. } => {
                write!(f, "HandshakeResponse({})", if *accepted { "OK" } else { "Failed" })
            }
            MessageType::Text { content, .. } => {
                write!(f, "Text({})", if content.len() > 50 { 
                    format!("{}...", &content[..50]) 
                } else { 
                    content.clone() 
                })
            }
            MessageType::Binary { data, .. } => write!(f, "Binary({} bytes)", data.len()),
            MessageType::Broadcast { content, .. } => write!(f, "Broadcast({})", content),
            MessageType::DirectMessage { content, .. } => write!(f, "DirectMessage({})", content),
            MessageType::Ping { .. } => write!(f, "Ping"),
            MessageType::Pong { .. } => write!(f, "Pong"),
            MessageType::ListClients => write!(f, "ListClients"),
            MessageType::ClientList { clients } => write!(f, "ClientList({} clients)", clients.len()),
            MessageType::Close { code, reason } => write!(f, "Close({}: {})", code, reason),
            MessageType::Error { code, message } => write!(f, "Error({}: {})", code, message),
        }
    }
}

/// 错误代码常量
pub mod error_codes {
    pub const PROTOCOL_ERROR: u16 = 1000;
    pub const INVALID_MESSAGE: u16 = 1001;
    pub const CLIENT_NOT_FOUND: u16 = 1002;
    pub const PERMISSION_DENIED: u16 = 1003;
    pub const SERVER_ERROR: u16 = 1004;
    pub const RATE_LIMITED: u16 = 1005;
}

/// 关闭代码常量
pub mod close_codes {
    pub const NORMAL_CLOSURE: u16 = 1000;
    pub const GOING_AWAY: u16 = 1001;
    pub const PROTOCOL_ERROR: u16 = 1002;
    pub const UNSUPPORTED_DATA: u16 = 1003;
    pub const INVALID_FRAME_PAYLOAD_DATA: u16 = 1007;
    pub const POLICY_VIOLATION: u16 = 1008;
    pub const MESSAGE_TOO_BIG: u16 = 1009;
    pub const INTERNAL_ERROR: u16 = 1011;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_message_serialization() {
        let message = MessageFrame::new(MessageType::Text {
            content: "Hello, World!".to_string(),
            timestamp: 1234567890,
        });

        let bytes = message.to_bytes().unwrap();
        let deserialized = MessageFrame::from_bytes(&bytes).unwrap();

        assert_eq!(message.id, deserialized.id);
        match (&message.message_type, &deserialized.message_type) {
            (MessageType::Text { content: c1, .. }, MessageType::Text { content: c2, .. }) => {
                assert_eq!(c1, c2);
            }
            _ => panic!("Message type mismatch"),
        }
    }
}
