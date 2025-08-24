#!/bin/bash

# QUIC WebSocket Server Runner Script
# This script handles certificate generation and server startup

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if certificates exist
check_certificates() {
    if [[ ! -f "certs/cert.pem" || ! -f "certs/key.pem" ]]; then
        print_warning "Certificates not found. Generating new ones..."
        ./generate_certs.sh
        if [[ $? -eq 0 ]]; then
            print_success "Certificates generated successfully"
        else
            print_error "Failed to generate certificates"
            exit 1
        fi
    else
        print_status "Certificates found"
    fi
}

# Check if Rust is installed
check_rust() {
    if ! command -v cargo &> /dev/null; then
        print_error "Rust/Cargo not found. Please install Rust or use Docker."
        print_status "Install Rust: https://rustup.rs/"
        print_status "Or run with Docker: ./run.sh docker"
        exit 1
    fi
}

# Build the project
build_project() {
    print_status "Building QUIC WebSocket Server..."
    cargo build --release
    if [[ $? -eq 0 ]]; then
        print_success "Build completed successfully"
    else
        print_error "Build failed"
        exit 1
    fi
}

# Run with native Rust
run_native() {
    check_rust
    check_certificates
    build_project
    
    print_status "Starting QUIC WebSocket Server..."
    print_status "Server will be available at: 127.0.0.1:4433"
    print_status "Press Ctrl+C to stop the server"
    echo ""
    
    cargo run --bin server -- \
        --addr 127.0.0.1:4433 \
        --name "QUIC WebSocket Server" \
        --max-clients 100 \
        --cert certs/cert.pem \
        --key certs/key.pem \
        --log-level info
}



# Run test client
run_test_client() {
    if ! command -v cargo &> /dev/null; then
        print_error "Rust/Cargo not found. Cannot run test client."
        exit 1
    fi
    
    print_status "Starting test client..."
    cargo run --example client -- \
        --server 127.0.0.1:4433 \
        --name "TestClient" \
        --count 3 \
        --interval 2 \
        --verbose
}

# Run chat client
run_chat_client() {
    if ! command -v cargo &> /dev/null; then
        print_error "Rust/Cargo not found. Cannot run chat client."
        exit 1
    fi

    print_status "Starting interactive chat client..."
    cargo run --example chat_client -- \
        --server 127.0.0.1:4433 \
        --name "ChatUser" \
        --verbose
}

# Run WebSocket client with subscription support
run_websocket_client() {
    if ! command -v cargo &> /dev/null; then
        print_error "Rust/Cargo not found. Cannot run WebSocket client."
        exit 1
    fi

    print_status "Starting enhanced WebSocket client with subscription support..."
    cargo run --example websocket_client -- \
        --server 127.0.0.1:4433 \
        --name "WebSocketUser" \
        --verbose
}

# Show help
show_help() {
    echo "QUIC WebSocket Server Runner"
    echo ""
    echo "Usage: $0 [COMMAND]"
    echo ""
    echo "Commands:"
    echo "  native      Run with native Rust (default)"
    echo "  test        Run test client"
    echo "  chat        Run interactive chat client"
    echo "  websocket   Run enhanced WebSocket client with subscriptions"
    echo "  certs       Generate certificates only"
    echo "  clean       Clean build artifacts"
    echo "  help        Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                # Run with native Rust"
    echo "  $0 test           # Run test client"
    echo "  $0 chat           # Run chat client"
    echo "  $0 websocket      # Run WebSocket client with push notifications"
}

# Clean build artifacts
clean() {
    print_status "Cleaning build artifacts..."
    if command -v cargo &> /dev/null; then
        cargo clean
    fi
    print_success "Clean completed"
}

# Main script logic
case "${1:-native}" in
    "native"|"")
        run_native
        ;;
    "test")
        run_test_client
        ;;
    "chat")
        run_chat_client
        ;;
    "websocket")
        run_websocket_client
        ;;
    "certs")
        ./generate_certs.sh
        ;;
    "clean")
        clean
        ;;
    "help"|"-h"|"--help")
        show_help
        ;;
    *)
        print_error "Unknown command: $1"
        show_help
        exit 1
        ;;
esac
