//! # QUIC WebSocket
//! 
//! A WebSocket-like server implementation built on top of the QUIC protocol.
//! 
//! This library provides a high-performance, multiplexed communication server
//! that combines the benefits of QUIC (0-RTT connection establishment, 
//! connection migration, built-in encryption) with WebSocket-like messaging patterns.
//! 
//! ## Features
//! 
//! - **High Performance**: Built on QUIC protocol with multiplexed streams
//! - **Real-time Communication**: WebSocket-like messaging patterns
//! - **Connection Management**: Automatic client lifecycle management
//! - **Message Broadcasting**: Support for broadcasting messages to all clients
//! - **Direct Messaging**: Private messaging between clients
//! - **Heartbeat Support**: Built-in ping/pong mechanism
//! - **Graceful Shutdown**: Clean server and client disconnection
//! 
//! ## Quick Start
//! 
//! ```rust,no_run
//! use quic_websocket::{QuicWebSocketServer, create_server_config};
//! use quinn::Endpoint;
//! use std::net::SocketAddr;
//! 
//! #[tokio::main]
//! async fn main() -> anyhow::Result<()> {
//!     // Create server configuration
//!     let server_config = create_server_config("cert.pem", "key.pem")?;
//!     
//!     // Create endpoint
//!     let addr: SocketAddr = "127.0.0.1:4433".parse()?;
//!     let endpoint = Endpoint::server(server_config, addr)?;
//!     
//!     // Create and run server
//!     let (server, _broadcast_rx) = QuicWebSocketServer::new(
//!         endpoint,
//!         "My QUIC WebSocket Server".to_string(),
//!         100, // max clients
//!     );
//!     
//!     server.run().await?;
//!     Ok(())
//! }
//! ```
//! 
//! ## Message Types
//! 
//! The server supports various message types:
//! 
//! - **Handshake**: Initial connection establishment
//! - **Text/Binary**: Content messages
//! - **Broadcast**: Messages sent to all connected clients
//! - **DirectMessage**: Private messages between specific clients
//! - **Ping/Pong**: Heartbeat mechanism
//! - **ClientList**: Request/response for connected clients
//! - **Close**: Graceful connection termination
//! 
//! ## Architecture
//! 
//! The server is built with the following components:
//! 
//! - **Server**: Main QUIC server handling connections
//! - **ClientManager**: Manages client connections and state
//! - **MessageHandler**: Processes different message types
//! - **MessageFrame**: Serializable message format
//! 
//! Each client connection uses QUIC streams for message transmission,
//! providing natural multiplexing and flow control.

pub mod client;
pub mod handler;
pub mod message;
pub mod server;
pub mod websocket;
pub mod h3_server;

// Re-export main types for convenience
pub use client::{ClientConnection, ClientManager, ClientState};
pub use handler::MessageHandler;
pub use message::{ClientId, ClientInfo, MessageFrame, MessageType};
pub use server::{QuicWebSocketServer, ServerStats, create_server_config};

// Re-export commonly used external types
pub use quinn::{Endpoint, ServerConfig, Connection};
pub use anyhow::{Result, Error};

/// Library version
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// Default protocol version
pub const PROTOCOL_VERSION: &str = "1.0";

/// Default maximum message size (1MB)
pub const DEFAULT_MAX_MESSAGE_SIZE: usize = 1024 * 1024;

/// Default maximum clients
pub const DEFAULT_MAX_CLIENTS: usize = 1000;

/// Default heartbeat interval (30 seconds)
pub const DEFAULT_HEARTBEAT_INTERVAL: std::time::Duration = std::time::Duration::from_secs(30);

/// Default connection timeout (5 minutes)
pub const DEFAULT_CONNECTION_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(300);

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::{MessageFrame, MessageType};

    #[test]
    fn test_message_frame_creation() {
        let frame = MessageFrame::new(MessageType::Text {
            content: "Hello, World!".to_string(),
            timestamp: 1234567890,
        });

        assert_eq!(frame.priority, 128);
        assert!(!frame.require_ack);
    }

    #[test]
    fn test_message_frame_with_ack() {
        let frame = MessageFrame::new_with_ack(MessageType::Ping {
            timestamp: 1234567890,
        });

        assert!(frame.require_ack);
    }

    #[test]
    fn test_message_frame_priority() {
        let frame = MessageFrame::new(MessageType::Text {
            content: "High priority".to_string(),
            timestamp: 1234567890,
        }).with_priority(255);

        assert_eq!(frame.priority, 255);
    }

    #[tokio::test]
    async fn test_client_manager_creation() {
        let (manager, _rx) = ClientManager::new(10);
        assert_eq!(manager.client_count().await, 0);
    }
}
