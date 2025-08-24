#!/bin/bash

# TQUIC WebSocket Server 测试脚本

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

# 测试服务状态
test_service_status() {
    log_info "测试服务状态..."
    
    if systemctl is-active --quiet tquic-websocket-server; then
        log_success "服务正在运行"
    else
        log_error "服务未运行"
        return 1
    fi
    
    if systemctl is-enabled --quiet tquic-websocket-server; then
        log_success "服务已启用自启动"
    else
        log_warning "服务未启用自启动"
    fi
}

# 测试端口监听
test_port_listening() {
    log_info "测试端口监听..."
    
    if netstat -tulpn | grep -q ":4433"; then
        log_success "端口 4433 正在监听"
        netstat -tulpn | grep ":4433"
    else
        log_error "端口 4433 未监听"
        return 1
    fi
}

# 测试配置文件
test_config_files() {
    log_info "测试配置文件..."
    
    if [[ -f "/etc/tquic-websocket-server/server.conf" ]]; then
        log_success "配置文件存在"
    else
        log_error "配置文件不存在"
        return 1
    fi
    
    if [[ -f "/etc/tquic-websocket-server/cert.pem" ]]; then
        log_success "证书文件存在"
    else
        log_error "证书文件不存在"
        return 1
    fi
    
    if [[ -f "/etc/tquic-websocket-server/key.pem" ]]; then
        log_success "私钥文件存在"
    else
        log_error "私钥文件不存在"
        return 1
    fi
}

# 测试日志目录
test_log_directory() {
    log_info "测试日志目录..."
    
    if [[ -d "/var/log/tquic-websocket-server" ]]; then
        log_success "日志目录存在"
        ls -la /var/log/tquic-websocket-server/
    else
        log_error "日志目录不存在"
        return 1
    fi
}

# 测试用户和权限
test_user_permissions() {
    log_info "测试用户和权限..."
    
    if id "tquic-ws" &>/dev/null; then
        log_success "用户 tquic-ws 存在"
    else
        log_error "用户 tquic-ws 不存在"
        return 1
    fi
    
    # 检查日志目录权限
    if [[ -O "/var/log/tquic-websocket-server" ]] || [[ $(stat -c %U /var/log/tquic-websocket-server) == "tquic-ws" ]]; then
        log_success "日志目录权限正确"
    else
        log_warning "日志目录权限可能不正确"
    fi
}

# 测试客户端连接（如果可用）
test_client_connection() {
    log_info "测试客户端连接..."
    
    CLIENT_PATH="../websocket-layered-client/build/bin/json_client"
    if [[ -f "$CLIENT_PATH" ]]; then
        log_info "找到测试客户端，尝试连接..."
        timeout 10s $CLIENT_PATH 127.0.0.1 4433 test_user &
        CLIENT_PID=$!
        sleep 3
        
        if kill -0 $CLIENT_PID 2>/dev/null; then
            log_success "客户端连接成功"
            kill $CLIENT_PID 2>/dev/null || true
        else
            log_warning "客户端连接测试失败"
        fi
    else
        log_info "测试客户端不可用，跳过连接测试"
    fi
}

# 显示服务信息
show_service_info() {
    log_info "服务信息:"
    echo
    systemctl status tquic-websocket-server --no-pager
    echo
    log_info "最近日志:"
    journalctl -u tquic-websocket-server -n 10 --no-pager
}

# 主函数
main() {
    log_info "开始测试 TQUIC WebSocket Server 安装..."
    echo
    
    test_service_status
    echo
    test_port_listening
    echo
    test_config_files
    echo
    test_log_directory
    echo
    test_user_permissions
    echo
    test_client_connection
    echo
    show_service_info
    
    log_success "测试完成！"
}

# 运行主函数
main "$@"
