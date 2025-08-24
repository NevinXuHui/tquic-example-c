use anyhow::{Context, Result};
use clap::Parser;
use quinn::Endpoint;
use quic_websocket::{QuicWebSocketServer, create_server_config};
use std::net::SocketAddr;
use std::path::PathBuf;
use tokio::signal;
use tracing::{info, error, Level};
use tracing_subscriber;

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

    // Create server
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

    // Run server
    info!("Server starting on {}", args.addr);
    info!("Press Ctrl+C to shutdown");
    
    if let Err(e) = server.run().await {
        error!("Server error: {}", e);
        return Err(e);
    }

    Ok(())
}


