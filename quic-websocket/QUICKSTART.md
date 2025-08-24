# 🚀 QUIC WebSocket Server - Quick Start Guide

## 📋 Prerequisites

### Install Rust
```bash
# Install Rust using rustup
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Reload your shell environment
source ~/.cargo/env

# Verify installation
cargo --version
rustc --version
```

### System Dependencies
```bash
# Ubuntu/Debian
sudo apt update
sudo apt install build-essential pkg-config libssl-dev openssl

# CentOS/RHEL/Fedora
sudo yum groupinstall "Development Tools"
sudo yum install openssl-devel pkg-config

# macOS (with Homebrew)
brew install openssl pkg-config
```

## 🏃‍♂️ Quick Start (5 minutes)

### 1. Clone and Setup
```bash
# Navigate to the project directory
cd quic-websocket

# Make scripts executable
chmod +x run.sh generate_certs.sh
```

### 2. Generate Certificates
```bash
./generate_certs.sh
```

### 3. Build and Run Server
```bash
# Build and start the server
./run.sh native

# Or manually
cargo build --release
cargo run --bin server
```

### 4. Test the Server
Open a new terminal and run:
```bash
# Simple test client
./run.sh test

# Interactive chat client
./run.sh chat
```

## 🎮 Usage Examples

### Basic Server Start
```bash
# Default configuration (127.0.0.1:4433)
./run.sh native

# Custom configuration
cargo run --bin server -- \
    --addr 0.0.0.0:4433 \
    --name "My QUIC Server" \
    --max-clients 200 \
    --log-level debug
```

### Client Examples

#### 1. Test Client
```bash
# Basic test
./run.sh test

# Custom test
cargo run --example client -- \
    --server 127.0.0.1:4433 \
    --name "MyClient" \
    --count 10 \
    --interval 1 \
    --verbose
```

#### 2. Interactive Chat Client
```bash
# Start chat client
./run.sh chat

# Available commands in chat:
# /broadcast <message>  - Send to all users
# /list                 - List connected users
# /ping                 - Test connection
# /quit                 - Exit chat
```

#### 3. Performance Benchmark
```bash
cargo run --example benchmark -- \
    --server 127.0.0.1:4433 \
    --clients 50 \
    --messages 1000 \
    --message-size 1024 \
    --delay-ms 10
```

## 🔧 Configuration Options

### Server Configuration
| Option | Default | Description |
|--------|---------|-------------|
| `--addr` | `127.0.0.1:4433` | Server bind address |
| `--name` | `QUIC WebSocket Server` | Server name |
| `--max-clients` | `100` | Maximum concurrent clients |
| `--cert` | `certs/cert.pem` | TLS certificate file |
| `--key` | `certs/key.pem` | TLS private key file |
| `--log-level` | `info` | Log level (trace, debug, info, warn, error) |
| `--verbose` | `false` | Enable verbose logging |

### Environment Variables
```bash
export RUST_LOG=debug              # Set log level
export QUIC_WS_MAX_CLIENTS=500     # Override max clients
export QUIC_WS_BIND_ADDR=0.0.0.0:8443  # Override bind address
```

## 🐛 Troubleshooting

### Common Issues

1. **"Rust/Cargo not found"**
   ```bash
   # Install Rust
   curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
   source ~/.cargo/env
   ```

2. **"Certificate file not found"**
   ```bash
   # Generate certificates
   ./generate_certs.sh
   ```

3. **"Permission denied" on scripts**
   ```bash
   chmod +x run.sh generate_certs.sh
   ```

4. **"Address already in use"**
   ```bash
   # Use different port
   cargo run --bin server -- --addr 127.0.0.1:4434
   
   # Or kill existing process
   sudo lsof -ti:4433 | xargs kill -9
   ```

5. **Build errors**
   ```bash
   # Update Rust
   rustup update
   
   # Clean and rebuild
   cargo clean
   cargo build
   ```

### Debug Mode
```bash
# Run with debug logging
RUST_LOG=debug ./run.sh native

# Or
cargo run --bin server -- --log-level debug --verbose
```

## 📊 Performance Tips

### For High Load
```bash
# Increase connection limits
cargo run --bin server -- \
    --max-clients 1000 \
    --addr 0.0.0.0:4433

# Monitor with verbose logging
RUST_LOG=info cargo run --bin server -- --verbose
```

### For Development
```bash
# Debug mode with detailed logs
RUST_LOG=debug cargo run --bin server -- \
    --log-level debug \
    --verbose
```

## 🔗 Next Steps

1. **Read the full documentation**: [README.md](README.md)
2. **Understand the architecture**: [ARCHITECTURE.md](ARCHITECTURE.md)
3. **Explore examples**: Check the `examples/` directory
4. **Customize the server**: Modify `src/` files for your needs

## 💡 Tips

- Use `./run.sh help` to see all available commands
- The server automatically cleans up disconnected clients
- All connections are encrypted with TLS 1.3 by default
- QUIC provides better performance than TCP for real-time applications
- Each client connection uses multiplexed streams for efficiency

## 🆘 Getting Help

If you encounter issues:

1. Check the troubleshooting section above
2. Run with debug logging: `RUST_LOG=debug ./run.sh native`
3. Verify certificates exist: `ls -la certs/`
4. Check if port is available: `netstat -ln | grep 4433`
5. Ensure Rust is properly installed: `cargo --version`

Happy coding! 🎉
