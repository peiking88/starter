---
name: /build
description: 构建Seastar项目（支持并行编译）
allowed-tools: [Bash, Execute]
---

# Seastar项目构建命令

## 快速构建
```bash
# 清理并重新构建
rm -rf build && mkdir build
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 调试构建
```bash
cmake -Bbuild -S. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc) -C build
```

## Ninja构建
```bash
CC=clang CXX=clang++ cmake -Bbuild -S. -GNinja
ninja -C build
```

## 依赖更新
```bash
git submodule update --init --recursive
./3rdparty/seastar/install-dependencies.sh
```