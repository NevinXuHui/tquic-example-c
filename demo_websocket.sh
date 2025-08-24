#!/bin/bash

# TQUIC WebSocket 双向通信演示脚本

echo "🚀 TQUIC WebSocket 双向通信演示"
echo "=================================="
echo ""

# 检查是否已编译
if [ ! -f "./tquic_websocket_server" ] || [ ! -f "./tquic_websocket_interactive_client" ]; then
    echo "📦 编译 WebSocket 程序..."
    make tquic_websocket_server tquic_websocket_interactive_client
    if [ $? -ne 0 ]; then
        echo "❌ 编译失败"
        exit 1
    fi
    echo "✅ 编译成功"
    echo ""
fi

echo "🎯 功能特性："
echo "  ✅ 基于 QUIC 协议的 WebSocket over HTTP/3"
echo "  ✅ 完整的双向实时通信"
echo "  ✅ 交互式用户界面"
echo "  ✅ 自动消息回显"
echo "  ✅ 优雅的连接关闭"
echo ""

echo "🔧 使用方法："
echo "  1. 启动服务器: ./tquic_websocket_server 127.0.0.1 4433"
echo "  2. 启动客户端: ./tquic_websocket_interactive_client 127.0.0.1 4433"
echo "  3. 在客户端输入消息进行双向通信"
echo "  4. 输入 'quit' 或 'exit' 退出客户端"
echo ""

echo "📋 测试步骤："
echo "  1. 服务器将自动发送欢迎消息"
echo "  2. 客户端可以发送任意文本消息"
echo "  3. 服务器会回显所有接收到的消息"
echo "  4. 支持实时双向通信"
echo ""

echo "🌟 技术亮点："
echo "  • 使用 tquic 库实现 QUIC 协议"
echo "  • 符合 RFC 9220 WebSocket over HTTP/3 标准"
echo "  • 支持 TLS 1.3 加密"
echo "  • 高性能异步事件处理"
echo "  • 完整的 WebSocket 帧解析"
echo ""

echo "🚀 现在可以开始测试双向通信了！"
echo ""
echo "启动服务器："
echo "  ./tquic_websocket_server 127.0.0.1 4433"
echo ""
echo "启动交互式客户端："
echo "  ./tquic_websocket_interactive_client 127.0.0.1 4433"
echo ""
echo "或者运行自动化测试："
echo "  ./tquic_websocket_client 127.0.0.1 4433"
