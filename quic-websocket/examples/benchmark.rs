use anyhow::{Context, Result};
use clap::Parser;
use quinn::{ClientConfig, Endpoint};
use quic_websocket::message::{MessageFrame, MessageType};
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::Semaphore;
use tokio::time::sleep;
use tracing::{debug, error, info, warn};

#[derive(Parser, Debug)]
#[command(author, version, about = "QUIC WebSocket benchmark client")]
struct Args {
    /// Server address to connect to
    #[arg(short, long, default_value = "127.0.0.1:4433")]
    server: SocketAddr,

    /// Number of concurrent clients
    #[arg(short, long, default_value_t = 10)]
    clients: usize,

    /// Number of messages per client
    #[arg(short, long, default_value_t = 100)]
    messages: usize,

    /// Message size in bytes
    #[arg(long, default_value_t = 1024)]
    message_size: usize,

    /// Test duration in seconds (0 = use message count)
    #[arg(short, long, default_value_t = 0)]
    duration: u64,

    /// Delay between messages in milliseconds
    #[arg(long, default_value_t = 10)]
    delay_ms: u64,

    /// Enable verbose logging
    #[arg(short, long)]
    verbose: bool,
}

#[derive(Debug)]
struct BenchmarkStats {
    messages_sent: AtomicU64,
    messages_received: AtomicU64,
    bytes_sent: AtomicU64,
    bytes_received: AtomicU64,
    errors: AtomicU64,
    start_time: Instant,
}

impl BenchmarkStats {
    fn new() -> Self {
        Self {
            messages_sent: AtomicU64::new(0),
            messages_received: AtomicU64::new(0),
            bytes_sent: AtomicU64::new(0),
            bytes_received: AtomicU64::new(0),
            errors: AtomicU64::new(0),
            start_time: Instant::now(),
        }
    }

    fn print_stats(&self) {
        let elapsed = self.start_time.elapsed();
        let sent = self.messages_sent.load(Ordering::Relaxed);
        let received = self.messages_received.load(Ordering::Relaxed);
        let bytes_sent = self.bytes_sent.load(Ordering::Relaxed);
        let bytes_received = self.bytes_received.load(Ordering::Relaxed);
        let errors = self.errors.load(Ordering::Relaxed);

        let elapsed_secs = elapsed.as_secs_f64();
        let send_rate = sent as f64 / elapsed_secs;
        let recv_rate = received as f64 / elapsed_secs;
        let send_throughput = bytes_sent as f64 / elapsed_secs / 1024.0 / 1024.0; // MB/s
        let recv_throughput = bytes_received as f64 / elapsed_secs / 1024.0 / 1024.0; // MB/s

        println!("\n📊 Benchmark Results:");
        println!("  Duration:           {:.2}s", elapsed_secs);
        println!("  Messages Sent:      {}", sent);
        println!("  Messages Received:  {}", received);
        println!("  Bytes Sent:         {} ({:.2} MB)", bytes_sent, bytes_sent as f64 / 1024.0 / 1024.0);
        println!("  Bytes Received:     {} ({:.2} MB)", bytes_received, bytes_received as f64 / 1024.0 / 1024.0);
        println!("  Errors:             {}", errors);
        println!("  Send Rate:          {:.2} msg/s", send_rate);
        println!("  Receive Rate:       {:.2} msg/s", recv_rate);
        println!("  Send Throughput:    {:.2} MB/s", send_throughput);
        println!("  Receive Throughput: {:.2} MB/s", recv_throughput);
        println!("  Success Rate:       {:.2}%", (received as f64 / sent as f64) * 100.0);
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

    info!("🚀 QUIC WebSocket Benchmark Client");
    info!("Server: {}", args.server);
    info!("Clients: {}", args.clients);
    info!("Messages per client: {}", args.messages);
    info!("Message size: {} bytes", args.message_size);
    info!("Test duration: {}s", if args.duration > 0 { args.duration.to_string() } else { "unlimited".to_string() });
    info!("Delay between messages: {}ms", args.delay_ms);

    let stats = Arc::new(BenchmarkStats::new());
    let semaphore = Arc::new(Semaphore::new(args.clients));

    // Create client configuration
    let client_config = create_client_config()?;

    // Spawn client tasks
    let mut handles = Vec::new();
    for client_id in 0..args.clients {
        let stats = stats.clone();
        let semaphore = semaphore.clone();
        let client_config = client_config.clone();
        let server_addr = args.server;
        let messages = args.messages;
        let message_size = args.message_size;
        let duration = args.duration;
        let delay_ms = args.delay_ms;

        let handle = tokio::spawn(async move {
            let _permit = semaphore.acquire().await.unwrap();
            
            if let Err(e) = run_client(
                client_id,
                server_addr,
                client_config,
                messages,
                message_size,
                duration,
                delay_ms,
                stats,
            ).await {
                error!("Client {} error: {}", client_id, e);
            }
        });

        handles.push(handle);
    }

    // Print periodic stats
    let stats_printer = stats.clone();
    let print_handle = tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(5));
        loop {
            interval.tick().await;
            stats_printer.print_stats();
        }
    });

    // Wait for all clients to complete
    for handle in handles {
        let _ = handle.await;
    }

    print_handle.abort();
    stats.print_stats();

    Ok(())
}

async fn run_client(
    client_id: usize,
    server_addr: SocketAddr,
    client_config: ClientConfig,
    message_count: usize,
    message_size: usize,
    duration_secs: u64,
    delay_ms: u64,
    stats: Arc<BenchmarkStats>,
) -> Result<()> {
    // Create endpoint
    let mut endpoint = Endpoint::client("0.0.0.0:0".parse()?)?;
    endpoint.set_default_client_config(client_config);

    // Connect to server
    let connection = endpoint
        .connect(server_addr, "localhost")?
        .await
        .context("Failed to connect to server")?;

    debug!("Client {} connected", client_id);

    // Send handshake
    let handshake = MessageFrame::new(MessageType::Handshake {
        client_name: Some(format!("BenchClient{}", client_id)),
        protocol_version: "1.0".to_string(),
    });

    send_message(&connection, &handshake, &stats).await?;

    // Start receiving messages
    let connection_recv = connection.clone();
    let stats_recv = stats.clone();
    let recv_handle = tokio::spawn(async move {
        let _ = receive_messages(connection_recv, stats_recv).await;
    });

    // Wait for handshake response
    sleep(Duration::from_millis(100)).await;

    // Generate test message
    let test_data = "x".repeat(message_size);
    let end_time = if duration_secs > 0 {
        Some(Instant::now() + Duration::from_secs(duration_secs))
    } else {
        None
    };

    // Send messages
    let mut sent_count = 0;
    loop {
        // Check if we should stop
        if let Some(end_time) = end_time {
            if Instant::now() >= end_time {
                break;
            }
        } else if sent_count >= message_count {
            break;
        }

        let message = MessageFrame::new(MessageType::Text {
            content: format!("{}:{}", client_id, test_data),
            timestamp: current_timestamp(),
        });

        if let Err(e) = send_message(&connection, &message, &stats).await {
            error!("Client {} send error: {}", client_id, e);
            stats.errors.fetch_add(1, Ordering::Relaxed);
        }

        sent_count += 1;

        if delay_ms > 0 {
            sleep(Duration::from_millis(delay_ms)).await;
        }
    }

    debug!("Client {} sent {} messages", client_id, sent_count);

    // Wait a bit for final responses
    sleep(Duration::from_secs(1)).await;

    // Close connection
    connection.close(quinn::VarInt::from_u32(0), b"Benchmark completed");
    recv_handle.abort();

    Ok(())
}

async fn send_message(
    connection: &quinn::Connection,
    frame: &MessageFrame,
    stats: &BenchmarkStats,
) -> Result<()> {
    let data = frame.to_bytes()?;
    
    let mut send_stream = connection.open_uni().await?;
    
    // Send message length + data
    let len = data.len() as u32;
    send_stream.write_all(&len.to_be_bytes()).await?;
    send_stream.write_all(&data).await?;
    send_stream.finish().await?;

    stats.messages_sent.fetch_add(1, Ordering::Relaxed);
    stats.bytes_sent.fetch_add(data.len() as u64, Ordering::Relaxed);

    Ok(())
}

async fn receive_messages(
    connection: quinn::Connection,
    stats: Arc<BenchmarkStats>,
) -> Result<()> {
    loop {
        match connection.accept_uni().await {
            Ok(mut recv_stream) => {
                let stats = stats.clone();
                tokio::spawn(async move {
                    if let Err(e) = handle_incoming_message(&mut recv_stream, &stats).await {
                        debug!("Error handling incoming message: {}", e);
                    }
                });
            }
            Err(quinn::ConnectionError::ApplicationClosed { .. }) => {
                break;
            }
            Err(e) => {
                debug!("Error accepting stream: {}", e);
                break;
            }
        }
    }
    Ok(())
}

async fn handle_incoming_message(
    recv_stream: &mut quinn::RecvStream,
    stats: &BenchmarkStats,
) -> Result<()> {
    // Read message length
    let mut len_bytes = [0u8; 4];
    recv_stream.read_exact(&mut len_bytes).await?;
    let message_len = u32::from_be_bytes(len_bytes) as usize;

    // Read message data
    let mut message_data = vec![0u8; message_len];
    recv_stream.read_exact(&mut message_data).await?;

    stats.messages_received.fetch_add(1, Ordering::Relaxed);
    stats.bytes_received.fetch_add(message_data.len() as u64, Ordering::Relaxed);

    Ok(())
}

fn create_client_config() -> Result<ClientConfig> {
    let mut crypto = rustls::ClientConfig::builder()
        .with_safe_defaults()
        .with_custom_certificate_verifier(Arc::new(SkipServerVerification))
        .with_no_client_auth();

    crypto.alpn_protocols = vec![b"quic-websocket".to_vec()];

    let client_config = ClientConfig::new(Arc::new(crypto));

    Ok(client_config)
}

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
