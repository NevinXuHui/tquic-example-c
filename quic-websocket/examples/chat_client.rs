use anyhow::{Context, Result};
use clap::Parser;
use quinn::{ClientConfig, Endpoint};
use quic_websocket::message::{MessageFrame, MessageType};
use std::io::{self, Write};
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tracing::{debug, error, info, warn};

#[derive(Parser, Debug)]
#[command(author, version, about = "QUIC WebSocket interactive chat client")]
struct Args {
    /// Server address to connect to
    #[arg(short, long, default_value = "127.0.0.1:4433")]
    server: SocketAddr,

    /// Client name
    #[arg(short, long, default_value = "ChatUser")]
    name: String,

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

    println!("🚀 QUIC WebSocket Chat Client");
    println!("Connecting to: {}", args.server);
    println!("Your name: {}", args.name);
    println!();

    // Create client configuration
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

    println!("✅ Connected successfully!");

    // Send handshake
    let handshake = MessageFrame::new(MessageType::Handshake {
        client_name: Some(args.name.clone()),
        protocol_version: "1.0".to_string(),
    });

    send_message(&connection, &handshake).await?;

    // Start receiving messages
    let connection_recv = connection.clone();
    let recv_handle = tokio::spawn(async move {
        if let Err(e) = receive_messages(connection_recv).await {
            error!("Receive error: {}", e);
        }
    });

    // Wait for handshake response
    tokio::time::sleep(tokio::time::Duration::from_millis(500)).await;

    println!();
    println!("💬 Chat Commands:");
    println!("  /broadcast <message>  - Send message to all users");
    println!("  /list                 - List all connected users");
    println!("  /ping                 - Send ping to server");
    println!("  /quit                 - Exit the chat");
    println!("  <message>             - Send regular message");
    println!();
    println!("Type your messages and press Enter:");

    // Handle user input
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

                if let Err(e) = handle_user_input(&connection, input).await {
                    error!("Error handling input: {}", e);
                }

                if input == "/quit" {
                    break;
                }
            }
            Err(e) => {
                error!("Error reading input: {}", e);
                break;
            }
        }
    }

    // Send close message
    let close = MessageFrame::new(MessageType::Close {
        code: 1000,
        reason: "User quit".to_string(),
    });
    let _ = send_message(&connection, &close).await;

    // Close connection
    connection.close(quinn::VarInt::from_u32(0), b"User quit");
    recv_handle.abort();

    println!("👋 Goodbye!");
    Ok(())
}

async fn handle_user_input(connection: &quinn::Connection, input: &str) -> Result<()> {
    if input.starts_with('/') {
        // Handle commands
        let parts: Vec<&str> = input.splitn(2, ' ').collect();
        let command = parts[0];
        let args = parts.get(1).unwrap_or(&"");

        match command {
            "/broadcast" => {
                if args.is_empty() {
                    println!("Usage: /broadcast <message>");
                    return Ok(());
                }
                let broadcast = MessageFrame::new(MessageType::Broadcast {
                    from: uuid::Uuid::new_v4(), // Server will set the correct ID
                    content: args.to_string(),
                    timestamp: current_timestamp(),
                });
                send_message(connection, &broadcast).await?;
            }
            "/list" => {
                let list_request = MessageFrame::new(MessageType::ListClients);
                send_message(connection, &list_request).await?;
            }
            "/ping" => {
                let ping = MessageFrame::new(MessageType::Ping {
                    timestamp: current_timestamp(),
                });
                send_message(connection, &ping).await?;
            }
            "/quit" => {
                // Will be handled in main loop
            }
            _ => {
                println!("Unknown command: {}", command);
                println!("Available commands: /broadcast, /list, /ping, /quit");
            }
        }
    } else {
        // Regular message
        let message = MessageFrame::new(MessageType::Text {
            content: input.to_string(),
            timestamp: current_timestamp(),
        });
        send_message(connection, &message).await?;
    }

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
                println!("\n🔌 Server closed connection");
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
                println!("✅ Connected to server: {}", server_name);
            } else {
                println!("❌ Connection rejected: {:?}", reason);
            }
        }
        MessageType::Text { content, .. } => {
            println!("💬 Server: {}", content);
        }
        MessageType::Broadcast { from, content, .. } => {
            println!("📢 Broadcast from {}: {}", from, content);
        }
        MessageType::DirectMessage { from, content, .. } => {
            println!("📩 Direct message from {}: {}", from, content);
        }
        MessageType::Pong { .. } => {
            println!("🏓 Pong received");
        }
        MessageType::ClientList { clients } => {
            println!("👥 Connected users ({}):", clients.len());
            for client in clients {
                println!("  • {} ({})", 
                        client.name.as_deref().unwrap_or("Anonymous"),
                        client.id);
            }
        }
        MessageType::Error { code, message } => {
            println!("❌ Error {}: {}", code, message);
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
