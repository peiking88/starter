// glm5_prime: 并行素数计算程序 - 使用 libfork 框架
// 工作模式：共享任务队列 + 工作窃取，动态负载均衡

#include <libfork/core.hpp>
#include <libfork/schedule.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>
#include <optional>
#include <iomanip>
#include <algorithm>
#include <memory>
#include <getopt.h>
#include "prime_sieve.hpp"

// ============================================================================
// 全局配置
// ============================================================================
struct Config {
    int num_tasks = 20;      // 任务总数
    int chunk_size = 100000; // 每个任务的区间大小（不超过10万）
    int num_threads = 4;     // 使用线程数
};

Config g_config;

// ============================================================================
// False Sharing 优化 — cache-line 对齐的原子类型
// ============================================================================
struct alignas(64) AlignedAtomicInt { std::atomic<int> value{0}; };
struct alignas(64) AlignedAtomicU64 { std::atomic<uint64_t> value{0}; };

// ============================================================================
// 任务队列 - 线程安全的工作窃取队列
// ============================================================================
class TaskQueue {
private:
    std::atomic<int> next_task_id_{0};
    int total_tasks_;

public:
    explicit TaskQueue(int total_tasks) : total_tasks_(total_tasks) {}

    // 获取下一个任务（原子操作，无需锁）
    std::optional<int> getNextTask() {
        int task_id = next_task_id_.fetch_add(1, std::memory_order_relaxed);
        if (task_id >= total_tasks_) [[unlikely]] {
            return std::nullopt;
        }
        return task_id;
    }

    void reset() { next_task_id_.store(0, std::memory_order_relaxed); }
};

// ============================================================================
// 任务结果结构 — noexcept 移动语义
// ============================================================================
struct TaskResult {
    int task_id;
    uint64_t start;
    uint64_t end;
    int core_id;
    std::vector<uint64_t> primes;

    TaskResult() = default;
    TaskResult(TaskResult&& other) noexcept
        : task_id(other.task_id), start(other.start), end(other.end),
          core_id(other.core_id), primes(std::move(other.primes)) {}
    TaskResult& operator=(TaskResult&& other) noexcept {
        if (this != &other) {
            task_id = other.task_id;
            start = other.start;
            end = other.end;
            core_id = other.core_id;
            primes = std::move(other.primes);
        }
        return *this;
    }
    TaskResult(const TaskResult&) = default;
    TaskResult& operator=(const TaskResult&) = default;
};

// ============================================================================
// 全局状态 — 消除 false sharing 和 mutex
// ============================================================================
constexpr int kMaxThreads = 128;

// per-thread 结果存储，避免 mutex 竞争
struct alignas(64) PaddedResults { std::vector<TaskResult> results; };
static PaddedResults g_results_per_thread[kMaxThreads];

static AlignedAtomicInt g_completed_tasks;
static AlignedAtomicU64 g_total_primes;

// unique_ptr 替代裸指针
static std::unique_ptr<TaskQueue> g_task_queue;

// ============================================================================
// libfork 并行任务 - 工作窃取模式
// ============================================================================

// 工作协程：不断从队列获取任务并执行
// 使用递归模式：每个worker处理完一个任务后，如果还有任务就继续
inline constexpr auto workerTask =
    [](auto self, int core_id) -> lf::task<void> {

    // 从共享队列获取下一个任务（工作窃取）
    auto task_opt = g_task_queue->getNextTask();

    if (!task_opt) [[unlikely]] {
        // 没有更多任务，退出
        co_return;
    }

    int task_id = *task_opt;

    // 计算任务区间（连续区间，无遗漏）
    // 第一个任务从2开始，后续任务紧接前一个任务
    uint64_t start = (task_id == 0) ? 2 : static_cast<uint64_t>(task_id) * g_config.chunk_size;
    uint64_t end = static_cast<uint64_t>(task_id + 1) * g_config.chunk_size;

    // 计算该区间的素数
    std::vector<uint64_t> primes = prime::segmented_sieve(start, end);
    size_t count = primes.size();

    // 收集结果到 per-thread 存储（无 mutex）
    {
        TaskResult result;
        result.task_id = task_id;
        result.start = start;
        result.end = end;
        result.core_id = core_id;
        result.primes = std::move(primes);
        g_results_per_thread[core_id].results.push_back(std::move(result));
    }

    // 更新统计（memory_order_relaxed）
    g_completed_tasks.value.fetch_add(1, std::memory_order_relaxed);
    g_total_primes.value.fetch_add(count, std::memory_order_relaxed);

    // 递归调用自己，继续处理下一个任务
    co_await lf::call[self](core_id);
};

// 并行启动多个worker
inline constexpr auto parallelCompute =
    [](auto self, int remaining_workers) -> lf::task<void> {

    if (remaining_workers <= 0) [[unlikely]] {
        co_return;
    }

    if (remaining_workers == 1) {
        // 最后一个worker，直接执行
        co_await lf::call[workerTask](0);
        co_return;
    }

    // Fork出第一个worker，然后递归处理剩余的
    co_await lf::fork[workerTask](remaining_workers - 1);
    co_await lf::call[self](remaining_workers - 1);

    co_await lf::join;
};

// ============================================================================
// 功能函数：初始化任务队列
// ============================================================================
std::unique_ptr<TaskQueue> initTaskQueue(int num_tasks, int chunk_size, int num_threads) {
    // 设置全局配置
    g_config.num_tasks = num_tasks;
    g_config.chunk_size = chunk_size;
    g_config.num_threads = num_threads;

    // 重置统计
    g_completed_tasks.value.store(0, std::memory_order_relaxed);
    g_total_primes.value.store(0, std::memory_order_relaxed);

    // 清空 per-thread 结果
    for (int i = 0; i < kMaxThreads; ++i) {
        g_results_per_thread[i].results.clear();
    }

    // 创建任务队列（unique_ptr）
    auto queue = std::make_unique<TaskQueue>(num_tasks);

    // 打印初始化信息
    std::cout << "\n========================================" << std::endl;
    std::cout << "       任务队列初始化完成" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "总任务数: " << num_tasks << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "工作线程: " << num_threads << std::endl;
    std::cout << "========================================\n" << std::endl;

    return queue;
}

// ============================================================================
// 功能函数：输出计算结果到CSV文件
// ============================================================================
void outputResults(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) [[unlikely]] {
        std::cerr << "错误: 无法打开输出文件 " << filename << std::endl;
        return;
    }

    std::cout << "\n正在写入结果文件: " << filename << std::endl;

    // 先合并所有 per-thread 结果
    std::vector<TaskResult> all_results;
    size_t total_count = 0;
    for (int i = 0; i < kMaxThreads; ++i) {
        total_count += g_results_per_thread[i].results.size();
    }
    all_results.reserve(total_count);
    for (int i = 0; i < kMaxThreads; ++i) {
        for (auto& r : g_results_per_thread[i].results) {
            all_results.push_back(std::move(r));
        }
        g_results_per_thread[i].results.clear();
    }

    // 按任务ID排序输出
    std::sort(all_results.begin(), all_results.end(),
              [](const TaskResult& a, const TaskResult& b) {
                  return a.task_id < b.task_id;
              });

    // 写入CSV
    for (const auto& result : all_results) {
        // 第一字段：任务范围
        file << result.start << "-" << result.end;
        // 第二字段：CPU核编号
        file << "," << result.core_id;
        // 后续字段：素数列表
        for (uint64_t prime : result.primes) {
            file << "," << prime;
        }
        file << "\n";
    }

    file.close();
    std::cout << "结果已写入: " << filename << std::endl;
}

// ============================================================================
// 功能函数：打印统计结果
// ============================================================================
void printStatistics(long duration_ms) noexcept {
    std::cout << "\n========================================" << std::endl;
    std::cout << "         计算结果统计" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "已完成任务: " << g_completed_tasks.value.load(std::memory_order_relaxed) << "/" << g_config.num_tasks << std::endl;
    std::cout << "素数总数:   " << g_total_primes.value.load(std::memory_order_relaxed) << std::endl;
    std::cout << "计算耗时:   " << duration_ms << " ms" << std::endl;

    uint64_t total_numbers = static_cast<uint64_t>(g_config.num_tasks) * g_config.chunk_size;
    double prime_density = 100.0 * g_total_primes.value.load(std::memory_order_relaxed) / total_numbers;

    std::cout << "素数密度:   " << std::fixed << std::setprecision(4) << prime_density << "%" << std::endl;
    if (duration_ms > 0) [[likely]] {
        std::cout << "计算速度:   " << std::fixed << std::setprecision(0)
                  << static_cast<double>(total_numbers) / duration_ms << " 数/毫秒" << std::endl;
        std::cout << "素数发现率: " << std::fixed << std::setprecision(2)
                  << static_cast<double>(g_total_primes.value.load(std::memory_order_relaxed)) / duration_ms << " 素数/毫秒" << std::endl;
    } else [[unlikely]] {
        std::cout << "计算速度:   N/A (耗时太短)" << std::endl;
        std::cout << "素数发现率: N/A (耗时太短)" << std::endl;
    }
    std::cout << "========================================" << std::endl;
}

// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char** argv) {
    // 默认参数
    int num_tasks = 20;
    int chunk_size = 100000;
    int num_threads = 4;

    // 解析命令行参数
    int opt;
    while ((opt = getopt(argc, argv, "t:n:c:h")) != -1) {
        switch (opt) {
            case 't':
                try { num_tasks = std::stoi(optarg); }
                catch (const std::exception&) { std::cerr << "错误: 无效的 -t 参数" << std::endl; return 1; }
                break;
            case 'n':
                try { chunk_size = std::stoi(optarg); }
                catch (const std::exception&) { std::cerr << "错误: 无效的 -n 参数" << std::endl; return 1; }
                break;
            case 'c':
                try { num_threads = std::stoi(optarg); }
                catch (const std::exception&) { std::cerr << "错误: 无效的 -c 参数" << std::endl; return 1; }
                break;
            case 'h':
            default:
                std::cout << "用法: " << argv[0] << " [-t 任务数] [-n 区间大小] [-c 线程数]\n" << std::endl;
                std::cout << "参数说明:" << std::endl;
                std::cout << "  -t <N>   任务总数 (默认: 20)" << std::endl;
                std::cout << "  -n <N>   区间大小，每任务计算的数字范围 (默认: 100000，最大: 100000)" << std::endl;
                std::cout << "  -c <N>   CPU核数/线程数 (默认: 4)" << std::endl;
                std::cout << "\n示例:" << std::endl;
                std::cout << "  " << argv[0] << " -t 100 -n 100000 -c 8    # 100任务, 每任务10万, 8核" << std::endl;
                std::cout << "  " << argv[0] << " -t 200 -n 50000 -c 16   # 200任务, 每任务5万, 16核" << std::endl;
                std::cout << "  " << argv[0] << " -t 320 -n 100000 -c 32  # 320任务, 每任务10万, 32核" << std::endl;
                return (opt == 'h') ? 0 : 1;
        }
    }

    // 参数校验
    if (num_tasks <= 0) [[unlikely]] num_tasks = 20;
    if (chunk_size <= 0) [[unlikely]] chunk_size = 100000;
    if (num_threads <= 0) [[unlikely]] num_threads = 4;

    // 1. 初始化任务队列
    g_task_queue = initTaskQueue(num_tasks, chunk_size, num_threads);

    // 2. 记录开始时间
    auto start_time = std::chrono::high_resolution_clock::now();

    // 3. 创建 libfork 线程池并执行并行计算
    lf::lazy_pool pool(static_cast<size_t>(num_threads));

    std::cout << "开始并行计算...\n" << std::endl;

    // 使用 libfork 的并行调度
    lf::sync_wait(pool, parallelCompute, num_threads);

    // 4. 记录结束时间
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // 5. 输出结果到CSV文件
    std::string output_file = "glm5_libfork_prime.csv";
    outputResults(output_file);

    // 6. 打印统计结果
    printStatistics(duration.count());

    // 7. 清理资源（unique_ptr 自动释放）
    g_task_queue.reset();

    return 0;
}
