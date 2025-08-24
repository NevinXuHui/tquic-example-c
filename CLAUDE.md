# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository contains C examples demonstrating the use of [TQUIC](https://github.com/Tencent/tquic), a high-performance QUIC library, on Linux. The examples show both HTTP/0.9 and HTTP/3 implementations over QUIC.

## Build System

The project uses a Makefile-based build system:

- **Build all examples**: `make`
- **Clean build artifacts**: `make clean`

The build process automatically handles the TQUIC dependency by:
1. Initializing git submodules recursively (`git submodule update --init --recursive`)
2. Building TQUIC with FFI support (`cd deps/tquic && cargo build --release -F ffi`)

## Dependencies

- **TQUIC**: Main QUIC implementation (included as git submodule in `deps/tquic`)
- **libev**: Event loop library
- **BoringSSL**: TLS implementation (bundled with TQUIC)
- **Build requirements**: C compiler, Cargo (Rust), libev development headers

## Example Applications

### HTTP/0.9 Examples
- **simple_server**: Basic HTTP/0.9 server responding "OK" to any request
  - Usage: `./simple_server <listen_addr> <listen_port>`
  - Example: `./simple_server 0.0.0.0 4433`
  
- **simple_client**: Basic HTTP/0.9 client sending GET requests
  - Usage: `./simple_client <dest_addr> <dest_port>`
  - Example: `./simple_client 127.0.0.1 4433`

### HTTP/3 Examples
- **simple_h3_server**: HTTP/3 server with file serving capabilities
- **simple_h3_client**: HTTP/3 client for making requests

## Code Architecture

### Common Structure
All examples follow a similar pattern:
1. **Socket Creation**: Non-blocking UDP sockets with address binding/resolution
2. **QUIC Configuration**: Transport parameters, TLS config, endpoint creation
3. **Event Loop**: libev-based event handling with timers and I/O watchers
4. **Callback Methods**: Transport methods for connection/stream lifecycle events
5. **Packet Handling**: Send/receive methods for QUIC packet processing

### Key Components
- `struct simple_server/simple_client`: Main application context
- `quic_transport_methods_t`: Callbacks for connection and stream events
- `quic_packet_send_methods_t`: Packet transmission handling
- `quic_endpoint_t`: QUIC endpoint management
- Event loop integration with connection processing and timeout handling

### TLS Configuration
- Hardcoded certificate files: `cert.crt` and `cert.key` (located in project root)
- HTTP/0.9 uses basic TLS config
- HTTP/3 includes additional HTTP/3-specific configuration

## Development Notes

- Code uses AddressSanitizer (`-fsanitize=address`) for memory error detection
- Error handling follows standard C patterns with cleanup sections
- Debug logging is enabled by default at TRACE level
- All examples require certificates for TLS - ensure `cert.crt` and `cert.key` exist in project root