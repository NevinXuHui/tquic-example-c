use anyhow::{Context, Result};
use clap::Parser;
use quinn::{ClientConfig, Endpoint};
use std::net::SocketAddr;
use std::sync::Arc;
use tracing::info;

#[derive(Parser, Debug)]
#[command(author, version, about = "Simple HTTP/3 client to test tquic server")]
struct Args {
    /// Server address to connect to
    #[arg(short, long, default_value = "127.0.0.1:4433")]
    server: SocketAddr,

    /// Enable verbose logging
    #[arg(short, long)]
    verbose: bool,
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

    println!("🚀 Simple HTTP/3 Client (tquic compatible)");
    println!("Connecting to: {}", args.server);

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
    info!("Connected to: {}", connection.remote_address());

    // Test 1: Simple HTTP request
    println!("\n📤 Test 1: Sending simple HTTP request...");
    let (mut send_stream, mut recv_stream) = connection
        .open_bi()
        .await
        .context("Failed to open bidirectional stream")?;

    let http_request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_stream.write_all(http_request.as_bytes()).await?;
    send_stream.finish().await?;

    let response = recv_stream.read_to_end(4096).await?;
    let response_text = String::from_utf8_lossy(&response);
    
    println!("📥 Server response:");
    println!("{}", response_text);

    // Test 2: WebSocket upgrade request
    println!("\n📤 Test 2: Sending WebSocket upgrade request...");
    let (mut ws_send, mut ws_recv) = connection
        .open_bi()
        .await
        .context("Failed to open WebSocket stream")?;

    let websocket_request = format!(
        "GET / HTTP/1.1\r\n\
         Host: localhost\r\n\
         Upgrade: websocket\r\n\
         Connection: Upgrade\r\n\
         Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\
         Sec-WebSocket-Version: 13\r\n\
         \r\n"
    );

    ws_send.write_all(websocket_request.as_bytes()).await?;
    
    // Read WebSocket upgrade response
    let mut ws_response = vec![0u8; 2048];
    let ws_len = ws_recv.read(&mut ws_response).await?.unwrap_or(0);
    let ws_response_text = String::from_utf8_lossy(&ws_response[..ws_len]);
    
    println!("📥 WebSocket upgrade response:");
    println!("{}", ws_response_text);

    if ws_response_text.contains("101") && ws_response_text.contains("websocket") {
        println!("✅ WebSocket upgrade successful!");
        
        // Send a simple WebSocket text frame
        println!("\n📤 Sending WebSocket text message...");
        let message = "Hello from HTTP/3 WebSocket client!";
        let frame = create_websocket_text_frame(message, true);
        ws_send.write_all(&frame).await?;

        // Try to read WebSocket response
        tokio::time::sleep(tokio::time::Duration::from_millis(500)).await;
        let mut ws_msg = vec![0u8; 1024];
        match ws_recv.read(&mut ws_msg).await {
            Ok(Some(len)) if len > 0 => {
                println!("📥 WebSocket response ({} bytes):", len);
                // Try to parse as WebSocket frame or show raw data
                if let Ok(text) = String::from_utf8(ws_msg[..len].to_vec()) {
                    println!("Text: {}", text);
                } else {
                    println!("Binary: {:?}", &ws_msg[..len]);
                }
            }
            Ok(_) => println!("📥 No WebSocket response received"),
            Err(e) => println!("❌ Error reading WebSocket response: {}", e),
        }

        // Send close frame
        let close_frame = create_websocket_close_frame(1000, "Normal closure", true);
        ws_send.write_all(&close_frame).await?;
        ws_send.finish().await?;
    } else {
        println!("❌ WebSocket upgrade failed");
    }

    // Close connection
    connection.close(quinn::VarInt::from_u32(0), b"Test completed");
    
    println!("\n✅ Test completed!");
    Ok(())
}

fn create_websocket_text_frame(text: &str, masked: bool) -> Vec<u8> {
    let payload = text.as_bytes();
    let payload_len = payload.len();
    
    let mut frame = Vec::new();
    
    // FIN=1, RSV=000, Opcode=0001 (text)
    frame.push(0x81);
    
    // MASK bit + payload length
    let mask_bit = if masked { 0x80 } else { 0x00 };
    
    if payload_len < 126 {
        frame.push(mask_bit | (payload_len as u8));
    } else if payload_len < 65536 {
        frame.push(mask_bit | 126);
        frame.extend_from_slice(&(payload_len as u16).to_be_bytes());
    } else {
        frame.push(mask_bit | 127);
        frame.extend_from_slice(&(payload_len as u64).to_be_bytes());
    }
    
    // Masking key and masked payload
    if masked {
        let mask = [
            rand::random::<u8>(),
            rand::random::<u8>(),
            rand::random::<u8>(),
            rand::random::<u8>(),
        ];
        frame.extend_from_slice(&mask);
        
        for (i, &byte) in payload.iter().enumerate() {
            frame.push(byte ^ mask[i % 4]);
        }
    } else {
        frame.extend_from_slice(payload);
    }
    
    frame
}

fn create_websocket_close_frame(code: u16, reason: &str, masked: bool) -> Vec<u8> {
    let mut payload = Vec::new();
    payload.extend_from_slice(&code.to_be_bytes());
    payload.extend_from_slice(reason.as_bytes());
    
    let mut frame = Vec::new();
    
    // FIN=1, RSV=000, Opcode=1000 (close)
    frame.push(0x88);
    
    // MASK bit + payload length
    let mask_bit = if masked { 0x80 } else { 0x00 };
    frame.push(mask_bit | (payload.len() as u8));
    
    // Masking key and masked payload
    if masked {
        let mask = [
            rand::random::<u8>(),
            rand::random::<u8>(),
            rand::random::<u8>(),
            rand::random::<u8>(),
        ];
        frame.extend_from_slice(&mask);
        
        for (i, &byte) in payload.iter().enumerate() {
            frame.push(byte ^ mask[i % 4]);
        }
    } else {
        frame.extend_from_slice(&payload);
    }
    
    frame
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
