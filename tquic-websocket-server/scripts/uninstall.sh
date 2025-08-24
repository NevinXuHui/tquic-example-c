#!/bin/bash

# TQUIC WebSocket Server 卸载脚本

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

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

# 检查是否以 root 权限运行
check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "此脚本需要 root 权限运行"
        log_info "请使用: sudo $0"
        exit 1
    fi
}

# 停止并禁用服务
stop_service() {
    log_info "停止并禁用服务..."
    
    if systemctl is-active --quiet tquic-websocket-server; then
        systemctl stop tquic-websocket-server
        log_info "服务已停止"
    fi
    
    if systemctl is-enabled --quiet tquic-websocket-server; then
        systemctl disable tquic-websocket-server
        log_info "服务已禁用"
    fi
}

# 删除 systemd 服务文件
remove_service() {
    log_info "删除 systemd 服务文件..."
    
    if [[ -f "/etc/systemd/system/tquic-websocket-server.service" ]]; then
        rm -f /etc/systemd/system/tquic-websocket-server.service
        systemctl daemon-reload
        log_success "systemd 服务文件已删除"
    fi
}

# 删除二进制文件
remove_binary() {
    log_info "删除二进制文件..."
    
    if [[ -f "/usr/local/bin/tquic-websocket-server" ]]; then
        rm -f /usr/local/bin/tquic-websocket-server
        log_success "二进制文件已删除"
    fi
}

# 删除配置文件和目录
remove_config() {
    log_info "删除配置文件和目录..."
    
    read -p "是否删除配置文件和证书？(y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        if [[ -d "/etc/tquic-websocket-server" ]]; then
            rm -rf /etc/tquic-websocket-server
            log_success "配置目录已删除"
        fi
    else
        log_info "保留配置文件和证书"
    fi
}

# 删除日志文件
remove_logs() {
    log_info "删除日志文件..."
    
    read -p "是否删除日志文件？(y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        if [[ -d "/var/log/tquic-websocket-server" ]]; then
            rm -rf /var/log/tquic-websocket-server
            log_success "日志目录已删除"
        fi
    else
        log_info "保留日志文件"
    fi
}

# 删除运行时目录
remove_runtime() {
    log_info "删除运行时目录..."
    
    if [[ -d "/opt/tquic-websocket-server" ]]; then
        rm -rf /opt/tquic-websocket-server
        log_success "运行时目录已删除"
    fi
}

# 删除用户
remove_user() {
    log_info "删除用户..."
    
    read -p "是否删除用户 tquic-ws？(y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        if id "tquic-ws" &>/dev/null; then
            userdel tquic-ws
            log_success "用户 tquic-ws 已删除"
        fi
    else
        log_info "保留用户 tquic-ws"
    fi
}

# 主函数
main() {
    log_info "开始卸载 TQUIC WebSocket Server..."
    
    check_root
    stop_service
    remove_service
    remove_binary
    remove_config
    remove_logs
    remove_runtime
    remove_user
    
    log_success "TQUIC WebSocket Server 卸载完成！"
}

# 运行主函数
main "$@"
