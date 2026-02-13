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

### Seastar 程序通用参数

Seastar程序使用框架内置的命令行参数处理：

```bash
./<program> -t <tasks> -n <chunk> -o <output> -l <log-level> -c <cores>
```

**通用参数说明:**
| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-t, --tasks` | 任务总数 | 20 |
| `-n, --chunk` | 每个任务的区间大小 | 100000 |
| `-o, --output` | 输出CSV文件路径 | `<program_name>.csv` |
| `-l, --log-level` | 日志级别 (debug/info/error/trace) | error |
| `-c, --smp` | CPU核心数 (Seastar框架参数) | 系统核心数 |

### minimax_seastar_prime

使用Seastar框架的素数计算器，采用**工作窃取模式**实现动态负载均衡。

**特点:**
- 无锁原子计数器实现任务分发
- 异步 DMA I/O 文件写入，避免 reactor 阻塞
- 各核心独立获取任务，无单点瓶颈

```bash
# 基本运行
./minimax_seastar_prime -t 20 -n 100000 -c 4

# 启用调试日志
./minimax_seastar_prime -t 10 -n 10000 -l debug -c 4
```

**输出格式:**
- CSV文件: `minimax_seastar_prime.csv` (或 `-o` 指定)
- 每行格式: `<start>-<end>,<core_id>,<prime1>,<prime2>,...`

### glm5_seastar_prime

使用Seastar框架的素数计算器，采用**集中式队列 (fork-join模式)**。

**特点:**
- 集中式任务队列由 core 0 管理
- 异步 DMA I/O 文件写入，避免 reactor 阻塞
- 任务获取和结果收集通过 `submit_to(0)` 通信

```bash
# 基本运行
./glm5_seastar_prime -t 20 -n 100000 -c 4

# 指定输出文件
./glm5_seastar_prime -t 20 -n 100000 -o result.csv -c 4
```

**输出格式:**
- CSV文件: `glm5_seastar_prime.csv` (或 `-o` 指定)
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

### Seastar 程序特性

1. **异步编程模式**: 使用 Seastar 的 `future<>`/`.then()` 调用链
2. **异步 DMA I/O**: 使用 `open_file_dma()` + `dma_write()` 避免阻塞 reactor
3. **seastar::async**: 将 CPU 密集计算放入后台线程
4. **seastar::repeat**: 高效的任务循环处理
5. **命令行参数**: 使用 `app_template::add_options()` 框架处理

### 两种 Seastar 实现对比

| 特性 | minimax_seastar | glm5_seastar |
|------|-----------------|--------------|
| **任务调度** | 无锁原子计数器 (工作窃取) | 集中式队列 (core 0) |
| **核心通信** | 最小化 | 频繁 `submit_to(0)` |
| **文件I/O** | 异步 DMA | 异步 DMA |
| **扩展性** | 线性好 | core 0 可能成为瓶颈 |
| **适用场景** | 高并发、多核 | 小规模、调试方便 |

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
