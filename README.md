# Seastar Starter Project

This project contains a small [Seastar](https://github.com/scylladb/seastar) program and minimal CMake scaffolding. The example contains both coroutines and continuation passing style uses of Seastar.

## Overview

This project demonstrates Seastar's capabilities through four main applications:
- **Big File Splitter**: Parallel file processing using Seastar's shard-based architecture
- **Prime Calculator**: Parallel prime number counting with work-stealing optimization
- **Prime Benchmark**: Performance comparison of different parallel frameworks (Seastar, libfork, Taskflow)
- **Pony Alpha**: Distributed prime calculator with CSV output and detailed logging

## Project Configuration Update

### Task Requirements

The project has been reconfigured according to the following requirements:

```
# Do not modify source files in 3rdparty directory, only modify project configuration files
# Modify project configuration to suppress treating warnings as errors
# Use -j$(nproc) parameter to speed up compilation
# Configure Seastar to generate only library files without other executables
# Update README.md with task requirements, work summary, and build commands upon successful generation
# Continuously report task progress
```

### Configuration Changes

1. **Dependency Location Adjustment**
   - Seastar dependency moved from project root to `3rdparty/seastar`
   - All dependencies unified under `3rdparty` directory management

2. **Compilation Configuration Optimization**
   - Suppress warnings being treated as errors: `-w -Wno-error`
   - Maintain original optimization level: `-O2` optimization
   - Configure Seastar to generate only library files

3. **Parallel Compilation Support**
   - Support using `-j$(nproc)` parameter for parallel compilation

4. **Dependency Library Configuration Fixes**
   - Correctly configured libfork library dependency
   - Correctly configured Taskflow header-only library (no compilation needed)

## Requirements

### System Requirements
- **Compiler**: C++23 compatible compiler (GCC 15+, Clang 14+)
- **CMake**: Version 3.14 or higher
- **Operating Systems**:
  - Ubuntu 25.10+ (GCC 15.2.0, Clang 16)
  
### Dependencies
```bash
# Install dependencies
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build clang

# Initialize and update submodules
git submodule update --init --recursive

# Install Seastar dependencies
./3rdparty/seastar/install-dependencies.sh
```

## Building

### Quick Build
```bash
# Clean and rebuild
rm -rf build && mkdir build

# Configure project (supports parallel compilation and warning suppression)
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release

# Parallel compilation (uses all CPU cores)
make -j$(nproc)
```

### Alternative Build Methods
```bash
# Using Ninja build system
CC=clang CXX=clang++ cmake -Bbuild -S. -GNinja
ninja -C build

# Debug build
cmake -Bbuild -S. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc) -C build
```

### Generated Executables
After successful build, the following executables will be generated in `build/` directory:
- `big_file_splitter` - Large file splitter
- `prime_calculator` - Prime number calculator
- `prime_bench` - Parallel prime calculation benchmark
- `pony_alpha` - Distributed prime calculator with CSV output

## Running the Applications

### Big File Splitter

The sample program splits an input file into chunks. Each core reads a subset of the input file into memory and then writes this subset out to a new file. The size of the per-core subset may be larger than memory, in which case more than one subset per core will be generated.

#### Example Usage

First, generate some test data (around 200MB):
```bash
dd if=/dev/zero of=input.dat bs=4096 count=50000
```

Run the program with resource constraints:
```bash
./build/big_file_splitter --input input.dat -m500 -c5 --memory-pct 1.0
```

**Command Line Arguments:**
- `--input`: Input file (required)
- `-m`: Total system memory limit in MB (optional)
- `-c`: Number of cores to use (optional)
- `--memory-pct`: Memory usage percentage per core (optional)

If memory or core parameters are not specified, the program will try to use all available resources.

#### Sample Output
```
INFO  2024-01-13 13:10:14,214 [shard 0] splitter - Processing 10000 pages with index 0 to 9999
INFO  2024-01-13 13:10:14,214 [shard 1] splitter - Processing 10000 pages with index 10000 to 19999
INFO  2024-01-13 13:10:14,214 [shard 3] splitter - Processing 10000 pages with index 30000 to 39999
INFO  2024-01-13 13:10:14,214 [shard 2] splitter - Processing 10000 pages with index 20000 to 29999
INFO  2024-01-13 13:10:14,214 [shard 4] splitter - Processing 10000 pages with index 40000 to 49999
INFO  2024-01-13 13:10:14,214 [shard 0] splitter - Progress: 0.0 0.0 0.0 0.0 0.0
INFO  2024-01-13 13:10:15,214 [shard 0] splitter - Progress: 54.5 54.3 55.2 53.8 53.6
INFO  2024-01-13 13:10:16,215 [shard 0] splitter - Progress: 100.0 100.0 100.0 100.0 100.0
```

After execution, chunk files will be created in the format: `chunk.<core-id>.<chunk-id>`

### Prime Calculator

Parallel prime number counting with work-stealing optimization.

#### Example Usage
```bash
./build/prime_calculator -t 1000
```

#### Sample Output
```
INFO  2025-12-14 11:35:37,087 [shard 30:main] test_simple - Shard 30 completed 30 tasks, total primes: 171998, time: 2493ms, average per task: 83ms
INFO  2025-12-14 11:35:37,091 [shard 11:main] test_simple - Shard 11 completed 33 tasks, total primes: 190181, time: 2502ms, average per task: 75ms
INFO  2025-12-14 11:35:37,094 [shard 10:main] test_simple - Shard 10 completed 33 tasks, total primes: 190301, time: 2505ms, average per task: 75ms
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - === Work-Stealing Mode Statistics ===
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - Total range: [1, 100000000]
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - Total tasks: 1000
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - Total primes found: 5761455
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - Prime density: 5.761455%
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - Total execution time: 2507ms
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - Computation performance: 39888.31 numbers/ms
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - Prime discovery rate: 2298.15 primes/ms
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - Task completed
```

### Prime Benchmark

Parallel prime calculation benchmark comparing different parallel frameworks (Seastar, async_simple, libfork, and sequential computation).

#### Features
- **Multi-framework Support**: Seastar (event-driven), async_simple (C++20 coroutines), libfork (C++23 coroutines)
- **Unified Interface**: All implementations use the same command-line parameters
- **Automatic Comparison**: Executes all four versions and outputs performance comparison
- **Result Validation**: Automatically verifies consistency across all implementations
- **Detailed Analysis**: Provides speedup ratios, framework comparison, and performance analysis

#### Example Usage
```bash
./build/prime_bench -t 8 -n 2500000 -c4  # 8 tasks, 2.5M each, using 4 cores
```

#### Sample Output
```
INFO  prime_bench - === Comprehensive Performance Comparison Test ===
INFO  prime_bench - Calculation range: [1, 20000000]
INFO  prime_bench - Task count: 8
INFO  prime_bench - Chunk size: 2500000
INFO  prime_bench - Worker threads: 32
INFO  prime_bench -
INFO  prime_bench - Starting Seastar parallel computation...
INFO  prime_bench - === Seastar Results ===
INFO  prime_bench - Total primes: 1270607
INFO  prime_bench - Computation time: 764ms
INFO  prime_bench -
INFO  prime_bench - Starting libfork parallel computation...
INFO  prime_bench - === libfork Results ===
INFO  prime_bench - Total primes: 1270607
INFO  prime_bench - Computation time: 888ms
INFO  prime_bench -
INFO  prime_bench - Starting Taskflow parallel computation...
INFO  prime_bench - === Taskflow Results ===
INFO  prime_bench - Total primes: 1270607
INFO  prime_bench - Computation time: 2593ms
INFO  prime_bench -
INFO  prime_bench - Starting sequential computation...
INFO  prime_bench - === Sequential Results ===
INFO  prime_bench - Total primes: 1270607
INFO  prime_bench - Computation time: 2586ms
INFO  prime_bench -
INFO  prime_bench - === Performance Comparison Summary ===
INFO  prime_bench - Result consistency: PASSED
INFO  prime_bench -   - Seastar: 1270607
INFO  prime_bench -   - libfork: 1270607
INFO  prime_bench -   - Taskflow: 1270607
INFO  prime_bench -   - Sequential: 1270607
INFO  prime_bench -
INFO  prime_bench - Seastar: 764ms
INFO  prime_bench - libfork: 888ms
INFO  prime_bench - Taskflow: 2593ms
INFO  prime_bench - Sequential: 2586ms
INFO  prime_bench -
INFO  prime_bench - Seastar speedup: 3.38x
INFO  prime_bench - Seastar is 3.38x faster than sequential
INFO  prime_bench - libfork speedup: 2.91x
INFO  prime_bench - libfork is 2.91x faster than sequential
INFO  prime_bench - Taskflow speedup: 1.00x
INFO  prime_bench - Parallel framework comparison:
INFO  prime_bench -   Seastar time: 764ms
INFO  prime_bench -   libfork time: 888ms
INFO  prime_bench -   Taskflow time: 2593ms
INFO  prime_bench - Seastar is 200.00% faster than other parallel frameworks
```

### Pony Alpha

Distributed prime calculator with detailed logging and CSV output. Demonstrates Seastar's multi-shard processing with task queue and work distribution.

#### Features
- **Multi-shard Processing**: Distributes tasks across all CPU cores
- **CSV Output**: Saves results to CSV format (range, shard_id, prime_list)
- **Progress Logging**: Real-time progress with prime count statistics
- **Timing Stats**: Total execution time measurement
- **Configurable Parameters**: Customizable task count and chunk size

#### Example Usage
```bash
# Default: 20 tasks, 100000 chunk size, calculates 2-2,000,000
./build/pony_alpha

# Custom configuration: 100 tasks, 10000 chunk size, using 4 cores
./build/pony_alpha -t 100 -n 10000 -c4 -m1G

# Large calculation: 2000 tasks, 1000 chunk size
./build/pony_alpha -t 2000 -n 1000 -c8 -m2G
```

**Command Line Arguments:**
- `-t, --tasks <N>`: Number of tasks (default: 20)
- `-n, --chunk <N>`: Numbers per task (default: 100000)
- `-o, --output <path>`: Output CSV file (default: primes.csv)
- `-c <N>`: Number of CPU cores to use (Seastar parameter)
- `-m <size>`: Memory limit (Seastar parameter)

#### Sample Output
```
配置: 任务数=20, 区间大小=5000, 计算范围=[2, 100000]

INFO  seastar - Reactor backend: linux-aio
INFO  pony_alpha - === 任务队列初始化完成 ===
INFO  pony_alpha - 计算范围: 2 - 100000
INFO  pony_alpha - 区间大小: 5000
INFO  pony_alpha - 总任务数: 20
INFO  pony_alpha - CPU核心数: 2
INFO  pony_alpha - === 开始写入结果文件 ===
INFO  pony_alpha - 输出文件: primes.csv
INFO  pony_alpha - 进度: 100.00% (20/20 任务, 素数: 9592)
INFO  pony_alpha - === 计算完成 ===
INFO  pony_alpha - 已完成任务: 20/20
INFO  pony_alpha - 素数总数: 9592
INFO  pony_alpha - 总耗时: 8ms
```

#### Output Format
The CSV file contains one line per task:
```
<start>-<end>,<shard_id>,<prime1>,<prime2>,<prime3>,...
```

Example:
```
2-5001,0,2,3,5,7,11,13,17,19,23,29,...
5002-10001,1,5003,5009,5011,5021,5023,...
```
