#!/bin/bash

# TQUIC Examples Build Script
# Provides easy interface to CMake build system

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
BUILD_TYPE="Release"
BUILD_DIR="build"
PRESET=""
CLEAN=false
INSTALL=false
VERBOSE=false
JOBS=$(nproc 2>/dev/null || echo 4)

# Function to print colored output
print_info() {
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

# Function to show usage
show_usage() {
    cat << EOF
TQUIC Examples Build Script

Usage: $0 [OPTIONS]

OPTIONS:
    -h, --help              Show this help message
    -t, --type TYPE         Build type: Debug, Release (default: Release)
    -d, --dir DIR           Build directory (default: build)
    -p, --preset PRESET     Use CMake preset: default, debug, release, websocket-only, ninja
    -c, --clean             Clean build directory before building
    -i, --install           Install after building
    -v, --verbose           Verbose build output
    -j, --jobs N            Number of parallel jobs (default: $(nproc 2>/dev/null || echo 4))

PRESETS:
    default                 Default configuration with all examples
    debug                   Debug build with symbols
    release                 Optimized release build
    websocket-only          Build only WebSocket examples
    ninja                   Fast build using Ninja generator

EXAMPLES:
    $0                      # Build with default settings
    $0 --preset debug       # Use debug preset
    $0 --type Debug --clean # Clean debug build
    $0 --preset ninja -j 8  # Fast build with 8 jobs
    $0 --install            # Build and install

EOF
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_usage
            exit 0
            ;;
        -t|--type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        -d|--dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -p|--preset)
            PRESET="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        -i|--install)
            INSTALL=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        *)
            print_error "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# Validate build type
if [[ "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" && "$BUILD_TYPE" != "RelWithDebInfo" && "$BUILD_TYPE" != "MinSizeRel" ]]; then
    print_error "Invalid build type: $BUILD_TYPE"
    print_info "Valid types: Debug, Release, RelWithDebInfo, MinSizeRel"
    exit 1
fi

# Set build directory based on preset if not specified
if [[ -n "$PRESET" && "$BUILD_DIR" == "build" ]]; then
    case "$PRESET" in
        debug)
            BUILD_DIR="build-debug"
            ;;
        release)
            BUILD_DIR="build-release"
            ;;
        websocket-only)
            BUILD_DIR="build-websocket"
            ;;
        ninja)
            BUILD_DIR="build-ninja"
            ;;
    esac
fi

print_info "TQUIC Examples Build Configuration:"
print_info "  Build Type: $BUILD_TYPE"
print_info "  Build Directory: $BUILD_DIR"
print_info "  Preset: ${PRESET:-none}"
print_info "  Jobs: $JOBS"
print_info "  Clean: $CLEAN"
print_info "  Install: $INSTALL"
print_info "  Verbose: $VERBOSE"

# Clean build directory if requested
if [[ "$CLEAN" == true ]]; then
    print_info "Cleaning build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Configure with CMake
print_info "Configuring with CMake..."
if [[ -n "$PRESET" ]]; then
    cmake --preset "$PRESET"
else
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

# Build
print_info "Building..."
CMAKE_BUILD_ARGS=("--build" "$BUILD_DIR" "--parallel" "$JOBS")

if [[ "$VERBOSE" == true ]]; then
    CMAKE_BUILD_ARGS+=("--verbose")
fi

cmake "${CMAKE_BUILD_ARGS[@]}"

print_success "Build completed successfully!"

# Install if requested
if [[ "$INSTALL" == true ]]; then
    print_info "Installing..."
    cmake --install "$BUILD_DIR"
    print_success "Installation completed!"
fi

# Show build results
print_info "Build artifacts:"
if [[ -d "$BUILD_DIR/bin" ]]; then
    ls -la "$BUILD_DIR/bin/"
else
    ls -la "$BUILD_DIR/"*websocket* "$BUILD_DIR/"simple_* 2>/dev/null || true
fi

print_success "All done! 🎉"
print_info "To run WebSocket examples:"
print_info "  Server: ./$BUILD_DIR/bin/tquic_websocket_server 127.0.0.1 4433"
print_info "  Client: ./$BUILD_DIR/bin/tquic_websocket_client 127.0.0.1 4433"
