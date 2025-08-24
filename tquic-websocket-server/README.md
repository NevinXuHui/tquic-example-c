# TQUIC WebSocket Server

基于 TQUIC (Tencent QUIC) 的高性能 WebSocket 服务器，支持 HTTP/3 和现代 QUIC 协议。

## 🚀 特性

- **QUIC 协议支持** - 基于 TQUIC 的高性能 QUIC 实现
- **HTTP/3 WebSocket** - 支持 WebSocket over HTTP/3
- **高并发** - 事件驱动的异步 I/O 模型
- **JSON 消息** - 内置 JSON 消息处理和路由
- **TLS 1.3** - 现代加密和安全传输
- **systemd 集成** - 完整的系统服务支持
- **配置灵活** - 支持配置文件和命令行参数

## 📋 系统要求

### 依赖库
```bash
# Ubuntu/Debian
sudo apt install build-essential cmake pkg-config
sudo apt install libev-dev libcjson-dev libssl-dev

# CentOS/RHEL
sudo yum groupinstall "Development Tools"
sudo yum install cmake libev-devel libcjson-devel openssl-devel
```

### TQUIC 库
确保 TQUIC 库已构建：
```bash
cd ../deps/tquic
cargo build --release -F ffi
```

## 🔧 构建和安装

### 快速构建
```bash
# 使用构建脚本
./build.sh

# 或手动构建
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 构建选项
```bash
# Debug 构建
./build.sh --build-type Debug --verbose

# 清理构建
./build.sh --clean

# 构建并安装
./build.sh --install
```

## 📦 系统服务安装

### 安装为 systemd 服务
```bash
# 运行安装脚本（需要 root 权限）
sudo ./scripts/install.sh
```

安装脚本会自动：
- 创建系统用户 `tquic-ws`
- 创建必要的目录和权限
- 安装二进制文件到 `/usr/local/bin/`
- 安装配置文件到 `/etc/tquic-websocket-server/`
- 生成自签名 TLS 证书
- 安装并启动 systemd 服务

### 服务管理
```bash
# 启动服务
sudo systemctl start tquic-websocket-server

# 停止服务
sudo systemctl stop tquic-websocket-server

# 重启服务
sudo systemctl restart tquic-websocket-server

# 查看状态
sudo systemctl status tquic-websocket-server

# 查看日志
sudo journalctl -u tquic-websocket-server -f

# 开机自启
sudo systemctl enable tquic-websocket-server
```

## ⚙️ 配置

### 配置文件位置
- 主配置: `/etc/tquic-websocket-server/server.conf`
- TLS 证书: `/etc/tquic-websocket-server/cert.pem`
- TLS 私钥: `/etc/tquic-websocket-server/key.pem`
- 日志目录: `/var/log/tquic-websocket-server/`

### 配置示例
```ini
# 服务器监听配置
listen_host=0.0.0.0
listen_port=4433

# TLS 证书配置
cert_file=/etc/tquic-websocket-server/cert.pem
key_file=/etc/tquic-websocket-server/key.pem

# 日志配置
log_level=info
log_file=/var/log/tquic-websocket-server/server.log

# 性能配置
max_connections=1000
worker_threads=4
```

## 🔒 安全配置

### TLS 证书
安装脚本会自动生成自签名证书，生产环境建议使用正式证书：

```bash
# 替换为你的证书
sudo cp your-cert.pem /etc/tquic-websocket-server/cert.pem
sudo cp your-key.pem /etc/tquic-websocket-server/key.pem
sudo chown root:tquic-ws /etc/tquic-websocket-server/key.pem
sudo chmod 640 /etc/tquic-websocket-server/key.pem
```

### 防火墙配置
```bash
# 开放端口 4433
sudo ufw allow 4433/udp
# 或使用 firewalld
sudo firewall-cmd --permanent --add-port=4433/udp
sudo firewall-cmd --reload
```

## 📊 监控和日志

### 日志文件
- 服务日志: `/var/log/tquic-websocket-server/server.log`
- 访问日志: `/var/log/tquic-websocket-server/access.log`
- 系统日志: `journalctl -u tquic-websocket-server`

### 性能监控
```bash
# 查看连接数
ss -tuln | grep :4433

# 查看进程状态
ps aux | grep tquic-websocket-server

# 查看资源使用
top -p $(pgrep tquic-websocket-server)
```

## 🧪 测试

### 基本连接测试
```bash
# 使用分层客户端测试
cd ../websocket-layered-client
./build/bin/chat_client 127.0.0.1 4433 testuser
```

### 性能测试
```bash
# 使用多个客户端连接
for i in {1..10}; do
    ./build/bin/json_client 127.0.0.1 4433 user$i &
done
```

## 🔧 故障排除

### 常见问题

1. **服务启动失败**
   ```bash
   # 检查日志
   sudo journalctl -u tquic-websocket-server -n 50
   
   # 检查配置文件
   sudo tquic-websocket-server --check-config
   ```

2. **端口被占用**
   ```bash
   # 查看端口使用
   sudo netstat -tulpn | grep :4433
   ```

3. **证书问题**
   ```bash
   # 检查证书有效性
   openssl x509 -in /etc/tquic-websocket-server/cert.pem -text -noout
   ```

## 🗑️ 卸载

```bash
# 运行卸载脚本
sudo ./scripts/uninstall.sh
```

卸载脚本会询问是否删除：
- 配置文件和证书
- 日志文件
- 系统用户

## 📄 许可证

Apache License 2.0

---

**🎉 享受高性能的 QUIC WebSocket 服务！**
