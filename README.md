# Seastar Starter Project

高性能C++并行计算项目，基于Seastar框架，支持多种并行计算框架的性能对比。

## 项目概述

本项目展示了多种并行计算框架的使用方法和性能对比：

| 程序 | 框架 | 描述 |
|------|------|------|
| `big_file_splitter` | Seastar | 大文件并行分割器 |
| `glm5_seastar_prime` | Seastar | 素数并行计算器 (fork-join模式) |
| `minimax_seastar_prime` | Seastar | 素数并行计算器 (工作窃取模式) |
| `glm5_libfork_prime` | libfork | 素数并行计算器 (fork-join模式) |
| `minimax_libfork_prime` | libfork | 素数并行计算器 (工作窃取模式) |
| `sequence_prime` | 标准库 | 顺序素数计算器 |
| `prime_bench` | 标准库 | 多框架性能基准测试 |

## 系统要求

- **编译器**: C++20兼容编译器 (GCC 15+, Clang 14+)
- **CMake**: 3.14或更高版本
- **操作系统**: Ubuntu 25.10+ (GCC 15.2.0, Clang 16)

## 快速开始

### 安装依赖

```bash
# 安装构建工具
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build clang

# 初始化子模块
git submodule update --init --recursive

# 安装Seastar依赖
./3rdparty/seastar/install-dependencies.sh
```

### 构建项目

```bash
# 清理并重新构建
rm -rf build && mkdir build
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Ninja构建

```bash
CC=clang CXX=clang++ cmake -Bbuild -S. -GNinja
ninja -C build
```

## 程序使用

### minimax_seastar_prime

使用Seastar框架的素数计算器，采用工作窃取模式实现动态负载均衡。

```bash
# 基本运行（使用环境变量配置）
NUM_TASKS=20 CHUNK_SIZE=100000 NUM_CORES=32 ./minimax_seastar_prime

# 启用调试日志
LOG_LEVEL=debug NUM_TASKS=10 CHUNK_SIZE=10000 NUM_CORES=4 ./minimax_seastar_prime
```

**参数说明:**
- `NUM_TASKS`: 任务总数 (默认: 20)
- `CHUNK_SIZE`: 每个任务的区间大小，不超过10万 (默认: 100000)
- `NUM_CORES`: CPU核心数 (默认: 4)
- `LOG_LEVEL`: 日志级别 - error/info/debug/trace (默认: error)

**输出格式:**
- CSV文件: `primes_<tasks>_<chunk>.csv`
- 每行格式: `<start>-<end>,<core_id>,<prime1>,<prime2>,...`

### prime_bench

多框架性能基准测试，比较不同并行框架的性能。

```bash
./prime_bench -t 32 -n 100000 -c 32
```

**参数说明:**
- `-t <N>`: 任务总数 (默认: 4)
- `-n <N>`: 区间大小 (默认: 100000)
- `-c <N>`: 线程数 (默认: 4)

**示例输出:**
```
============================================================
素数计算性能基准测试
============================================================
计算范围: 2 - 3200000
任务数:   32
区间大小: 100000
线程数:   32
============================================================

框架                         素数总数   耗时(ms)
------------------------------------------------------------
sequence_prime                       230204          157
minimax_libfork_prime                230204           29
glm5_libfork_prime                   230204           23
glm5_seastar_prime                   230204          446
minimax_seastar                      230204          589
------------------------------------------------------------
结果一致性: ✓ 通过
============================================================

最快框架: glm5_libfork_prime (23ms)
============================================================
```

### big_file_splitter

大文件并行分割器。

```bash
# 生成测试文件
dd if=/dev/zero of=input.dat bs=4096 count=50000

# 运行分割器
./big_file_splitter --input input.dat -m500 -c5 --memory-pct 1.0

# 清理
rm -f input.dat chunk.*
```

## 技术特性

### minimax_seastar_prime 特性

1. **.then()调用链**: 使用Seastar的future/promise模式，避免C++20协程
2. **工作窃取**: 使用原子计数器实现动态任务分配
3. **submit_to**: 使用`smp::submit_to()`将任务提交到指定CPU核心
4. **Seastar日志**: 使用`seastar::logger`进行日志记录
5. **CSV输出**: 格式化的结果输出

### 代码结构

```
/home/li/starter/
├── CMakeLists.txt          # CMake配置
├── README.md               # 项目说明
├── big_file_splitter.cpp   # 文件分割器
├── glm5_seastar_prime.cpp  # Seastar fork-join模式
├── minimax_seastar_prime.cpp # Seastar工作窃取模式
├── glm5_libfork_prime.cpp  # libfork fork-join模式
├── minimax_libfork_prime.cpp # libfork工作窃取模式
├── sequence_prime.cpp      # 顺序计算
├── prime_bench.cpp         # 性能基准测试
└── 3rdparty/
    ├── seastar/            # Seastar框架
    └── libfork/            # libfork框架
```

## 许可证

Apache License 2.0
