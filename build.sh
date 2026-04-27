#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

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

print_usage() {
    echo "用法: $0 [选项] [目标]"
    echo ""
    echo "选项:"
    echo "  -r, --release    构建 Release 版本 (build/release)"
    echo "  -d, --debug      构建 Debug 版本 (build/debug)"
    echo "  -a, --all        构建 Release 和 Debug 版本"
    echo "  -c, --clean      清理构建目录后重新构建"
    echo "  -j, --jobs N     并行编译任务数 (默认: $(nproc))"
    echo "  -h, --help       显示此帮助信息"
    echo ""
    echo "目标:"
    echo "  all              构建所有目标 (默认)"
    echo "  prime_bench      只构建 prime_bench"
    echo "  glm5turbo_seastar_prime  只构建 glm5turbo_seastar_prime"
    echo "  ..."
    echo ""
    echo "示例:"
    echo "  $0 -r                    # Release 构建"
    echo "  $0 -d                    # Debug 构建"
    echo "  $0 -r -c                 # 清理后 Release 构建"
    echo "  $0 -a                    # 构建 Release 和 Debug"
    echo "  $0 -r -j 8 prime_bench   # 8 并行构建 prime_bench"
}

BUILD_RELEASE=false
BUILD_DEBUG=false
CLEAN_BUILD=false
JOBS=$(nproc)
TARGET="all"

while [[ $# -gt 0 ]]; do
    case $1 in
        -r|--release)
            BUILD_RELEASE=true
            shift
            ;;
        -d|--debug)
            BUILD_DEBUG=true
            shift
            ;;
        -a|--all)
            BUILD_RELEASE=true
            BUILD_DEBUG=true
            shift
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        -*)
            print_error "未知选项: $1"
            print_usage
            exit 1
            ;;
        *)
            TARGET="$1"
            shift
            ;;
    esac
done

if [[ "$BUILD_RELEASE" == false && "$BUILD_DEBUG" == false ]]; then
    BUILD_RELEASE=true
fi

build() {
    local build_type="$1"
    local build_dir="build/${build_type,,}"
    
    print_info "构建 ${build_type} 版本 -> ${build_dir}"
    
    if [[ "$CLEAN_BUILD" == true ]]; then
        print_info "清理 ${build_dir}..."
        rm -rf "${build_dir}"
    fi
    
    mkdir -p "${build_dir}"
    
    print_info "配置 CMake..."
    cmake -B"${build_dir}" -S. \
        -DCMAKE_BUILD_TYPE="${build_type}" \
        -GNinja \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    
    print_info "编译 ${TARGET} (${JOBS} 并行任务)..."
    ninja -C "${build_dir}" -j "${JOBS}" "${TARGET}"
    
    print_success "${build_type} 构建完成: ${build_dir}"
}

echo ""
echo "========================================"
echo "  Seastar Starter 构建脚本"
echo "========================================"
echo ""

if [[ "$BUILD_RELEASE" == true ]]; then
    build "Release"
fi

if [[ "$BUILD_DEBUG" == true ]]; then
    build "Debug"
fi

if [[ "$BUILD_RELEASE" == true && "$BUILD_DEBUG" == true ]]; then
    ln -sf release/compile_commands.json build/compile_commands.json 2>/dev/null || true
elif [[ "$BUILD_RELEASE" == true ]]; then
    ln -sf release/compile_commands.json build/compile_commands.json 2>/dev/null || true
elif [[ "$BUILD_DEBUG" == true ]]; then
    ln -sf debug/compile_commands.json build/compile_commands.json 2>/dev/null || true
fi

echo ""
print_success "构建完成！"
echo ""
echo "可执行文件位置:"
if [[ "$BUILD_RELEASE" == true ]]; then
    echo "  Release: build/release/"
fi
if [[ "$BUILD_DEBUG" == true ]]; then
    echo "  Debug:   build/debug/"
fi
echo ""
