use anyhow::{Context, Result};
use clap::Parser;
use quinn::{ClientConfig, Endpoint};
use std::io::{self, Write};
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tracing::{debug, error, warn};
use base64::{Engine as _, engine::general_purpose};

#[derive(Parser, Debug)]
#[command(author, version, about = "HTTP/3 WebSocket client compatible with tquic")]
struct Args {
    /// Server address to connect to
    #[arg(short, long, default_value = "127.0.0.1:4433")]
    server: SocketAddr,

    /// Client name
    #[arg(short, long, default_value = "HTTP3WebSocketUser")]
    name: String,

    /// Enable verbose logging
    #[arg(short, long)]
    verbose: bool,
}

// WebSocket 帧类型
#[derive(Debug)]
enum WebSocketOpcode {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xa,
}

// WebSocket 帧结构
#[derive(Debug)]
struct WebSocketFrame {
    fin: bool,
    opcode: u8,
    masked: bool,
    payload_len: u64,
    masking_key: Option<[u8; 4]>,
    payload: Vec<u8>,
}

impl WebSocketFrame {
    fn new_text(text: &str, masked: bool) -> Self {
        let payload = text.as_bytes().to_vec();
        let masking_key = if masked {
            Some([
                rand::random(),
                rand::random(),
                rand::random(),
                rand::random(),
            ])
        } else {
            None
        };

        Self {
            fin: true,
            opcode: WebSocketOpcode::Text as u8,
            masked,
            payload_len: payload.len() as u64,
            masking_key,
            payload,
        }
    }

    fn new_ping(masked: bool) -> Self {
        let masking_key = if masked {
            Some([
                rand::random(),
                rand::random(),
                rand::random(),
                rand::random(),
            ])
        } else {
            None
        };

        Self {
            fin: true,
            opcode: WebSocketOpcode::Ping as u8,
            masked,
            payload_len: 0,
            masking_key,
            payload: Vec::new(),
        }
    }

    fn new_close(code: u16, reason: &str, masked: bool) -> Self {
        let mut payload = Vec::new();
        payload.extend_from_slice(&code.to_be_bytes());
        payload.extend_from_slice(reason.as_bytes());

        let masking_key = if masked {
            Some([
                rand::random(),
                rand::random(),
                rand::random(),
                rand::random(),
            ])
        } else {
            None
        };

        Self {
            fin: true,
            opcode: WebSocketOpcode::Close as u8,
            masked,
            payload_len: payload.len() as u64,
            masking_key,
            payload,
        }
    }

    fn to_bytes(&self) -> Vec<u8> {
        let mut frame = Vec::new();

        // 第一个字节：FIN + RSV + Opcode
        let first_byte = if self.fin { 0x80 } else { 0x00 } | (self.opcode & 0x0F);
        frame.push(first_byte);

        // 第二个字节：MASK + Payload length
        let mask_bit = if self.masked { 0x80 } else { 0x00 };
        
        if self.payload_len < 126 {
            frame.push(mask_bit | (self.payload_len as u8));
        } else if self.payload_len < 65536 {
            frame.push(mask_bit | 126);
            frame.extend_from_slice(&(self.payload_len as u16).to_be_bytes());
        } else {
            frame.push(mask_bit | 127);
            frame.extend_from_slice(&self.payload_len.to_be_bytes());
        }

        // Masking key
        if let Some(key) = self.masking_key {
            frame.extend_from_slice(&key);
        }

        // Payload (masked if needed)
        if self.masked && self.masking_key.is_some() {
            let key = self.masking_key.unwrap();
            let mut masked_payload = self.payload.clone();
            for (i, byte) in masked_payload.iter_mut().enumerate() {
                *byte ^= key[i % 4];
            }
            frame.extend_from_slice(&masked_payload);
        } else {
            frame.extend_from_slice(&self.payload);
        }

        frame
    }

    fn from_bytes(data: &[u8]) -> Result<Self> {
        if data.len() < 2 {
            return Err(anyhow::anyhow!("Frame too short"));
        }

        let first_byte = data[0];
        let fin = (first_byte & 0x80) != 0;
        let opcode = first_byte & 0x0F;

        let second_byte = data[1];
        let masked = (second_byte & 0x80) != 0;
        let mut payload_len = (second_byte & 0x7F) as u64;

        let mut offset = 2;

        // Extended payload length
        if payload_len == 126 {
            if data.len() < offset + 2 {
                return Err(anyhow::anyhow!("Frame too short for extended length"));
            }
            payload_len = u16::from_be_bytes([data[offset], data[offset + 1]]) as u64;
            offset += 2;
        } else if payload_len == 127 {
            if data.len() < offset + 8 {
                return Err(anyhow::anyhow!("Frame too short for extended length"));
            }
            payload_len = u64::from_be_bytes([
                data[offset], data[offset + 1], data[offset + 2], data[offset + 3],
                data[offset + 4], data[offset + 5], data[offset + 6], data[offset + 7],
            ]);
            offset += 8;
        }

        // Masking key
        let masking_key = if masked {
            if data.len() < offset + 4 {
                return Err(anyhow::anyhow!("Frame too short for masking key"));
            }
            let key = [data[offset], data[offset + 1], data[offset + 2], data[offset + 3]];
            offset += 4;
            Some(key)
        } else {
            None
        };

        // Payload
        if data.len() < offset + payload_len as usize {
            return Err(anyhow::anyhow!("Frame too short for payload"));
        }

        let mut payload = data[offset..offset + payload_len as usize].to_vec();

        // Unmask payload if needed
        if let Some(key) = masking_key {
            for (i, byte) in payload.iter_mut().enumerate() {
                *byte ^= key[i % 4];
            }
        }

        Ok(Self {
            fin,
            opcode,
            masked,
            payload_len,
            masking_key,
            payload,
        })
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();

    // Initialize logging
    let log_level = if args.verbose {
        tracing::Level::DEBUG
    } else {
        tracing::Level::INFO
    };

    tracing_subscriber::fmt()
        .with_max_level(log_level)
        .with_target(false)
        .init();

    println!("🚀 HTTP/3 WebSocket Client (tquic compatible)");
    println!("Connecting to: {}", args.server);
    println!("Your name: {}", args.name);
    println!();

    // Create client configuration with HTTP/3 ALPN
    let client_config = create_client_config()?;

    // Create endpoint
    let mut endpoint = Endpoint::client("0.0.0.0:0".parse()?)?;
    endpoint.set_default_client_config(client_config);

    // Connect to server
    println!("🔗 Connecting to server...");
    let connection = endpoint
        .connect(args.server, "localhost")?
        .await
        .context("Failed to connect to server")?;

    println!("✅ QUIC connection established!");

    // Open bidirectional stream for HTTP/3
    let (mut send_stream, mut recv_stream) = connection
        .open_bi()
        .await
        .context("Failed to open bidirectional stream")?;

    // Send HTTP/3 WebSocket upgrade request
    let websocket_key = generate_websocket_key();
    let upgrade_request = format!(
        "GET / HTTP/1.1\r\n\
         Host: localhost\r\n\
         Upgrade: websocket\r\n\
         Connection: Upgrade\r\n\
         Sec-WebSocket-Key: {}\r\n\
         Sec-WebSocket-Version: 13\r\n\
         \r\n",
        websocket_key
    );

    send_stream.write_all(upgrade_request.as_bytes()).await?;
    println!("📤 WebSocket upgrade request sent");

    // Read upgrade response
    let mut response_buffer = vec![0u8; 1024];
    let response_len = recv_stream.read(&mut response_buffer).await?;
    let response_len = response_len.unwrap_or(0);
    let response = String::from_utf8_lossy(&response_buffer[..response_len]);
    
    println!("📥 Server response:");
    println!("{}", response);

    if response.contains("101") && response.contains("websocket") {
        println!("✅ WebSocket upgrade successful!");
        
        // Start receiving WebSocket frames
        let recv_connection = connection.clone();
        let recv_handle = tokio::spawn(async move {
            if let Err(e) = receive_websocket_frames(recv_stream).await {
                error!("Receive error: {}", e);
            }
        });

        // Interactive message sending
        println!();
        println!("💬 WebSocket Commands:");
        println!("  /ping                 - Send ping frame");
        println!("  /quit                 - Close connection");
        println!("  <message>             - Send text message");
        println!();
        println!("Type your messages and press Enter:");

        let stdin = tokio::io::stdin();
        let mut reader = BufReader::new(stdin);
        let mut line = String::new();

        loop {
            print!("> ");
            io::stdout().flush().unwrap();
            
            line.clear();
            match reader.read_line(&mut line).await {
                Ok(0) => break, // EOF
                Ok(_) => {
                    let input = line.trim();
                    if input.is_empty() {
                        continue;
                    }

                    if input == "/quit" {
                        // Send close frame
                        let close_frame = WebSocketFrame::new_close(1000, "Normal closure", true);
                        send_stream.write_all(&close_frame.to_bytes()).await?;
                        break;
                    } else if input == "/ping" {
                        // Send ping frame
                        let ping_frame = WebSocketFrame::new_ping(true);
                        send_stream.write_all(&ping_frame.to_bytes()).await?;
                        println!("🏓 Ping sent");
                    } else {
                        // Send text message
                        let text_frame = WebSocketFrame::new_text(input, true);
                        send_stream.write_all(&text_frame.to_bytes()).await?;
                        println!("📤 Message sent: {}", input);
                    }
                }
                Err(e) => {
                    error!("Error reading input: {}", e);
                    break;
                }
            }
        }

        // Close connection
        connection.close(quinn::VarInt::from_u32(0), b"User quit");
        recv_handle.abort();
    } else {
        println!("❌ WebSocket upgrade failed");
        println!("Server response: {}", response);
    }

    println!("👋 Goodbye!");
    Ok(())
}

async fn receive_websocket_frames(mut recv_stream: quinn::RecvStream) -> Result<()> {
    let mut buffer = vec![0u8; 4096];
    
    loop {
        match recv_stream.read(&mut buffer).await {
            Ok(Some(0)) => {
                println!("🔌 Connection closed by server");
                break;
            }
            Ok(Some(n)) => {
                // Try to parse WebSocket frame
                match WebSocketFrame::from_bytes(&buffer[..n]) {
                    Ok(frame) => {
                        match frame.opcode {
                            0x1 => { // Text frame
                                let text = String::from_utf8_lossy(&frame.payload);
                                println!("📥 Received text: {}", text);
                            }
                            0x2 => { // Binary frame
                                println!("📥 Received binary data ({} bytes)", frame.payload.len());
                            }
                            0x8 => { // Close frame
                                if frame.payload.len() >= 2 {
                                    let code = u16::from_be_bytes([frame.payload[0], frame.payload[1]]);
                                    let reason = if frame.payload.len() > 2 {
                                        String::from_utf8_lossy(&frame.payload[2..])
                                    } else {
                                        "".into()
                                    };
                                    println!("🔌 Connection closed by server: {} - {}", code, reason);
                                } else {
                                    println!("🔌 Connection closed by server");
                                }
                                break;
                            }
                            0x9 => { // Ping frame
                                println!("🏓 Received ping");
                                // Should send pong response
                            }
                            0xa => { // Pong frame
                                println!("🏓 Received pong");
                            }
                            _ => {
                                println!("📥 Received frame with opcode: 0x{:x}", frame.opcode);
                            }
                        }
                    }
                    Err(e) => {
                        debug!("Failed to parse WebSocket frame: {}", e);
                        // Might be HTTP response or other data
                        let text = String::from_utf8_lossy(&buffer[..n]);
                        if !text.trim().is_empty() {
                            println!("📥 Raw data: {}", text.trim());
                        }
                    }
                }
            }
            Ok(None) => {
                // No data available, continue
                tokio::time::sleep(tokio::time::Duration::from_millis(10)).await;
            }
            Err(e) => {
                warn!("Error reading from stream: {}", e);
                break;
            }
        }
    }
    Ok(())
}

fn generate_websocket_key() -> String {
    let mut key = [0u8; 16];
    for byte in &mut key {
        *byte = rand::random();
    }
    general_purpose::STANDARD.encode(key)
}

fn create_client_config() -> Result<ClientConfig> {
    let mut crypto = rustls::ClientConfig::builder()
        .with_safe_defaults()
        .with_custom_certificate_verifier(Arc::new(SkipServerVerification))
        .with_no_client_auth();

    // Set ALPN protocol for HTTP/3
    crypto.alpn_protocols = vec![b"h3".to_vec()];

    let client_config = ClientConfig::new(Arc::new(crypto));

    Ok(client_config)
}

// Skip certificate verification for testing
struct SkipServerVerification;

impl rustls::client::ServerCertVerifier for SkipServerVerification {
    fn verify_server_cert(
        &self,
        _end_entity: &rustls::Certificate,
        _intermediates: &[rustls::Certificate],
        _server_name: &rustls::ServerName,
        _scts: &mut dyn Iterator<Item = &[u8]>,
        _ocsp_response: &[u8],
        _now: std::time::SystemTime,
    ) -> Result<rustls::client::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::ServerCertVerified::assertion())
    }
}
