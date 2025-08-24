#!/bin/bash

# 🚀 TQUIC WebSocket 完整功能演示脚本
# 展示从"偷懒实现"到"标准实现"的优化成果

set -e

echo "🚀 TQUIC WebSocket over HTTP/3 完整演示"
echo "========================================"
echo ""

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# 检查依赖
check_dependencies() {
    echo -e "${BLUE}🔍 检查系统依赖...${NC}"
    
    # 检查编译工具
    if ! command -v gcc &> /dev/null; then
        echo -e "${RED}❌ GCC 未安装${NC}"
        exit 1
    fi
    
    # 检查 OpenSSL
    if ! command -v openssl &> /dev/null; then
        echo -e "${RED}❌ OpenSSL 未安装${NC}"
        exit 1
    fi
    
    # 检查 libev (通过头文件检查)
    if ! echo '#include <ev.h>' | gcc -E - &>/dev/null; then
        echo -e "${RED}❌ libev 未安装${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}✅ 所有依赖已满足${NC}"
    echo ""
}

# 编译项目
build_project() {
    echo -e "${BLUE}🔨 编译项目...${NC}"
    
    if make clean && make; then
        echo -e "${GREEN}✅ 编译成功${NC}"
    else
        echo -e "${RED}❌ 编译失败${NC}"
        exit 1
    fi
    echo ""
}

# 生成证书
generate_certificates() {
    echo -e "${BLUE}🔐 生成测试证书...${NC}"
    
    if [ ! -f "cert.crt" ] || [ ! -f "cert.key" ]; then
        openssl req -x509 -newkey rsa:2048 -keyout cert.key -out cert.crt \
                   -days 365 -nodes -subj "/CN=localhost" &> /dev/null
        echo -e "${GREEN}✅ 证书生成成功${NC}"
    else
        echo -e "${YELLOW}⚠️  证书已存在，跳过生成${NC}"
    fi
    echo ""
}

# 展示优化成果
show_optimization() {
    echo -e "${PURPLE}🎯 优化成果展示${NC}"
    echo "================================"
    echo ""
    
    echo -e "${YELLOW}❌ 优化前的问题：${NC}"
    echo "  • 假装的 WebSocket 升级检查"
    echo "  • 硬编码的假密钥"
    echo "  • 简化的哈希实现（全零）"
    echo "  • 缺少真正的协议解析"
    echo ""
    
    echo -e "${GREEN}✅ 优化后的实现：${NC}"
    echo "  • 真正的 HTTP/3 头部解析"
    echo "  • 标准的 SHA-1 + Base64 密钥生成"
    echo "  • 完整的 WebSocket 协议支持"
    echo "  • 详细的调试和错误信息"
    echo "  • 符合 RFC 9220 和 RFC 6455 标准"
    echo ""
}

# 演示基础功能
demo_basic() {
    echo -e "${CYAN}📋 演示 1: 基础 QUIC 通信${NC}"
    echo "================================"
    echo ""
    
    echo "启动 simple_server..."
    ./simple_server 127.0.0.1 4433 &
    SERVER_PID=$!
    sleep 2
    
    echo "运行 simple_client..."
    timeout 5 ./simple_client 127.0.0.1 4433 || true
    
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    echo ""
}

# 演示 HTTP/3 功能
demo_http3() {
    echo -e "${CYAN}📋 演示 2: HTTP/3 通信${NC}"
    echo "================================"
    echo ""
    
    echo "启动 simple_h3_server..."
    ./simple_h3_server 127.0.0.1 4433 &
    H3_SERVER_PID=$!
    sleep 2
    
    echo "运行 simple_h3_client..."
    timeout 5 ./simple_h3_client 127.0.0.1 4433 || true
    
    kill $H3_SERVER_PID 2>/dev/null || true
    wait $H3_SERVER_PID 2>/dev/null || true
    echo ""
}

# 演示 WebSocket 自动化测试
demo_websocket_auto() {
    echo -e "${CYAN}📋 演示 3: WebSocket 自动化测试${NC}"
    echo "================================"
    echo ""
    
    echo "启动优化后的 WebSocket 服务器..."
    ./tquic_websocket_server 127.0.0.1 4433 &
    WS_SERVER_PID=$!
    sleep 2
    
    echo "运行自动化测试客户端..."
    timeout 10 ./tquic_websocket_client 127.0.0.1 4433 || true
    
    kill $WS_SERVER_PID 2>/dev/null || true
    wait $WS_SERVER_PID 2>/dev/null || true
    echo ""
}

# 演示交互式 WebSocket
demo_websocket_interactive() {
    echo -e "${CYAN}📋 演示 4: 交互式 WebSocket 聊天${NC}"
    echo "================================"
    echo ""
    
    echo "启动 WebSocket 服务器..."
    ./tquic_websocket_server 127.0.0.1 4433 &
    WS_SERVER_PID=$!
    sleep 2
    
    echo "启动交互式客户端（5秒后自动退出）..."
    (
        sleep 1
        echo "Hello from demo script!"
        sleep 1
        echo "Testing optimized WebSocket!"
        sleep 1
        echo "All features working perfectly!"
        sleep 1
        echo "quit"
    ) | timeout 10 ./tquic_websocket_interactive_client 127.0.0.1 4433 || true
    
    kill $WS_SERVER_PID 2>/dev/null || true
    wait $WS_SERVER_PID 2>/dev/null || true
    echo ""
}

# 显示技术特性
show_features() {
    echo -e "${PURPLE}🌟 技术特性总结${NC}"
    echo "================================"
    echo ""
    
    echo -e "${GREEN}✅ 协议标准支持：${NC}"
    echo "  • RFC 9220 - WebSocket over HTTP/3"
    echo "  • RFC 6455 - WebSocket Protocol"
    echo "  • RFC 9114 - HTTP/3"
    echo "  • RFC 9000 - QUIC Transport"
    echo ""
    
    echo -e "${GREEN}✅ 安全特性：${NC}"
    echo "  • TLS 1.3 端到端加密"
    echo "  • 标准 WebSocket 密钥生成"
    echo "  • 证书验证支持"
    echo ""
    
    echo -e "${GREEN}✅ 性能优化：${NC}"
    echo "  • 异步 I/O 事件处理"
    echo "  • QUIC 多路复用"
    echo "  • 0-RTT 连接建立"
    echo "  • 高效内存管理"
    echo ""
}

# 显示使用指南
show_usage() {
    echo -e "${BLUE}📖 使用指南${NC}"
    echo "================================"
    echo ""
    
    echo -e "${YELLOW}🚀 快速开始：${NC}"
    echo "  1. 启动服务器: ./tquic_websocket_server 127.0.0.1 4433"
    echo "  2. 自动测试:   ./tquic_websocket_client 127.0.0.1 4433"
    echo "  3. 交互聊天:   ./tquic_websocket_interactive_client 127.0.0.1 4433"
    echo ""
    
    echo -e "${YELLOW}🔧 高级用法：${NC}"
    echo "  • 启用调试:    RUST_LOG=debug ./tquic_websocket_server 127.0.0.1 4433"
    echo "  • 网络抓包:    sudo tcpdump -i lo -n port 4433"
    echo "  • 内存检查:    valgrind ./tquic_websocket_server 127.0.0.1 4433"
    echo ""
}

# 主函数
main() {
    echo -e "${GREEN}开始完整演示...${NC}"
    echo ""
    
    check_dependencies
    build_project
    generate_certificates
    show_optimization
    
    demo_basic
    demo_http3
    demo_websocket_auto
    demo_websocket_interactive
    
    show_features
    show_usage
    
    echo -e "${GREEN}🎉 演示完成！${NC}"
    echo ""
    echo -e "${CYAN}💡 提示：${NC}"
    echo "  • 查看详细文档: cat README.md"
    echo "  • 运行交互式客户端体验完整功能"
    echo "  • 所有源代码都有详细注释"
    echo ""
    echo -e "${PURPLE}🚀 享受基于 QUIC 的下一代 WebSocket 体验！${NC}"
}

# 运行主函数
main "$@"
