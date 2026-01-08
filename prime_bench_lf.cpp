/**
 * @file prime_bench_lf.cpp
 * @brief 使用 libfork 框架的并行素数计算基准测试程序
 *
 * 该程序利用 libfork 的 fork-join 并行特性，高效计算指定范围内的素数。
 * 采用递归分治策略，自动实现工作窃取和负载均衡。
 */

#include <libfork/core.hpp>
#include <libfork/schedule.hpp>

#include <algorithm>
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
    int num_threads = static_cast<int>(std::thread::hardware_concurrency());
    int granularity = 100000;
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

inline constexpr auto parallel_prime_count =
    [](auto self, int start, int end, int granularity) -> lf::task<size_t> {
    int range = end - start;

    if (range <= granularity) {
        co_return count_primes_in_range(start, end);
    }

    int mid = start + range / 2;

    size_t left_count = 0;
    size_t right_count = 0;

    co_await lf::fork[&left_count, self](start, mid, granularity);
    co_await lf::call[&right_count, self](mid + 1, end, granularity);

    co_await lf::join;

    co_return left_count + right_count;
};

// 简单顺序计算（仅计数）
size_t sequential_prime_count(int max_number) {
    return count_primes_in_range(2, max_number);
}

// 原始 prime_bench 顺序计算方法（使用 vector 存储所有素数）
std::pair<size_t, long> sequential_prime_count_original(int max_number) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 使用相同的算法计算质数
    std::vector<int> primes;
    
    // 处理特殊情况：2是唯一的偶质数
    if (max_number >= 2) {
        primes.push_back(2);
    }

    // 从奇数开始检查，跳过所有偶数
    int actual_start = 3;

    for (int i = actual_start; i <= max_number; i += 2) {
        bool is_prime = true;
        int limit = static_cast<int>(std::sqrt(i));

        // 只检查奇数因子
        for (int j = 3; j <= limit; j += 2) {
            if (i % j == 0) {
                is_prime = false;
                break;
            }
        }
        if (is_prime)
            primes.push_back(i);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    return std::make_pair(primes.size(), duration.count());
}

void print_usage(const char* program_name) {
    std::cout << "用法: " << program_name << " [选项]\n"
              << "选项:\n"
              << "  -n, --max-number <N>    计算范围上限 (默认: 20000000)\n"
              << "  -t, --threads <N>       线程数 (默认: CPU核心数)\n"
              << "  -g, --granularity <N>   任务粒度 (默认: 100000)\n"
              << "  -v, --verbose           详细输出\n"
              << "  -h, --help              显示此帮助信息\n";
}

Config parse_args(int argc, char** argv) {
    Config config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if ((arg == "-n" || arg == "--max-number") && i + 1 < argc) {
            config.max_number = std::atoi(argv[++i]);
        } else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            config.num_threads = std::atoi(argv[++i]);
        } else if ((arg == "-g" || arg == "--granularity") && i + 1 < argc) {
            config.granularity = std::atoi(argv[++i]);
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        }
    }

    if (config.max_number <= 0) config.max_number = 20000000;
    if (config.num_threads <= 0)
        config.num_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (config.granularity <= 0) config.granularity = 100000;

    return config;
}

int main(int argc, char** argv) {
    Config config = parse_args(argc, argv);

    std::cout << "=== libfork 并行素数计算基准测试 ===\n"
              << "计算范围: [1, " << config.max_number << "]\n"
              << "线程数: " << config.num_threads << "\n"
              << "任务粒度: " << config.granularity << "\n"
              << std::endl;

    std::cout << "开始并行计算..." << std::endl;
    auto parallel_start = std::chrono::high_resolution_clock::now();

    lf::lazy_pool pool(static_cast<std::size_t>(config.num_threads));
    size_t parallel_primes =
        lf::sync_wait(pool, parallel_prime_count, 1, config.max_number, config.granularity);

    auto parallel_end = std::chrono::high_resolution_clock::now();
    auto parallel_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(parallel_end - parallel_start);

    std::cout << "\n=== 并行计算结果 ===" << std::endl;
    std::cout << "质数总数: " << parallel_primes << std::endl;
    std::cout << "并行计算耗时: " << parallel_duration.count() << "ms" << std::endl;

    // ============ 顺序计算（简单版，仅计数）============
    std::cout << "\n开始顺序计算（简单版）..." << std::endl;
    auto seq_start = std::chrono::high_resolution_clock::now();

    size_t seq_primes = sequential_prime_count(config.max_number);

    auto seq_end = std::chrono::high_resolution_clock::now();
    auto seq_duration = std::chrono::duration_cast<std::chrono::milliseconds>(seq_end - seq_start);

    std::cout << "\n=== 顺序计算结果（简单版）===" << std::endl;
    std::cout << "质数总数: " << seq_primes << std::endl;
    std::cout << "顺序计算耗时: " << seq_duration.count() << "ms" << std::endl;

    // ============ 顺序计算（原始版，使用vector存储）============
    std::cout << "\n开始顺序计算（原始版，vector存储）..." << std::endl;
    
    auto [seq_original_primes, seq_original_duration] = sequential_prime_count_original(config.max_number);

    std::cout << "\n=== 顺序计算结果（原始版）===" << std::endl;
    std::cout << "质数总数: " << seq_original_primes << std::endl;
    std::cout << "顺序计算耗时: " << seq_original_duration << "ms" << std::endl;

    std::cout << "\n" << std::string(50, '=') << std::endl;
    std::cout << "=== 性能比较结果 ===" << std::endl;
    std::cout << std::string(50, '=') << std::endl;
    
    // 结果一致性检查
    bool all_match = (parallel_primes == seq_primes) && (parallel_primes == seq_original_primes);
    std::cout << "结果一致性: " << (all_match ? "✓ 通过" : "✗ 失败") << std::endl;
    std::cout << "  - libfork 并行: " << parallel_primes << std::endl;
    std::cout << "  - 顺序(简单版): " << seq_primes << std::endl;
    std::cout << "  - 顺序(原始版): " << seq_original_primes << std::endl;

    std::cout << std::fixed << std::setprecision(2);
    
    if (parallel_duration.count() > 0) {
        std::cout << "\n--- 耗时对比 ---" << std::endl;
        std::cout << "libfork 并行:     " << std::setw(6) << parallel_duration.count() << "ms" << std::endl;
        std::cout << "顺序(简单版):     " << std::setw(6) << seq_duration.count() << "ms" << std::endl;
        std::cout << "顺序(原始版):     " << std::setw(6) << seq_original_duration << "ms" << std::endl;
        
        std::cout << "\n--- 加速比 ---" << std::endl;
        double speedup_simple = static_cast<double>(seq_duration.count()) /
                                static_cast<double>(parallel_duration.count());
        double speedup_original = static_cast<double>(seq_original_duration) /
                                  static_cast<double>(parallel_duration.count());
        
        std::cout << "vs 顺序(简单版): " << speedup_simple << "x" << std::endl;
        std::cout << "vs 顺序(原始版): " << speedup_original << "x" << std::endl;
        
        std::cout << "\n并行计算比顺序计算(简单版)快 " << speedup_simple << " 倍" << std::endl;
        std::cout << "并行计算比顺序计算(原始版)快 " << speedup_original << " 倍" << std::endl;

        if (config.verbose) {
            double numbers_per_ms =
                static_cast<double>(config.max_number) / static_cast<double>(parallel_duration.count());
            double primes_per_ms =
                static_cast<double>(parallel_primes) / static_cast<double>(parallel_duration.count());
            
            std::cout << "\n=== 详细性能指标 ===" << std::endl;
            std::cout << "计算性能: " << numbers_per_ms << " 个数字/毫秒" << std::endl;
            std::cout << "素数发现率: " << primes_per_ms << " 个素数/毫秒" << std::endl;
            std::cout << "素数密度: "
                      << (static_cast<double>(parallel_primes) /
                          static_cast<double>(config.max_number) * 100.0)
                      << "%" << std::endl;
        }
    }

    std::cout << "\n测试完成!" << std::endl;
    return 0;
}
