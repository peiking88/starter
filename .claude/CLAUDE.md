# Seastar Starter Project - Claude Code配置

## 项目概述
高性能C++并行计算项目，基于Seastar框架，支持多种并行计算框架的性能对比。

## 技术栈
- **语言**: C++23
- **框架**: Seastar (事件驱动)、libfork (协程)、Taskflow (任务图)
- **构建**: CMake 3.14+
- **编译器**: GCC 15+ 或 Clang 14+

## 构建命令
```bash
# 快速构建
rm -rf build && mkdir build
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Ninja构建
CC=clang CXX=clang++ cmake -Bbuild -S. -GNinja
ninja -C build

# 调试构建
cmake -Bbuild -S. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc) -C build
```

## 依赖管理
```bash
# 初始化子模块
git submodule update --init --recursive

# Seastar依赖
./3rdparty/seastar/install-dependencies.sh
```

## 执行程序
- **big_file_splitter**: 大文件并行分割器
- **prime_calculator**: 素数并行计算器
- **prime_bench**: 多框架性能基准测试

## 开发规范
- 使用C++23标准特性
- 遵循Seastar异步编程模式
- 保持并行算法的正确性和性能
- 所有实现需通过一致性验证

## 测试与验证
- 所有并行实现必须产生相同结果
- 性能基准测试需包含多框架对比
- 内存使用需优化避免OOM

## Claude Code工具建议
- 频繁使用：Read、Bash、Execute、Search
- 推荐自动允许：Read、Bash、Execute
- 特定任务：C++代码分析、性能优化建议