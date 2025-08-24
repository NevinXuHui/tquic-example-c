#!/bin/bash

# 分层 WebSocket 客户端构建脚本

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 默认值
BUILD_TYPE="Release"
BUILD_DIR="build"
CLEAN=false
INSTALL=false
VERBOSE=false
BUILD_EXAMPLES=true
BUILD_TESTS=false
JOBS=$(nproc 2>/dev/null || echo 4)

# 打印函数
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

# 显示帮助
show_usage() {
    cat << EOF
分层 WebSocket 客户端构建脚本

用法: $0 [选项]

选项:
    -h, --help              显示此帮助信息
    -t, --type TYPE         构建类型: Debug, Release (默认: Release)
    -d, --dir DIR           构建目录 (默认: build)
    -c, --clean             清理构建目录
    -i, --install           构建后安装
    -v, --verbose           详细输出
    -j, --jobs N            并行作业数 (默认: $(nproc 2>/dev/null || echo 4))
    --examples              构建示例程序 (默认: 开启)
    --no-examples           不构建示例程序
    --tests                 构建测试程序
    --no-tests              不构建测试程序 (默认)

示例:
    $0                      # 默认构建
    $0 --type Debug --clean # 清理并调试构建
    $0 --tests --verbose    # 构建测试并显示详细输出
    $0 --install            # 构建并安装

EOF
}

# 解析命令行参数
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
        --examples)
            BUILD_EXAMPLES=true
            shift
            ;;
        --no-examples)
            BUILD_EXAMPLES=false
            shift
            ;;
        --tests)
            BUILD_TESTS=true
            shift
            ;;
        --no-tests)
            BUILD_TESTS=false
            shift
            ;;
        *)
            print_error "未知选项: $1"
            show_usage
            exit 1
            ;;
    esac
done

# 验证构建类型
if [[ "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" && "$BUILD_TYPE" != "RelWithDebInfo" && "$BUILD_TYPE" != "MinSizeRel" ]]; then
    print_error "无效的构建类型: $BUILD_TYPE"
    print_info "有效类型: Debug, Release, RelWithDebInfo, MinSizeRel"
    exit 1
fi

print_info "分层 WebSocket 客户端构建配置:"
print_info "  构建类型: $BUILD_TYPE"
print_info "  构建目录: $BUILD_DIR"
print_info "  并行作业: $JOBS"
print_info "  构建示例: $BUILD_EXAMPLES"
print_info "  构建测试: $BUILD_TESTS"
print_info "  清理构建: $CLEAN"
print_info "  安装: $INSTALL"
print_info "  详细输出: $VERBOSE"

# 检查依赖
print_info "检查依赖..."

# 检查 TQUIC 库
TQUIC_LIB="../deps/tquic/target/release/libtquic.a"
if [[ ! -f "$TQUIC_LIB" ]]; then
    print_warning "TQUIC 库未找到: $TQUIC_LIB"
    print_info "正在构建 TQUIC 库..."
    
    if [[ -d "../deps/tquic" ]]; then
        cd ../deps/tquic
        cargo build --release -F ffi
        cd - > /dev/null
        print_success "TQUIC 库构建完成"
    else
        print_error "TQUIC 源码目录未找到: ../deps/tquic"
        print_info "请确保已正确初始化 git 子模块"
        exit 1
    fi
fi

# 检查系统依赖
check_header() {
    if ! echo "#include <$1>" | gcc -E - >/dev/null 2>&1; then
        print_error "缺少头文件: $1"
        print_info "请安装: sudo apt install $2"
        return 1
    fi
    return 0
}

check_library() {
    if ! echo "int main(){return 0;}" | gcc -l$1 -xc - -o /dev/null >/dev/null 2>&1; then
        print_error "缺少库文件: lib$1"
        print_info "请安装: sudo apt install $2"
        return 1
    fi
    return 0
}

DEPS_OK=true
check_header "ev.h" "libev-dev" || DEPS_OK=false
check_header "cjson/cJSON.h" "libcjson-dev" || DEPS_OK=false
check_library "ev" "libev-dev" || DEPS_OK=false
check_library "cjson" "libcjson-dev" || DEPS_OK=false

if [[ "$DEPS_OK" != true ]]; then
    print_error "请安装缺少的依赖后重试"
    exit 1
fi

# 清理构建目录
if [[ "$CLEAN" == true ]]; then
    print_info "清理构建目录: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# 创建构建目录
mkdir -p "$BUILD_DIR"

# 配置 CMake
print_info "配置 CMake..."
CMAKE_ARGS=(
    "-B" "$BUILD_DIR"
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DBUILD_EXAMPLES=$BUILD_EXAMPLES"
    "-DBUILD_TESTS=$BUILD_TESTS"
)

if [[ "$VERBOSE" == true ]]; then
    CMAKE_ARGS+=("--debug-output")
fi

cmake "${CMAKE_ARGS[@]}"

# 构建
print_info "开始构建..."
CMAKE_BUILD_ARGS=("--build" "$BUILD_DIR" "--parallel" "$JOBS")

if [[ "$VERBOSE" == true ]]; then
    CMAKE_BUILD_ARGS+=("--verbose")
fi

cmake "${CMAKE_BUILD_ARGS[@]}"

print_success "构建完成!"

# 显示构建结果
print_info "构建产物:"
if [[ -d "$BUILD_DIR/bin" ]]; then
    ls -la "$BUILD_DIR/bin/"
fi

if [[ -d "$BUILD_DIR/lib" ]]; then
    print_info "库文件:"
    ls -la "$BUILD_DIR/lib/"
fi

# 安装
if [[ "$INSTALL" == true ]]; then
    print_info "安装..."
    cmake --install "$BUILD_DIR"
    print_success "安装完成!"
fi

# 运行测试
if [[ "$BUILD_TESTS" == true ]]; then
    print_info "运行测试..."
    cd "$BUILD_DIR"
    ctest --output-on-failure
    cd - > /dev/null
    print_success "测试完成!"
fi

print_success "所有操作完成! 🎉"

if [[ "$BUILD_EXAMPLES" == true ]]; then
    print_info "运行示例程序:"
    print_info "  聊天客户端: ./$BUILD_DIR/bin/chat_client 127.0.0.1 4433 myusername"
    print_info "  JSON客户端: ./$BUILD_DIR/bin/json_client 127.0.0.1 4433 json_user"
fi
