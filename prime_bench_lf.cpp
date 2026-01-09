/**
 * @file prime_bench_lf.cpp
 * @brief 使用 libfork 协程的并行素数计算基准测试程序
 */

#include <libfork/core.hpp>
#include <libfork/schedule.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace lf = ::lf;

struct Config {
    int max_number = 20000000;
    int num_tasks = 4;  // 任务数（默认4个任务）
    int chunk_size = 5000000;  // 每个任务的区间大小（默认500万）
    int num_threads = static_cast<int>(std::thread::hardware_concurrency());  // 线程数（默认CPU核心数）
    bool verbose = false;
};

inline size_t count_primes_in_range(int start, int end) {
    if (start > end) return 0;
    if (start < 2) start = 2;

    size_t count = 0;

    if (start <= 2 && end >= 2) {
        count++;
    }

    int actual_start = (start % 2 == 0) ? start + 1 : start;
    if (actual_start < 3) actual_start = 3;

    for (int i = actual_start; i <= end; i += 2) {
        bool is_prime = true;
        int limit = static_cast<int>(std::sqrt(static_cast<double>(i)));

        for (int j = 3; j <= limit; j += 2) {
            if (i % j == 0) {
                is_prime = false;
                break;
            }
        }
        if (is_prime) count++;
    }
    return count;
}

// 简单顺序计算（仅计数）
size_t sequential_prime_count(int max_number) {
    return count_primes_in_range(2, max_number);
}

// libfork 并行计算 - 使用 fork-join 模式处理任务
inline constexpr auto parallel_prime_count =
    [](auto self, int start_task, int end_task, int chunk_size) -> lf::task<size_t> {
    int num_tasks = end_task - start_task;
    
    if (num_tasks == 0) {
        co_return 0;
    }
    
    if (num_tasks == 1) {
        // 只有一个任务，直接计算
        int start = start_task * chunk_size + 1;
        int end = (start_task + 1) * chunk_size;
        co_return count_primes_in_range(start, end);
    }
    
    // 多个任务，分割为两部分
    int mid = start_task + num_tasks / 2;
    
    size_t left_count = 0;
    size_t right_count = 0;
    
    // 并行执行左右两部分
    co_await lf::fork[&left_count, self](start_task, mid, chunk_size);
    co_await lf::call[&right_count, self](mid, end_task, chunk_size);
    
    co_await lf::join;
    
    co_return left_count + right_count;
};

void print_usage(const char* program_name) {
    std::cout << "用法: " << program_name << " [选项]\n"
              << "选项:\n"
              << "  -n, --chunk-size <N>    每个任务的区间大小 (默认: 5000000)\n"
              << "  -t, --tasks <N>         任务数 (默认: 4)\n"
              << "  -p, --threads <N>       工作线程数 (默认: CPU核心数)\n"
              << "  -v, --verbose           详细输出\n"
              << "  -h, --help              显示此帮助信息\n"
              << "\n说明:\n"
              << "  总计算上限 = 任务数 × 区间大小\n"
              << "  例如: -t 8 -n 2500000 将计算 8 × 2500000 = 20000000 以内的素数\n";
}

Config parse_args(int argc, char** argv) {
    Config config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if ((arg == "-n" || arg == "--chunk-size") && i + 1 < argc) {
            config.chunk_size = std::atoi(argv[++i]);
        } else if ((arg == "-t" || arg == "--tasks") && i + 1 < argc) {
            config.num_tasks = std::atoi(argv[++i]);
        } else if ((arg == "-p" || arg == "--threads") && i + 1 < argc) {
            config.num_threads = std::atoi(argv[++i]);
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        }
    }

    if (config.num_tasks <= 0) config.num_tasks = 4;
    if (config.chunk_size <= 0) config.chunk_size = 5000000;
    if (config.num_threads <= 0)
        config.num_threads = static_cast<int>(std::thread::hardware_concurrency());
    
    // 计算总范围
    config.max_number = config.num_tasks * config.chunk_size;

    return config;
}

int main(int argc, char** argv) {
    Config config = parse_args(argc, argv);

    std::cout << "=== 并行素数计算基准测试 ===" << std::endl;
    std::cout << "计算范围: [1, " << config.max_number << "]" << std::endl;
    std::cout << "任务数: " << config.num_tasks << std::endl;
    std::cout << "区间大小: " << config.chunk_size << std::endl;
    std::cout << "工作线程数: " << config.num_threads << std::endl;
    std::cout << "" << std::endl;

    // 并行计算（使用 libfork 协程）
    std::cout << "开始并行计算..." << std::endl;
    auto parallel_start = std::chrono::high_resolution_clock::now();

    lf::lazy_pool pool(static_cast<std::size_t>(config.num_threads));
    size_t parallel_primes = lf::sync_wait(pool, parallel_prime_count, 0, config.num_tasks, config.chunk_size);

    auto parallel_end = std::chrono::high_resolution_clock::now();
    auto parallel_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(parallel_end - parallel_start);

    std::cout << "=== 并行计算结果 ===" << std::endl;
    std::cout << "质数总数: " << parallel_primes << std::endl;
    std::cout << "计算耗时: " << parallel_duration.count() << "ms" << std::endl;

    // 顺序计算
    std::cout << "" << std::endl;
    std::cout << "开始顺序计算..." << std::endl;
    auto seq_start = std::chrono::high_resolution_clock::now();

    size_t seq_primes = sequential_prime_count(config.max_number);

    auto seq_end = std::chrono::high_resolution_clock::now();
    auto seq_duration = std::chrono::duration_cast<std::chrono::milliseconds>(seq_end - seq_start);

    std::cout << "=== 顺序计算结果 ===" << std::endl;
    std::cout << "质数总数: " << seq_primes << std::endl;
    std::cout << "计算耗时: " << seq_duration.count() << "ms" << std::endl;

    // 性能比较
    std::cout << "" << std::endl;
    std::cout << "=== 性能比较结果 ===" << std::endl;
    std::cout << "计算范围: [1, " << config.max_number << "]" << std::endl;
    std::cout << "质数总数（并行）: " << parallel_primes << std::endl;
    std::cout << "质数总数（顺序）: " << seq_primes << std::endl;
    std::cout << "结果一致性: " << (parallel_primes == seq_primes ? "通过" : "失败") << std::endl;
    std::cout << "" << std::endl;
    std::cout << "并行计算耗时: " << parallel_duration.count() << "ms" << std::endl;
    std::cout << "顺序计算耗时: " << seq_duration.count() << "ms" << std::endl;
    std::cout << "" << std::endl;

    if (seq_duration.count() > 0 && parallel_duration.count() > 0) {
        double speedup = static_cast<double>(seq_duration.count()) /
                        static_cast<double>(parallel_duration.count());
        std::cout << "加速比（顺序/并行）: " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
        
        if (speedup > 1.0) {
            std::cout << "并行计算比顺序计算快 " << speedup << " 倍" << std::endl;
        } else if (speedup < 1.0) {
            std::cout << "顺序计算比并行计算快 " << 1.0 / speedup << " 倍" << std::endl;
        } else {
            std::cout << "两种方法性能相同" << std::endl;
        }
    }

    return 0;
}
