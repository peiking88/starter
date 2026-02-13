// sequence_prime: 顺序素数计算程序

#include <iostream>
#include <fstream>
#include <vector>
#include <atomic>
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
    int num_tasks = 1;       // 任务总数（顺序执行，默认为1）
    int chunk_size = 100000;  // 每个任务的区间大小
    int num_threads = 1;      // 使用线程数（顺序执行，默认为1）
};

Config g_config;

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
std::vector<TaskResult> g_results;
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
    primes.reserve(static_cast<size_t>((end - start) / 20));

    for (uint64_t n = start; n <= end; ++n) {
        if (isPrime(n)) {
            primes.push_back(n);
        }
    }
    return primes;
}

// ============================================================================
// 功能函数：初始化任务队列
// ============================================================================
void initTaskQueue(int num_tasks, int chunk_size, int num_threads) {
    // 设置全局配置
    g_config.num_tasks = num_tasks;
    g_config.chunk_size = chunk_size;
    g_config.num_threads = num_threads;
    
    // 重置统计
    g_completed_tasks.store(0);
    g_total_primes.store(0);
    g_results.clear();
    
    // 打印初始化信息
    std::cout << "\n========================================" << std::endl;
    std::cout << "       任务队列初始化完成" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "总任务数: " << num_tasks << std::endl;
    std::cout << "工作线程: " << num_threads << std::endl;
    std::cout << "========================================\n" << std::endl;
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
// 顺序计算素数
// ============================================================================
void sequentialCompute() {
    // 顺序执行所有任务
    for (int task_id = 0; task_id < g_config.num_tasks; ++task_id) {
        // 计算任务区间（从2开始，1不是素数）
        uint64_t start = static_cast<uint64_t>(task_id) * g_config.chunk_size + 2;
        uint64_t end = static_cast<uint64_t>(task_id + 1) * g_config.chunk_size;
        
        // 计算该区间的素数
        std::vector<uint64_t> primes = computePrimesInRange(start, end);
        size_t count = primes.size();
        
        // 收集结果
        TaskResult result;
        result.task_id = task_id;
        result.start = start;
        result.end = end;
        result.core_id = 0;  // 顺序执行，始终使用核心0
        result.primes = std::move(primes);
        g_results.push_back(std::move(result));
        
        // 更新统计
        g_completed_tasks.fetch_add(1);
        g_total_primes.fetch_add(count);
        
        // 打印进度（在一行动态更新）
        int completed = g_completed_tasks.load();
        if (completed % 10 == 0 || completed == g_config.num_tasks) {
            double progress = 100.0 * completed / g_config.num_tasks;
            std::cout << "\r进度: " << std::fixed << std::setprecision(1)
                      << progress << "% (" << completed << "/" << g_config.num_tasks 
                      << " 任务, 素数: " << g_total_primes.load() << ")" << std::flush;
        }
    }
}

// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char** argv) {
    // 默认参数
    int num_tasks = 1;
    int chunk_size = 100000;
    int num_threads = 1;
    
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
                std::cout << "  -t <N>   任务总数 (默认: 1)" << std::endl;
                std::cout << "  -n <N>   区间大小，每任务计算的数字范围 (默认: 100000，最大: 100000)" << std::endl;
                std::cout << "  -c <N>   线程数 (默认: 1，顺序执行)" << std::endl;
                std::cout << "\n示例:" << std::endl;
                std::cout << "  " << argv[0] << " -t 1 -n 100000 -c 1  # 顺序计算 2-100000" << std::endl;
                std::cout << "  " << argv[0] << " -t 10 -n 100000 -c 1  # 顺序计算 2-1000000" << std::endl;
                return (opt == 'h') ? 0 : 1;
        }
    }
    
    // 参数校验
    if (num_tasks <= 0) num_tasks = 1;
    if (chunk_size <= 0) chunk_size = 100000;
    if (num_threads <= 0) num_threads = 1;
    
    // 1. 初始化任务队列
    initTaskQueue(num_tasks, chunk_size, num_threads);
    
    // 2. 记录开始时间
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::cout << "开始顺序计算...\n" << std::endl;
    std::cout << std::flush;
    
    // 3. 顺序执行计算
    sequentialCompute();
    
    std::cout << std::endl;  // 结束进度条行
    
    // 4. 记录结束时间
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // 5. 输出结果到CSV文件
    std::string output_file = "primes_" + std::to_string(num_tasks) + "_" + 
                               std::to_string(chunk_size) + ".csv";
    outputResults(output_file);
    
    // 6. 打印统计结果
    printStatistics(duration.count());
    
    return 0;
}
