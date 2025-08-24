#!/bin/bash

# TQUIC WebSocket Server 安装脚本
# 用于将服务器安装为 systemd 服务

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

# 检查系统是否支持 systemd
check_systemd() {
    if ! command -v systemctl &> /dev/null; then
        log_error "系统不支持 systemd"
        exit 1
    fi
    log_info "检测到 systemd 支持"
}

# 创建用户和组
create_user() {
    if ! id "tquic-ws" &>/dev/null; then
        log_info "创建用户 tquic-ws..."
        useradd --system --no-create-home --shell /bin/false tquic-ws
        log_success "用户 tquic-ws 创建成功"
    else
        log_info "用户 tquic-ws 已存在"
    fi
}

# 创建目录
create_directories() {
    log_info "创建必要的目录..."
    
    # 配置目录
    mkdir -p /etc/tquic-websocket-server
    chown root:root /etc/tquic-websocket-server
    chmod 755 /etc/tquic-websocket-server
    
    # 日志目录
    mkdir -p /var/log/tquic-websocket-server
    chown tquic-ws:tquic-ws /var/log/tquic-websocket-server
    chmod 755 /var/log/tquic-websocket-server
    
    # 运行时目录
    mkdir -p /opt/tquic-websocket-server
    chown tquic-ws:tquic-ws /opt/tquic-websocket-server
    chmod 755 /opt/tquic-websocket-server
    
    log_success "目录创建完成"
}

# 安装二进制文件
install_binary() {
    log_info "安装二进制文件..."
    
    if [[ ! -f "build/tquic-websocket-server" ]]; then
        log_error "二进制文件不存在，请先编译项目"
        log_info "运行: mkdir build && cd build && cmake .. && make"
        exit 1
    fi
    
    cp build/tquic-websocket-server /usr/local/bin/
    chown root:root /usr/local/bin/tquic-websocket-server
    chmod 755 /usr/local/bin/tquic-websocket-server
    
    log_success "二进制文件安装完成"
}

# 安装配置文件
install_config() {
    log_info "安装配置文件..."
    
    if [[ -f "/etc/tquic-websocket-server/server.conf" ]]; then
        log_warning "配置文件已存在，备份为 server.conf.backup"
        cp /etc/tquic-websocket-server/server.conf /etc/tquic-websocket-server/server.conf.backup
    fi
    
    cp config/server.conf /etc/tquic-websocket-server/
    chown root:root /etc/tquic-websocket-server/server.conf
    chmod 644 /etc/tquic-websocket-server/server.conf
    
    log_success "配置文件安装完成"
}

# 生成自签名证书
generate_cert() {
    log_info "生成自签名 TLS 证书..."
    
    if [[ -f "/etc/tquic-websocket-server/cert.pem" ]]; then
        log_warning "证书文件已存在，跳过生成"
        return
    fi
    
    openssl req -x509 -newkey rsa:4096 -keyout /etc/tquic-websocket-server/key.pem \
        -out /etc/tquic-websocket-server/cert.pem -days 365 -nodes \
        -subj "/C=CN/ST=Beijing/L=Beijing/O=TQUIC/OU=WebSocket/CN=localhost"
    
    chown root:tquic-ws /etc/tquic-websocket-server/key.pem
    chown root:root /etc/tquic-websocket-server/cert.pem
    chmod 640 /etc/tquic-websocket-server/key.pem
    chmod 644 /etc/tquic-websocket-server/cert.pem
    
    log_success "TLS 证书生成完成"
}

# 安装 systemd 服务
install_service() {
    log_info "安装 systemd 服务..."
    
    cp scripts/tquic-websocket-server.service /etc/systemd/system/
    chown root:root /etc/systemd/system/tquic-websocket-server.service
    chmod 644 /etc/systemd/system/tquic-websocket-server.service
    
    systemctl daemon-reload
    
    log_success "systemd 服务安装完成"
}

# 启用并启动服务
enable_service() {
    log_info "启用并启动服务..."
    
    systemctl enable tquic-websocket-server
    systemctl start tquic-websocket-server
    
    # 等待服务启动
    sleep 2
    
    if systemctl is-active --quiet tquic-websocket-server; then
        log_success "服务启动成功"
    else
        log_error "服务启动失败"
        log_info "查看日志: journalctl -u tquic-websocket-server -f"
        exit 1
    fi
}

# 显示状态信息
show_status() {
    log_info "服务状态信息:"
    echo
    systemctl status tquic-websocket-server --no-pager
    echo
    log_info "常用命令:"
    echo "  启动服务: sudo systemctl start tquic-websocket-server"
    echo "  停止服务: sudo systemctl stop tquic-websocket-server"
    echo "  重启服务: sudo systemctl restart tquic-websocket-server"
    echo "  查看状态: sudo systemctl status tquic-websocket-server"
    echo "  查看日志: sudo journalctl -u tquic-websocket-server -f"
    echo "  编辑配置: sudo nano /etc/tquic-websocket-server/server.conf"
    echo
    log_success "TQUIC WebSocket Server 安装完成！"
}

# 主函数
main() {
    log_info "开始安装 TQUIC WebSocket Server..."
    
    check_root
    check_systemd
    create_user
    create_directories
    install_binary
    install_config
    generate_cert
    install_service
    enable_service
    show_status
}

# 运行主函数
main "$@"
