use anyhow::{Context, Result};
use clap::Parser;
use quinn::{ClientConfig, Endpoint};
use quic_websocket::message::{MessageFrame, MessageType};
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::time::{sleep, Duration};
use tracing::{debug, error, info, warn};

#[derive(Parser, Debug)]
#[command(author, version, about = "QUIC WebSocket test client")]
struct Args {
    /// Server address to connect to
    #[arg(short, long, default_value = "127.0.0.1:4433")]
    server: SocketAddr,

    /// Client name
    #[arg(short, long, default_value = "TestClient")]
    name: String,

    /// Number of messages to send
    #[arg(short, long, default_value_t = 5)]
    count: usize,

    /// Interval between messages (seconds)
    #[arg(short, long, default_value_t = 2)]
    interval: u64,

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

    info!("QUIC WebSocket Test Client");
    info!("Connecting to: {}", args.server);
    info!("Client name: {}", args.name);

    // Create client configuration
    let client_config = create_client_config()?;

    // Create endpoint
    let mut endpoint = Endpoint::client("0.0.0.0:0".parse()?)?;
    endpoint.set_default_client_config(client_config);

    // Connect to server
    info!("Connecting to server...");
    let connection = endpoint
        .connect(args.server, "localhost")?
        .await
        .context("Failed to connect to server")?;

    info!("Connected successfully!");

    // Send handshake
    let handshake = MessageFrame::new(MessageType::Handshake {
        client_name: Some(args.name.clone()),
        protocol_version: "1.0".to_string(),
    });

    send_message(&connection, &handshake).await?;
    info!("Handshake sent");

    // Start receiving messages
    let connection_recv = connection.clone();
    let recv_handle = tokio::spawn(async move {
        if let Err(e) = receive_messages(connection_recv).await {
            error!("Receive error: {}", e);
        }
    });

    // Wait a bit for handshake response
    sleep(Duration::from_millis(500)).await;

    // Send test messages
    for i in 1..=args.count {
        let message = MessageFrame::new(MessageType::Text {
            content: format!("Test message {} from {}", i, args.name),
            timestamp: current_timestamp(),
        });

        send_message(&connection, &message).await?;
        info!("Sent message {}/{}", i, args.count);

        if i < args.count {
            sleep(Duration::from_secs(args.interval)).await;
        }
    }

    // Send ping
    let ping = MessageFrame::new(MessageType::Ping {
        timestamp: current_timestamp(),
    });
    send_message(&connection, &ping).await?;
    info!("Sent ping");

    // Request client list
    let list_request = MessageFrame::new(MessageType::ListClients);
    send_message(&connection, &list_request).await?;
    info!("Requested client list");

    // Send broadcast message
    let broadcast = MessageFrame::new(MessageType::Broadcast {
        from: uuid::Uuid::new_v4(), // This would normally be set by the server
        content: format!("Broadcast from {}", args.name),
        timestamp: current_timestamp(),
    });
    send_message(&connection, &broadcast).await?;
    info!("Sent broadcast message");

    // Wait for responses
    sleep(Duration::from_secs(3)).await;

    // Send close message
    let close = MessageFrame::new(MessageType::Close {
        code: 1000,
        reason: "Test completed".to_string(),
    });
    send_message(&connection, &close).await?;
    info!("Sent close message");

    // Wait for final messages
    sleep(Duration::from_secs(1)).await;

    // Close connection
    connection.close(quinn::VarInt::from_u32(0), b"Test completed");
    recv_handle.abort();

    info!("Test completed successfully");
    Ok(())
}

async fn send_message(connection: &quinn::Connection, frame: &MessageFrame) -> Result<()> {
    let data = frame.to_bytes()?;
    
    let mut send_stream = connection.open_uni().await?;
    
    // Send message length + data
    let len = data.len() as u32;
    send_stream.write_all(&len.to_be_bytes()).await?;
    send_stream.write_all(&data).await?;
    send_stream.finish().await?;

    debug!("Sent: {}", frame.message_type);
    Ok(())
}

async fn receive_messages(connection: quinn::Connection) -> Result<()> {
    loop {
        match connection.accept_uni().await {
            Ok(mut recv_stream) => {
                tokio::spawn(async move {
                    if let Err(e) = handle_incoming_message(&mut recv_stream).await {
                        error!("Error handling incoming message: {}", e);
                    }
                });
            }
            Err(quinn::ConnectionError::ApplicationClosed { .. }) => {
                info!("Server closed connection");
                break;
            }
            Err(e) => {
                warn!("Error accepting stream: {}", e);
                break;
            }
        }
    }
    Ok(())
}

async fn handle_incoming_message(recv_stream: &mut quinn::RecvStream) -> Result<()> {
    // Read message length
    let mut len_bytes = [0u8; 4];
    recv_stream.read_exact(&mut len_bytes).await?;
    let message_len = u32::from_be_bytes(len_bytes) as usize;

    // Read message data
    let mut message_data = vec![0u8; message_len];
    recv_stream.read_exact(&mut message_data).await?;

    // Deserialize message
    let frame = MessageFrame::from_bytes(&message_data)?;
    
    match &frame.message_type {
        MessageType::HandshakeResponse { accepted, server_name, reason, .. } => {
            if *accepted {
                info!("✅ Handshake accepted by server: {}", server_name);
            } else {
                warn!("❌ Handshake rejected: {:?}", reason);
            }
        }
        MessageType::Text { content, .. } => {
            info!("📝 Received text: {}", content);
        }
        MessageType::Broadcast { from, content, .. } => {
            info!("📢 Broadcast from {}: {}", from, content);
        }
        MessageType::Pong { .. } => {
            info!("🏓 Received pong");
        }
        MessageType::ClientList { clients } => {
            info!("👥 Client list ({} clients):", clients.len());
            for client in clients {
                info!("  - {} ({})", 
                      client.id, 
                      client.name.as_deref().unwrap_or("Anonymous"));
            }
        }
        MessageType::Error { code, message } => {
            error!("❌ Server error {}: {}", code, message);
        }
        _ => {
            debug!("Received: {}", frame.message_type);
        }
    }

    Ok(())
}

fn create_client_config() -> Result<ClientConfig> {
    let mut crypto = rustls::ClientConfig::builder()
        .with_safe_defaults()
        .with_custom_certificate_verifier(Arc::new(SkipServerVerification))
        .with_no_client_auth();

    // Set ALPN protocol
    crypto.alpn_protocols = vec![b"quic-websocket".to_vec()];

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

fn current_timestamp() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs()
}
