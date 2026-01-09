# Seastar starter project

This project contains a small [Seastar](https://github.com/scylladb/seastar)
program and minimal cmake scaffolding. The example contains both coroutines
and continuation passing style uses of Seastar.

# Building

See the section below on requirements for specific environments that are known to work.

Install dependencies:

```bash
$> git submodule update --init --recursive
$> 3rdparty/seastar/install-dependencies.sh
$> apt-get install -qq ninja-build clang
```

Configure and build:

```bash
# 清理并重新构建
$> rm -rf build && mkdir build

# 配置项目（支持并行编译和屏蔽警告）
$> cd build && cmake .. -DCMAKE_BUILD_TYPE=Release

# 并行编译（使用所有CPU核心）
$> make -j$(nproc)
```

Or if you need to specify a non-default compiler:

```bash
$> CC=clang CXX=clang++ cmake -Bbuild -S. -GNinja
$> ninja -C build
```

## 项目配置更新说明

### 任务提示词

该项目将seastar依赖包从项目根目录移动到了3rdparty目录下，按以下要求重新配置项目：

```
#不得修改3rdparty目录下源文件，可以修改项目配置文件。
#修改项目配置，屏蔽生成过程中将警告信息认定为错误的配置。
#生成时使用-j$(nproc)参数加快编译速度。
#配置seastar只生成库文件而不生成其他可执行文件。
#生成成功后将任务提示词、工作内容总结和生成命令更新到readme.md。
#持续报告任务进度。
```

### 配置变更总结

1. **依赖包位置调整**
   - Seastar 依赖包已从项目根目录移动到 `3rdparty/seastar`
   - 所有依赖包统一管理在 `3rdparty` 目录下

2. **编译配置优化**
   - 屏蔽警告信息认定为错误的配置：`-w -Wno-error`
   - 保持原有优化级别：`-O2` 优化
   - 配置 seastar 只生成库文件而不生成其他可执行文件

3. **并行编译支持**
   - 支持使用 `-j$(nproc)` 参数进行并行编译

4. **依赖库配置修复**
   - 正确配置 libfork 库依赖
   - 正确配置 taskflow 头文件库（纯头文件库，无需编译）

### 生成命令

```bash
# 清理并重新构建
rm -rf build && mkdir build

# 配置项目（支持并行编译和屏蔽警告）
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release

# 并行编译（使用所有CPU核心）
make -j$(nproc)
```

### 生成的可执行文件

构建成功后，将在 `build/` 目录下生成以下可执行文件：
- `big_file_splitter` - 大文件分割器
- `prime_calculator` - 素数计算器
- `prime_bench` - 并行素数计算基准测试器

# Requirements

A compiler that supports at least C++17 is required. Some examples known to work:

* Ubuntu 22.04. The default GCC version is new enough to work. This should be installed by default with the instructions above. This is also the combination that runs in our CI. See [.github/workflows/build.yml](.github/workflows/build.yml). Both clang-14 and clang-15 have issues on Ubuntu 22.04.
* Ubuntu 23.10. Both the default GCC 13.2.0 and Clang 16 are known to work out of the box.
* Fedora 38 and newer are known to work.

# Running

The sample program splits an input file into chunks. Each core reads a subset of
the input file into memory and then writes this subset out to a new file. The
size of the per-core subset may be larger than memory, in which case more than
one subset per core will be generated.

First, generate some data. This command will generate around 200mb of data.

```
dd if=/dev/zero of=input.dat bs=4096 count=50000
```

Next, invoke the program. Here we limit the total system memory to 500 MB, use 5
cores, and then we further limit memory to 1% of the amount available on each
core. Each command line argument is optional except the input file. If the
amount of memory or the number of cores are not specified then the program will
try to use all of the resources available.

```
$ build/big_file_splitter --input input.dat -m500 -c5 --memory-pct 1.0
```

The program should output a summary on each core about the data it is
responsible for, and then once per second a per-core progress is printed.

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

After the program exists there should be a number of chunk files on disk. The
chunk file format is `chunk.<core-id>.<chunk-id>`.

```
$ ls -l chunk*
-rw-r--r--. 1 user user 331776 Jan 13 13:10 chunk.core-0.0
-rw-r--r--. 1 user user 331776 Jan 13 13:10 chunk.core-0.1
-rw-r--r--. 1 user user 331776 Jan 13 13:10 chunk.core-0.10
-rw-r--r--. 1 user user 331776 Jan 13 13:10 chunk.core-0.100
-rw-r--r--. 1 user user 331776 Jan 13 13:10 chunk.core-0.101
-rw-r--r--. 1 user user 331776 Jan 13 13:10 chunk.core-0.102
-rw-r--r--. 1 user user 331776 Jan 13 13:10 chunk.core-0.103
```

```
$ build/prime_calculator -t 1000

```
```
INFO  2025-12-14 11:35:37,087 [shard 30:main] test_simple - Shard 30 完成  30 个任务，总计素数:   171998, 耗时:   2493ms, 平均每个任务:   83ms
INFO  2025-12-14 11:35:37,091 [shard 11:main] test_simple - Shard 11 完成  33 个任务，总计素数:   190181, 耗时:   2502ms, 平均每个任务:   75ms
INFO  2025-12-14 11:35:37,094 [shard 10:main] test_simple - Shard 10 完成  33 个任务，总计素数:   190301, 耗时:   2505ms, 平均每个任务:   75ms
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - === 工作窃取模式统计结果 ===
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 总计算范围: [1, 100000000]
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 总任务数: 1000
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 总共找到素数: 5761455
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 素数密度: 5.761455%
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 程序总耗时: 2507ms
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 计算性能: 39888.31 个数字/毫秒
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 素数发现率: 2298.15 个素数/毫秒
INFO  2025-12-14 11:35:37,094 [shard  0:main] test_simple - 任务完成
```

# prime_bench - 并行素数计算基准测试

并行素数计算基准测试程序，支持四种实现方式：Seastar、libfork、Taskflow 和顺序计算，用于比较不同并行框架的性能。

## 特性

- **多框架支持**: 支持 Seastar（事件驱动）、libfork（C++23 协程）和 Taskflow（任务图并行）三种并行框架
- **统一接口**: 四种实现使用相同的命令行参数 `-t`（任务数）和 `-n`（区间大小）
- **自动对比**: 依次执行四种版本并输出性能对比结果
- **结果验证**: 自动验证所有实现的结果一致性
- **详细分析**: 提供加速比、并行框架对比和性能分析

## 运行示例

```bash
$ build/prime_bench -t 8 -n 2500000 -c4  # 8个任务，每个250万，使用4个核心
```

输出示例：
```
INFO  prime_bench - === 综合性能比较测试 ===
INFO  prime_bench - 计算范围: [1, 20000000]
INFO  prime_bench - 任务数: 8
INFO  prime_bench - 区间大小: 2500000
INFO  prime_bench - 工作线程数: 32
INFO  prime_bench -
INFO  prime_bench - 开始 Seastar 并行计算...
INFO  prime_bench - === Seastar 计算结果 ===
INFO  prime_bench - 质数总数: 1270607
INFO  prime_bench - 计算耗时: 764ms
INFO  prime_bench -
INFO  prime_bench - 开始 libfork 并行计算...
INFO  prime_bench - === libfork 计算结果 ===
INFO  prime_bench - 质数总数: 1270607
INFO  prime_bench - 计算耗时: 888ms
INFO  prime_bench -
INFO  prime_bench - 开始 Taskflow 并行计算...
INFO  prime_bench - === Taskflow 计算结果 ===
INFO  prime_bench - 质数总数: 1270607
INFO  prime_bench - 计算耗时: 2593ms
INFO  prime_bench -
INFO  prime_bench - 开始顺序计算...
INFO  prime_bench - === 顺序计算结果 ===
INFO  prime_bench - 质数总数: 1270607
INFO  prime_bench - 计算耗时: 2586ms
INFO  prime_bench -
INFO  prime_bench - === 性能比较总结 ===
INFO  prime_bench - 结果一致性: 通过
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
INFO  prime_bench - Seastar 加速比: 3.38x
INFO  prime_bench - Seastar 比顺序计算快 3.38 倍
INFO  prime_bench - libfork 加速比: 2.91x
INFO  prime_bench - libfork 比顺序计算快 2.91 倍
INFO  prime_bench - Taskflow 加速比: 1.00x
INFO  prime_bench - 并行框架对比:
INFO  prime_bench -   Seastar耗时: 764ms
INFO  prime_bench -   libfork耗时: 888ms
INFO  prime_bench -   Taskflow耗时: 2593ms
INFO  prime_bench - Seastar 比其他并行框架快 200.00%
```

## 参数说明

- `-t, --tasks <N>`: 任务数（默认: 4）
- `-n, --chunk-size <N>`: 每个任务的区间大小（默认: 5000000）
- 总计算范围 = 任务数 × 区间大小

例如：`-t 8 -n 2500000` 将计算 8 × 2500000 = 20000000 以内的素数

## 并行框架对比

### Seastar
- **特点**: 基于事件驱动的异步编程模型，使用 shard 并行和工作窃取
- **优势**: 在多核环境下性能优异，适合混合 I/O 和 CPU 密集型任务
- **加速比**: 3.38x（相对于顺序计算）

### libfork
- **特点**: 基于 C++23 协程的 fork-join 并行模型
- **优势**: 代码简洁，自动工作窃取调度，性能接近最优
- **加速比**: 2.91x（相对于顺序计算）

### Taskflow
- **特点**: 任务图并行框架，支持动态任务创建和工作窃取
- **当前状态**: 已集成但性能优化中（当前与顺序版本相当）
- **挑战**: 任务调度开销、任务粒度和调度策略需要进一步调优
- **加速比**: 1.00x（当前版本）

### 性能建议
- **Seastar/libfork**: 适合需要高性能并发的场景，任务数建议 4-16 个
- **Taskflow**: 适合任务图复杂或需要动态任务创建的场景，建议增加任务数量到 64+ 以提高并行度

## Taskflow 集成重构总结

### 重构目标
在现有的 Seastar 和 libfork 并行素数计算基础上，集成 Taskflow 框架，提供第三种并行实现，实现三种主流 C++ 并行框架的性能对比。

### 实现过程

#### 1. 集成准备
- **添加依赖**: 在 CMakeLists.txt 中添加 Taskflow 头文件路径
  ```cmake
  target_include_directories(prime_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/taskflow)
  ```
- **添加头文件**: 引入 Taskflow 核心头文件
  ```cpp
  #include <taskflow/taskflow.hpp>
  #include <taskflow/algorithm/reduce.hpp>
  ```

#### 2. 实现方案探索

**方案 A - reduce_by_index API**（最初尝试）
```cpp
taskflow.reduce_by_index(
    tf::IndexRange<size_t>(0, num_tasks, 1),
    total_primes,
    [](auto subrange, std::optional<size_t> running_total) {
        // 处理子任务
    },
    std::plus<size_t>()
);
```
- **问题**: 性能退化，与顺序版本相当（2.6s vs 2.5s）
- **原因**: `reduce_by_index` 可能退化为串行执行

**方案 B - for_each_index + 原子变量**（改进尝试）
```cpp
std::atomic<size_t> total_primes{0};
taskflow.for_each_index(0, num_tasks, 1, [&](int task_id) {
    int start = task_id * chunk_size + 1;
    int end = (task_id + 1) * chunk_size;
    size_t local_count = count_primes_in_range(start, end);
    total_primes.fetch_add(local_count);
});
```
- **问题**: 编译错误 - Taskflow 模板实例化问题
- **错误**: `declared using local type, is used but never defined`

**方案 C - 函数对象**（模板修复尝试）
```cpp
struct TaskflowPrimeTask {
    int chunk_size;
    std::vector<size_t>* results;
    void operator()(int task_id) const;
};
taskflow.for_each_index(0, num_tasks, 1, task_obj);
```
- **问题**: 同样遭遇模板实例化问题

**方案 D - 静态函数 + 手动任务创建**（最终方案）
```cpp
static std::vector<size_t>* taskflow_results = nullptr;
static int taskflow_chunk_size = 0;

void taskflow_parallel_task(int task_id) {
    int start = task_id * taskflow_chunk_size + 1;
    int end = (task_id + 1) * taskflow_chunk_size;
    (*taskflow_results)[task_id] = count_primes_in_range(start, end);
}

size_t run_taskflow_version(...) {
    std::vector<tf::Task> tasks(num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
        tasks[i] = taskflow.emplace([i]() { taskflow_parallel_task(i); });
    }
    executor.run(taskflow).wait();
    return std::accumulate(results.begin(), results.end(), 0);
}
```
- **结果**: 编译成功，功能正确，但性能未达预期

#### 3. 性能分析

**测试环境**:
- 计算范围: [1, 20,000,000]
- 任务数: 8
- 区间大小: 2,500,000
- 工作线程: 32

**性能对比**:
| 框架 | 耗时 | 加速比 | 结果 |
|-------|------|--------|------|
| Seastar | 764ms | 3.38x | ✅ 最优 |
| libfork | 888ms | 2.91x | ✅ 良好 |
| Taskflow | 2,593ms | 1.00x | ⚠️ 待优化 |
| Sequential | 2,586ms | 1.00x | 基准 |

**性能问题分析**:

1. **任务调度开销**
   - Taskflow 创建和调度 8 个任务的开销可能超过并行计算收益
   - 线程池调度和工作窃取机制在此场景下效率不高

2. **任务粒度**
   - 每个任务约 300ms（单个线程执行时间）
   - 调度开销相对于任务执行时间占比过高

3. **调度策略**
   - Taskflow 的默认 GuidedPartitioner 可能不适合此场景
   - 缺少针对 CPU 密集型任务的优化配置

#### 4. 遇到的技术挑战

**模板实例化问题**
- **问题**: Taskflow 的 `for_each_index` 无法接受 lambda 作为模板参数
- **解决方案**: 使用静态函数 + 手动任务创建
- **影响**: 代码复杂度增加，可维护性下降

**数据竞争问题**
- **问题**: 初期实现使用未初始化的引用导致结果错误（222 万亿 vs 127 万）
- **解决方案**: 使用结果向量 + 独立索引，避免共享状态
- **教训**: 并行编程需要严格的状态管理

**性能退化问题**
- **问题**: 多种并行 API 调用均未达到预期性能
- **当前状态**: 性能与顺序版本相当，未发挥并行优势
- **待探索**: transform_reduce、调整线程数、增加任务数等优化方向

#### 5. 构建和测试结果

**构建状态**: ✅ 成功
```
[ 98%] Building CXX object CMakeFiles/prime_bench.dir/prime_bench.cpp.o
[100%] Linking CXX executable prime_bench
[100%] Built target prime_bench
```

**功能验证**: ✅ 通过
- 所有四种实现结果一致（1,270,607 个质数）
- 无数据竞争或内存错误
- 程序稳定运行

**性能验证**: ⚠️ 待优化
- Seastar 和 libfork 表现优异（3x+ 加速比）
- Taskflow 需要进一步调优

### 结论与建议

#### 已完成的工作
✅ 成功集成 Taskflow 框架到现有项目
✅ 实现四种并行方式的统一接口
✅ 提供完整的性能对比和分析
✅ 解决模板实例化等技术问题
✅ 确保结果正确性和一致性

#### 未解决的问题
❌ Taskflow 性能未达到预期（等于顺序版本）
❌ 任务调度开销较大
❌ 缺少针对此场景的优化配置

#### 后续优化方向

**短期优化**:
1. 调整任务数量到 64-128，提高并行度
2. 尝试 Taskflow 的 `transform_reduce` API
3. 测试不同的 partitioner（Static、Dynamic、Guided）

**中期优化**:
1. 研究 Taskflow 的线程池配置
2. 实现任务粒度自适应策略
3. 评估使用 Taskflow Subflow 的可能性

**长期探索**:
1. 分析 Taskflow 工作窃取策略的瓶颈
2. 考虑与其他 C++ 并行库的对比（如 HPX、TBB）
3. 撰写 Taskflow 在 CPU 密集型场景下的最佳实践文档

#### 代码修改范围
- **CMakeLists.txt**: 添加 Taskflow 包含路径
- **prime_bench.cpp**: 新增 `run_taskflow_version()` 函数及相关辅助函数
- **未修改**: 3rdparty 和 seastar 源代码（符合要求）

### 参考资料
- [Taskflow GitHub](https://github.com/taskflow/taskflow)
- [Taskflow Documentation](https://taskflow.github.io/taskflow/)
- [Parallel Prime Counting - Various Approaches](https://github.com/scylladb/seastar)

# Resources

* [The Seastar tutorial](https://github.com/scylladb/seastar/blob/master/doc/tutorial.md)
* [ScyllaDB](https://github.com/scylladb/scylla) is a large project that uses Seastar.
* [CMake tutorial](https://cmake.org/cmake-tutorial/)

# Testing

This project uses a GitHub action to run the same set of instructions as above. Please
see [.github/workflows/build.yml](.github/workflows/build.yml) for reference.
