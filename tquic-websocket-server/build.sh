#!/bin/bash

# TQUIC WebSocket Server 构建脚本

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 默认配置
BUILD_TYPE="Release"
BUILD_DIR="build"
CLEAN_BUILD=false
INSTALL=false
VERBOSE=false

# 日志函数
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 显示帮助信息
show_help() {
    echo "TQUIC WebSocket Server 构建脚本"
    echo
    echo "用法: $0 [选项]"
    echo
    echo "选项:"
    echo "  -t, --build-type TYPE    构建类型 (Debug|Release) [默认: Release]"
    echo "  -d, --build-dir DIR      构建目录 [默认: build]"
    echo "  -c, --clean              清理构建目录"
    echo "  -i, --install            安装到系统"
    echo "  -v, --verbose            详细输出"
    echo "  -h, --help               显示此帮助信息"
    echo
    echo "示例:"
    echo "  $0                       # 默认构建"
    echo "  $0 -t Debug -v           # Debug 构建，详细输出"
    echo "  $0 -c -i                 # 清理构建并安装"
}

# 解析命令行参数
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -t|--build-type)
                BUILD_TYPE="$2"
                shift 2
                ;;
            -d|--build-dir)
                BUILD_DIR="$2"
                shift 2
                ;;
            -c|--clean)
                CLEAN_BUILD=true
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
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                log_error "未知选项: $1"
                show_help
                exit 1
                ;;
        esac
    done
}

# 检查依赖
check_dependencies() {
    log_info "检查依赖..."
    
    # 检查 CMake
    if ! command -v cmake &> /dev/null; then
        log_error "CMake 未安装"
        exit 1
    fi
    
    # 检查编译器
    if ! command -v gcc &> /dev/null && ! command -v clang &> /dev/null; then
        log_error "未找到 C 编译器 (gcc 或 clang)"
        exit 1
    fi
    
    # 检查 pkg-config
    if ! command -v pkg-config &> /dev/null; then
        log_warning "pkg-config 未安装，可能影响依赖库检测"
    fi
    
    # 检查 TQUIC 库
    TQUIC_LIB="../deps/tquic/target/release/libtquic.a"
    if [[ ! -f "$TQUIC_LIB" ]]; then
        log_error "TQUIC 库未找到: $TQUIC_LIB"
        log_info "请先构建 TQUIC 库:"
        log_info "  cd ../deps/tquic"
        log_info "  cargo build --release -F ffi"
        exit 1
    fi
    
    log_success "依赖检查通过"
}

# 清理构建目录
clean_build() {
    if [[ "$CLEAN_BUILD" == true ]]; then
        log_info "清理构建目录..."
        rm -rf "$BUILD_DIR"
        log_success "构建目录已清理"
    fi
}

# 配置构建
configure_build() {
    log_info "配置构建..."
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    CMAKE_ARGS="-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    
    if [[ "$VERBOSE" == true ]]; then
        CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_VERBOSE_MAKEFILE=ON"
    fi
    
    cmake .. $CMAKE_ARGS
    
    log_success "构建配置完成"
}

# 执行构建
build_project() {
    log_info "开始构建..."
    
    MAKE_ARGS="-j$(nproc)"
    
    if [[ "$VERBOSE" == true ]]; then
        MAKE_ARGS="$MAKE_ARGS VERBOSE=1"
    fi
    
    make $MAKE_ARGS
    
    log_success "构建完成"
}

# 安装项目
install_project() {
    if [[ "$INSTALL" == true ]]; then
        log_info "安装项目..."
        
        if [[ $EUID -ne 0 ]]; then
            log_warning "需要 root 权限进行安装"
            sudo make install
        else
            make install
        fi
        
        log_success "安装完成"
    fi
}

# 显示构建信息
show_build_info() {
    log_info "构建信息:"
    echo "  构建类型: $BUILD_TYPE"
    echo "  构建目录: $BUILD_DIR"
    echo "  二进制文件: $BUILD_DIR/tquic-websocket-server"
    
    if [[ -f "$BUILD_DIR/tquic-websocket-server" ]]; then
        echo "  文件大小: $(du -h "$BUILD_DIR/tquic-websocket-server" | cut -f1)"
    fi
    
    echo
    log_info "运行服务器:"
    echo "  ./$BUILD_DIR/tquic-websocket-server 127.0.0.1 4433"
    echo
    log_info "安装为系统服务:"
    echo "  sudo ./scripts/install.sh"
}

# 主函数
main() {
    log_info "TQUIC WebSocket Server 构建脚本"
    
    parse_args "$@"
    check_dependencies
    clean_build
    configure_build
    build_project
    install_project
    
    cd ..
    show_build_info
    
    log_success "所有操作完成！"
}

# 运行主函数
main "$@"
