// minimax_prime: 并行素数计算程序 - 使用 libfork 框架
// 工作模式：共享任务队列 + 工作窃取，动态负载均衡

#include <libfork/core.hpp>
#include <libfork/schedule.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <string>
#include <optional>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <getopt.h>

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
        int task_id = next_task_id_.fetch_add(1);
        if (task_id >= total_tasks_) {
            return std::nullopt;
        }
        return task_id;
    }

    void reset() { next_task_id_.store(0); }
};

// ============================================================================
// 任务结果结构
// ============================================================================
struct TaskResult {
    int task_id;
    uint64_t start;
    uint64_t end;
    int core_id;
    std::vector<uint64_t> primes;
};

// ============================================================================
// 全局状态
// ============================================================================
TaskQueue* g_task_queue = nullptr;
std::vector<TaskResult> g_results;
std::mutex g_results_mutex;
std::atomic<int> g_completed_tasks{0};
std::atomic<uint64_t> g_total_primes{0};

// ============================================================================
// 素数计算函数
// ============================================================================

// 优化的素数检查函数（6k±1优化）
inline bool isPrime(uint64_t n) {
    if (n < 2) return false;
    if (n == 2 || n == 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;

    uint64_t sqrt_n = static_cast<uint64_t>(std::sqrt(static_cast<double>(n)));
    for (uint64_t i = 5; i <= sqrt_n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) {
            return false;
        }
    }
    return true;
}

// 计算区间内的所有素数
std::vector<uint64_t> computePrimesInRange(uint64_t start, uint64_t end) {
    std::vector<uint64_t> primes;
    // 素数密度约 1/ln(n)，预分配空间
    primes.reserve(static_cast<size_t>((end - start) / 20));

    for (uint64_t n = start; n <= end; ++n) {
        if (isPrime(n)) {
            primes.push_back(n);
        }
    }
    return primes;
}

// ============================================================================
// libfork 并行任务 - 工作窃取模式
// ============================================================================

// 工作协程：不断从队列获取任务并执行
// 使用递归模式：每个worker处理完一个任务后，如果还有任务就继续
inline constexpr auto workerTask = 
    [](auto self, int core_id) -> lf::task<void> {
    
    // 从共享队列获取下一个任务（工作窃取）
    auto task_opt = g_task_queue->getNextTask();
    
    if (!task_opt) {
        // 没有更多任务，退出
        co_return;
    }
    
    int task_id = *task_opt;
    
    // 计算任务区间（从2开始，1不是素数）
    uint64_t start = static_cast<uint64_t>(task_id) * g_config.chunk_size + 2;
    uint64_t end = static_cast<uint64_t>(task_id + 1) * g_config.chunk_size;
    
    // 计算该区间的素数
    std::vector<uint64_t> primes = computePrimesInRange(start, end);
    size_t count = primes.size();
    
    // 收集结果
    {
        std::lock_guard<std::mutex> lock(g_results_mutex);
        TaskResult result;
        result.task_id = task_id;
        result.start = start;
        result.end = end;
        result.core_id = core_id;
        result.primes = std::move(primes);
        g_results.push_back(std::move(result));
    }
    
    // 更新统计
    g_completed_tasks.fetch_add(1);
    g_total_primes.fetch_add(count);
    
    // 打印进度
    int completed = g_completed_tasks.load();
    if (completed % 20 == 0 || completed == g_config.num_tasks) {
        double progress = 100.0 * completed / g_config.num_tasks;
        std::cout << "\r进度: " << std::fixed << std::setprecision(1) 
                  << progress << "% (" << completed << "/" << g_config.num_tasks 
                  << " 任务, 素数: " << g_total_primes.load() << ")" << std::flush;
    }
    
    // 递归调用自己，继续处理下一个任务
    co_await lf::call[self](core_id);
};

// 并行启动多个worker
inline constexpr auto parallelCompute = 
    [](auto self, int remaining_workers) -> lf::task<void> {
    
    if (remaining_workers <= 0) {
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
TaskQueue* initTaskQueue(int num_tasks, int chunk_size, int num_threads) {
    // 设置全局配置
    g_config.num_tasks = num_tasks;
    g_config.chunk_size = chunk_size;
    g_config.num_threads = num_threads;
    
    // 重置统计
    g_completed_tasks.store(0);
    g_total_primes.store(0);
    g_results.clear();
    
    // 创建任务队列
    TaskQueue* queue = new TaskQueue(num_tasks);
    
    // 打印初始化信息
    std::cout << "\n========================================" << std::endl;
    std::cout << "       任务队列初始化完成" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "总任务数: " << num_tasks << std::endl;
    std::cout << "工作线程: " << num_threads << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    return queue;
}

// ============================================================================
// 功能函数：输出计算结果到CSV文件
// ============================================================================
void outputResults(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "错误: 无法打开输出文件 " << filename << std::endl;
        return;
    }
    
    std::cout << "\n正在写入结果文件: " << filename << std::endl;
    
    // 按任务ID排序输出
    std::sort(g_results.begin(), g_results.end(), 
              [](const TaskResult& a, const TaskResult& b) {
                  return a.task_id < b.task_id;
              });
    
    // 写入CSV
    for (const auto& result : g_results) {
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
void printStatistics(long duration_ms) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "         计算结果统计" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "已完成任务: " << g_completed_tasks.load() << "/" << g_config.num_tasks << std::endl;
    std::cout << "素数总数:   " << g_total_primes.load() << std::endl;
    std::cout << "计算耗时:   " << duration_ms << " ms" << std::endl;
    
    uint64_t total_numbers = static_cast<uint64_t>(g_config.num_tasks) * g_config.chunk_size;
    double prime_density = 100.0 * g_total_primes.load() / total_numbers;
    
    std::cout << "素数密度:   " << std::fixed << std::setprecision(4) << prime_density << "%" << std::endl;
    std::cout << "计算速度:   " << std::fixed << std::setprecision(0) 
              << static_cast<double>(total_numbers) / duration_ms << " 数/毫秒" << std::endl;
    std::cout << "素数发现率: " << std::fixed << std::setprecision(2) 
              << static_cast<double>(g_total_primes.load()) / duration_ms << " 素数/毫秒" << std::endl;
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
                num_tasks = std::atoi(optarg);
                break;
            case 'n':
                chunk_size = std::atoi(optarg);
                break;
            case 'c':
                num_threads = std::atoi(optarg);
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
    if (num_tasks <= 0) num_tasks = 20;
    if (chunk_size <= 0) chunk_size = 100000;
    if (num_threads <= 0) num_threads = 4;
    
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
    std::string output_file = "primes_" + std::to_string(num_tasks) + "_" + 
                               std::to_string(chunk_size) + ".csv";
    outputResults(output_file);
    
    // 6. 打印统计结果
    printStatistics(duration.count());
    
    // 7. 清理资源
    delete g_task_queue;
    g_task_queue = nullptr;
    
    return 0;
}
