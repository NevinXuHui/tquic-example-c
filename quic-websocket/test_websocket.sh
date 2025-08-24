#!/bin/bash

# 测试 WebSocket 客户端的脚本

echo "🚀 Testing Enhanced QUIC WebSocket Client"
echo "=========================================="

# 测试命令序列
TEST_COMMANDS="
/subscribe sensors,stocks,system
Hello WebSocket Server!
/list
/broadcast Hello from automated test!
/ping
/unsubscribe sensors
/quit
"

echo "📝 Test commands to be executed:"
echo "$TEST_COMMANDS"
echo ""

echo "🔗 Starting WebSocket client test..."
echo "$TEST_COMMANDS" | timeout 15 cargo run --example websocket_client -- \
    --server 127.0.0.1:4433 \
    --name "TestUser" \
    --verbose

echo ""
echo "✅ Test completed!"
