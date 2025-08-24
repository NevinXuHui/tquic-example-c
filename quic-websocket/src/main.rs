use anyhow::{Context, Result};
use clap::Parser;
use quinn::Endpoint;
use quic_websocket::{QuicWebSocketServer, create_server_config};
use std::net::SocketAddr;
use std::path::PathBuf;
use tokio::signal;
use tracing::{info, error, Level};
use tracing_subscriber;

mod websocket;
mod h3_server;
use h3_server::H3WebSocketServer;

#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Server listening address
    #[arg(short, long, default_value = "127.0.0.1:4433")]
    addr: SocketAddr,

    /// Server name
    #[arg(short, long, default_value = "QUIC WebSocket Server")]
    name: String,

    /// Maximum number of concurrent clients
    #[arg(short, long, default_value_t = 100)]
    max_clients: usize,

    /// Path to TLS certificate file
    #[arg(short, long, default_value = "certs/cert.pem")]
    cert: PathBuf,

    /// Path to TLS private key file
    #[arg(short, long, default_value = "certs/key.pem")]
    key: PathBuf,

    /// Log level
    #[arg(long, default_value = "info")]
    log_level: String,

    /// Enable verbose logging
    #[arg(short, long)]
    verbose: bool,

    /// Server mode: 'custom' for original QUIC WebSocket, 'http3' for HTTP/3 WebSocket
    #[arg(long, default_value = "http3")]
    mode: String,
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();

    // Initialize logging
    let log_level = if args.verbose {
        Level::DEBUG
    } else {
        match args.log_level.to_lowercase().as_str() {
            "trace" => Level::TRACE,
            "debug" => Level::DEBUG,
            "info" => Level::INFO,
            "warn" => Level::WARN,
            "error" => Level::ERROR,
            _ => Level::INFO,
        }
    };

    tracing_subscriber::fmt()
        .with_max_level(log_level)
        .with_target(false)
        .with_thread_ids(true)
        .with_file(true)
        .with_line_number(true)
        .init();

    info!("Starting QUIC WebSocket Server");
    info!("Version: {}", env!("CARGO_PKG_VERSION"));
    info!("Configuration:");
    info!("  Address: {}", args.addr);
    info!("  Server Name: {}", args.name);
    info!("  Max Clients: {}", args.max_clients);
    info!("  Certificate: {}", args.cert.display());
    info!("  Private Key: {}", args.key.display());
    info!("  Log Level: {}", log_level);
    info!("  Mode: {}", args.mode);

    // Check if certificate and key files exist
    if !args.cert.exists() {
        error!("Certificate file not found: {}", args.cert.display());
        error!("Please generate certificates using:");
        error!("  openssl req -x509 -newkey rsa:4096 -keyout {} -out {} -days 365 -nodes -subj '/CN=localhost'", 
               args.key.display(), args.cert.display());
        return Err(anyhow::anyhow!("Certificate file not found"));
    }

    if !args.key.exists() {
        error!("Private key file not found: {}", args.key.display());
        return Err(anyhow::anyhow!("Private key file not found"));
    }

    // Create server configuration
    let server_config = create_server_config(
        args.cert.to_str().context("Invalid certificate path")?,
        args.key.to_str().context("Invalid key path")?,
    ).context("Failed to create server configuration")?;

    // Create endpoint
    let endpoint = Endpoint::server(server_config, args.addr)
        .context("Failed to create server endpoint")?;

    info!("Server endpoint created successfully");

    // Choose server mode based on command line argument
    match args.mode.to_lowercase().as_str() {
        "http3" => {
            info!("🚀 Starting HTTP/3 WebSocket server (compatible with tquic_websocket_client.c)");

            // Create HTTP/3 WebSocket server
            let h3_server = H3WebSocketServer::new(endpoint, args.name);

            // Handle shutdown signals
            tokio::spawn(async move {
                match signal::ctrl_c().await {
                    Ok(()) => {
                        info!("Received Ctrl+C, shutting down gracefully...");
                        std::process::exit(0);
                    }
                    Err(err) => {
                        error!("Unable to listen for shutdown signal: {}", err);
                    }
                }
            });

            // Run HTTP/3 server
            info!("🌐 HTTP/3 WebSocket server starting on {}", args.addr);
            info!("📋 Protocol: WebSocket over HTTP/3 (RFC 9220)");
            info!("🔧 Compatible with: tquic_websocket_client.c");
            info!("Press Ctrl+C to shutdown");

            if let Err(e) = h3_server.run().await {
                error!("HTTP/3 server error: {}", e);
                return Err(e);
            }
        }
        "custom" => {
            info!("🔧 Starting custom QUIC WebSocket server (original implementation)");

            // Create custom server
            let (server, mut broadcast_rx) = QuicWebSocketServer::new(
                endpoint,
                args.name,
                args.max_clients,
            );

            // Spawn broadcast message logger
            tokio::spawn(async move {
                while let Ok(frame) = broadcast_rx.recv().await {
                    info!("Broadcast message: {}", frame.message_type);
                }
            });

            // Create Arc wrapper for sharing server between tasks
            let server = std::sync::Arc::new(server);

            // Spawn statistics reporter
            let server_stats = server.clone();
            tokio::spawn(async move {
                let mut interval = tokio::time::interval(std::time::Duration::from_secs(60));
                loop {
                    interval.tick().await;
                    let stats = server_stats.get_stats().await;
                    info!("Server stats - Active clients: {}, Address: {}",
                          stats.active_clients, stats.local_addr);
                }
            });

            // Handle shutdown signals
            let server_shutdown = server.clone();
            tokio::spawn(async move {
                match signal::ctrl_c().await {
                    Ok(()) => {
                        info!("Received Ctrl+C, shutting down gracefully...");
                        if let Err(e) = server_shutdown.shutdown().await {
                            error!("Error during shutdown: {}", e);
                        }
                        std::process::exit(0);
                    }
                    Err(err) => {
                        error!("Unable to listen for shutdown signal: {}", err);
                    }
                }
            });

            // Run custom server
            info!("🚀 Custom QUIC WebSocket server starting on {}", args.addr);
            info!("📋 Protocol: Custom QUIC WebSocket");
            info!("Press Ctrl+C to shutdown");

            if let Err(e) = server.run().await {
                error!("Custom server error: {}", e);
                return Err(e);
            }
        }
        _ => {
            error!("Invalid server mode: {}. Use 'http3' or 'custom'", args.mode);
            return Err(anyhow::anyhow!("Invalid server mode"));
        }
    }

    Ok(())
}


