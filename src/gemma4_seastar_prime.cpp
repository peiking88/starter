#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <seastar/core.hh>
#include <seastar/slog.hh>
#include <seastar/file.hh>
#include <seastar/smp.hh>

// --- 参数化配置 ---
const uint64_t INTERVAL_SIZE = 100000; // 每个区间大小
const uint32_t TOTAL_TASKS = 100;      // 任务总数 (总范围 = 100 * 100,000 = 10,000,000)

// 素数判断函数
bool is_prime(uint64_t n) {
    if (n < 2) return false;
    if (n == 2 || n == 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (uint64_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return false;
    }
    return true;
}

// 核心计算逻辑：在特定核心上执行
seastar::future<std::string> compute_primes_task(uint64_t start, uint64_t end, int core_id) {
    return seastar::make_ready_future<std::string>([start, end, core_id]() {
        std::stringstream ss;
        ss << start << "-" << end << "," << core_id;
        
        for (uint64_t i = start; i <= end; ++i) {
            if (is_prime(i)) {
                ss << "," << i;
            }
        }
        return ss.str();
    });
}

// 任务初始化与分发函数
seastar::future<std::vector<std::string>> initialize_tasks() {
    uint32_t smp_count = seastar::smp::get_smp_count();
    std::vector<seastar::future<std::string>> futures;

    for (uint32_t i = 0; i < TOTAL_TASKS; ++i) {
        uint64_t start = (uint64_t)i * INTERVAL_SIZE + 1;
        uint64_t end = (uint64_t)(i + 1) * INTERVAL_SIZE;
        
        // 动态分配：通过取模将任务均匀分布到各个 CPU 核心
        int target_core = i % smp_count;

        // 使用 submit_to 将任务提交到指定核的队列中执行
        auto f = seastar::submit_to(target_core, [start, end, target_core]() {
            return compute_primes_task(start, end, target_core);
        });
        futures.push_back(std::move(f));
    }

    // 等待所有核上的任务完成，并收集结果
    return seastar::collect_all(std::move(futures));
}

// 结果输出函数
seastar::future<void> write_results(std::vector<std::string> results) {
    return seastar::file::open("primes.csv", seastar::open_flags::create | seastar::open_flags::truncate)
        .then([results = std::move(results)](seastar::file handle) {
            std::string final_content;
            for (const auto& line : results) {
                final_content += line + "\n";
            }
            return handle.write(final_content).then([handle = std::move(handle)]() {
                return handle.close();
            });
        };
    }

// 主入口
int main(int argc, char** argv) {
    seastar::app app;
    
    // 设置日志级别为 error
    seastar::app_options options;
    options.log_level = seastar::slog::level::error;
    app.run(argc, argv, options);

    // 任务链路：初始化分发 -> 收集结果 -> 写入文件
    return initialize_tasks().then([](std::vector<std::string> results) {
        return write_results(std::move(results));
    }).get();
}