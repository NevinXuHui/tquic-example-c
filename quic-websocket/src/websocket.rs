use anyhow::Result;
use sha1::{Digest, Sha1};
use base64::{Engine as _, engine::general_purpose};

/// WebSocket 魔术字符串 (RFC 6455)
const WEBSOCKET_MAGIC_STRING: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// WebSocket 操作码
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum WebSocketOpcode {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
}

impl From<u8> for WebSocketOpcode {
    fn from(value: u8) -> Self {
        match value & 0x0F {
            0x0 => WebSocketOpcode::Continuation,
            0x1 => WebSocketOpcode::Text,
            0x2 => WebSocketOpcode::Binary,
            0x8 => WebSocketOpcode::Close,
            0x9 => WebSocketOpcode::Ping,
            0xA => WebSocketOpcode::Pong,
            _ => WebSocketOpcode::Close, // 默认为关闭
        }
    }
}

/// WebSocket 帧
#[derive(Debug, Clone)]
pub struct WebSocketFrame {
    pub fin: bool,
    pub opcode: WebSocketOpcode,
    pub mask: bool,
    pub payload: Vec<u8>,
}

impl WebSocketFrame {
    /// 创建新的 WebSocket 帧
    pub fn new(opcode: WebSocketOpcode, payload: Vec<u8>, fin: bool) -> Self {
        Self {
            fin,
            opcode,
            mask: false, // 服务器发送的帧不需要掩码
            payload,
        }
    }

    /// 将帧转换为字节
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut bytes = Vec::new();
        
        // 第一个字节：FIN + RSV + Opcode
        let first_byte = if self.fin { 0x80 } else { 0x00 } | (self.opcode as u8);
        bytes.push(first_byte);
        
        // 第二个字节及后续：MASK + Payload length
        let payload_len = self.payload.len();
        if payload_len < 126 {
            bytes.push(payload_len as u8);
        } else if payload_len < 65536 {
            bytes.push(126);
            bytes.extend_from_slice(&(payload_len as u16).to_be_bytes());
        } else {
            bytes.push(127);
            bytes.extend_from_slice(&(payload_len as u64).to_be_bytes());
        }
        
        // 载荷数据
        bytes.extend_from_slice(&self.payload);
        
        bytes
    }

    /// 从字节解析帧
    pub fn parse(data: &[u8]) -> Result<Option<(WebSocketFrame, usize)>> {
        if data.len() < 2 {
            return Ok(None); // 需要更多数据
        }

        let mut cursor = 0;
        
        // 解析第一个字节
        let first_byte = data[cursor];
        cursor += 1;
        
        let fin = (first_byte & 0x80) != 0;
        let opcode = WebSocketOpcode::from(first_byte);
        
        // 解析第二个字节
        let second_byte = data[cursor];
        cursor += 1;
        
        let mask = (second_byte & 0x80) != 0;
        let mut payload_len = (second_byte & 0x7F) as u64;
        
        // 解析扩展载荷长度
        if payload_len == 126 {
            if data.len() < cursor + 2 {
                return Ok(None); // 需要更多数据
            }
            payload_len = u16::from_be_bytes([data[cursor], data[cursor + 1]]) as u64;
            cursor += 2;
        } else if payload_len == 127 {
            if data.len() < cursor + 8 {
                return Ok(None); // 需要更多数据
            }
            let mut len_bytes = [0u8; 8];
            len_bytes.copy_from_slice(&data[cursor..cursor + 8]);
            payload_len = u64::from_be_bytes(len_bytes);
            cursor += 8;
        }
        
        // 解析掩码密钥
        let mask_key = if mask {
            if data.len() < cursor + 4 {
                return Ok(None); // 需要更多数据
            }
            let key = [data[cursor], data[cursor + 1], data[cursor + 2], data[cursor + 3]];
            cursor += 4;
            Some(key)
        } else {
            None
        };
        
        // 检查是否有足够的载荷数据
        if data.len() < cursor + payload_len as usize {
            return Ok(None); // 需要更多数据
        }
        
        // 解析载荷数据
        let mut payload = data[cursor..cursor + payload_len as usize].to_vec();
        cursor += payload_len as usize;
        
        // 如果有掩码，进行解掩码
        if let Some(key) = mask_key {
            for (i, byte) in payload.iter_mut().enumerate() {
                *byte ^= key[i % 4];
            }
        }
        
        let frame = WebSocketFrame {
            fin,
            opcode,
            mask,
            payload,
        };
        
        Ok(Some((frame, cursor)))
    }
}

/// 生成 WebSocket Accept 密钥
pub fn generate_websocket_accept(key: &str) -> String {
    let mut hasher = Sha1::new();
    hasher.update(key.as_bytes());
    hasher.update(WEBSOCKET_MAGIC_STRING.as_bytes());
    let hash = hasher.finalize();
    general_purpose::STANDARD.encode(&hash)
}

/// 检查是否为有效的 WebSocket 升级请求
pub fn is_websocket_upgrade_request(headers: &http::HeaderMap) -> bool {
    // 检查必要的头部
    let upgrade = headers.get("upgrade")
        .and_then(|v| v.to_str().ok())
        .map(|s| s.to_lowercase());
    
    let connection = headers.get("connection")
        .and_then(|v| v.to_str().ok())
        .map(|s| s.to_lowercase());
    
    let version = headers.get("sec-websocket-version")
        .and_then(|v| v.to_str().ok());
    
    let key = headers.get("sec-websocket-key")
        .and_then(|v| v.to_str().ok());
    
    upgrade == Some("websocket".to_string()) &&
    connection.as_ref().map_or(false, |c| c.contains("upgrade")) &&
    version == Some("13") &&
    key.is_some()
}

/// WebSocket 连接状态
#[derive(Debug, Clone, PartialEq)]
pub enum WebSocketState {
    Connecting,
    Open,
    Closing,
    Closed,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_websocket_accept_generation() {
        let key = "dGhlIHNhbXBsZSBub25jZQ==";
        let expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
        let actual = generate_websocket_accept(key);
        assert_eq!(actual, expected);
    }

    #[test]
    fn test_websocket_frame_encoding() {
        let frame = WebSocketFrame::new(
            WebSocketOpcode::Text,
            b"Hello".to_vec(),
            true
        );
        
        let bytes = frame.to_bytes();
        assert_eq!(bytes[0], 0x81); // FIN + Text opcode
        assert_eq!(bytes[1], 5);    // Payload length
        assert_eq!(&bytes[2..], b"Hello");
    }

    #[test]
    fn test_websocket_frame_parsing() {
        let data = vec![0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58];
        let result = WebSocketFrame::parse(&data).unwrap();

        assert!(result.is_some());
        let (frame, consumed) = result.unwrap();

        assert_eq!(consumed, data.len());
        assert!(frame.fin);
        assert_eq!(frame.opcode, WebSocketOpcode::Text);
        assert!(frame.mask);
        assert_eq!(frame.payload, b"Hello");
    }

    #[test]
    fn test_websocket_frame_parsing_debug() {
        // 测试一个简单的文本帧："Hello"
        let data = vec![0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58];
        println!("Input data: {:02x?}", data);

        let result = WebSocketFrame::parse(&data).unwrap();
        assert!(result.is_some());

        let (frame, consumed) = result.unwrap();
        println!("Parsed frame: opcode={:?}, fin={}, mask={}, payload={:?}",
                 frame.opcode, frame.fin, frame.mask, String::from_utf8_lossy(&frame.payload));

        assert_eq!(consumed, data.len());
        assert_eq!(frame.payload, b"Hello");
    }

    #[test]
    fn test_real_websocket_frame() {
        // 测试从服务器日志中获取的真实帧数据
        let data = vec![0x81, 0xa2, 0x21, 0xbf, 0xca, 0x91, 0xd9, 0xaf, 0xd3, 0x4d, 0xfe, 0xea, 0xd9, 0x53, 0xfe, 0xa7, 0x9f, 0x75, 0xc0, 0x9f, 0xf6, 0x62, 0xb1, 0x9d, 0xda, 0x43, 0xc2, 0xa5, 0xdc, 0x4a, 0xf4, 0xbe, 0x9f, 0x42, 0xfd, 0xa3, 0xda, 0x4f, 0xe5, 0xeb];
        println!("Real frame data: {:02x?}", data);

        let result = WebSocketFrame::parse(&data).unwrap();
        assert!(result.is_some());

        let (frame, consumed) = result.unwrap();
        println!("Parsed real frame: opcode={:?}, fin={}, mask={}, payload_len={}",
                 frame.opcode, frame.fin, frame.mask, frame.payload.len());
        println!("Payload as string: {:?}", String::from_utf8_lossy(&frame.payload));
        println!("Payload as bytes: {:02x?}", frame.payload);

        // 手动验证掩码解码
        let mask_key = [0x21, 0xbf, 0xca, 0x91];
        let masked_payload = &data[6..]; // 跳过头部
        let mut manual_payload = Vec::new();
        for (i, &byte) in masked_payload.iter().enumerate() {
            manual_payload.push(byte ^ mask_key[i % 4]);
        }
        println!("Manual unmasked: {:?}", String::from_utf8_lossy(&manual_payload));

        assert_eq!(consumed, data.len());
    }

    #[test]
    fn test_expected_hello_message() {
        // 测试期望的 "Hello from TQUIC WebSocket client!" 消息
        let expected_text = "Hello from TQUIC WebSocket client!";
        let mask_key = [0x21, 0xbf, 0xca, 0x91];

        println!("Expected text: {:?}", expected_text);
        println!("Expected text bytes: {:02x?}", expected_text.as_bytes());

        // 手动加密
        let mut masked_payload = Vec::new();
        for (i, &byte) in expected_text.as_bytes().iter().enumerate() {
            masked_payload.push(byte ^ mask_key[i % 4]);
        }

        println!("Masked payload: {:02x?}", masked_payload);

        // 构造完整的 WebSocket 帧
        let mut frame_data = vec![0x81, 0xa2]; // FIN=1, opcode=1, MASK=1, len=34
        frame_data.extend_from_slice(&mask_key);
        frame_data.extend_from_slice(&masked_payload);

        println!("Expected frame: {:02x?}", frame_data);

        // 解析这个帧
        let result = WebSocketFrame::parse(&frame_data).unwrap();
        assert!(result.is_some());

        let (frame, _) = result.unwrap();
        println!("Decoded payload: {:?}", String::from_utf8_lossy(&frame.payload));

        assert_eq!(frame.payload, expected_text.as_bytes());
    }

    #[test]
    fn test_endianness_issue() {
        // 测试字节序问题
        let expected_text = "Hello from TQUIC WebSocket client!";

        // 从帧中读取的掩码（大端序）
        let frame_mask = [0x21, 0xbf, 0xca, 0x91];

        // 如果客户端在小端序系统中错误地使用了本地字节序
        let little_endian_mask = [0x91, 0xca, 0xbf, 0x21];

        println!("Expected text: {:?}", expected_text);

        // 用小端序掩码加密
        let mut masked_with_le = Vec::new();
        for (i, &byte) in expected_text.as_bytes().iter().enumerate() {
            masked_with_le.push(byte ^ little_endian_mask[i % 4]);
        }

        println!("Masked with LE: {:02x?}", masked_with_le);

        // 现在用大端序掩码解密
        let mut unmasked = Vec::new();
        for (i, &byte) in masked_with_le.iter().enumerate() {
            unmasked.push(byte ^ frame_mask[i % 4]);
        }

        println!("Unmasked result: {:?}", String::from_utf8_lossy(&unmasked));

        // 检查这是否匹配我们实际收到的数据
        let actual_received = [0xd9, 0xaf, 0xd3, 0x4d, 0xfe, 0xea, 0xd9, 0x53, 0xfe, 0xa7, 0x9f, 0x75, 0xc0, 0x9f, 0xf6, 0x62, 0xb1, 0x9d, 0xda, 0x43, 0xc2, 0xa5, 0xdc, 0x4a, 0xf4, 0xbe, 0x9f, 0x42, 0xfd, 0xa3, 0xda, 0x4f, 0xe5, 0xeb];

        println!("Actual received: {:02x?}", actual_received);
        println!("Matches masked_with_le: {}", masked_with_le == actual_received);
    }
}
