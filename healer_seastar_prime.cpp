// healer_seastar_prime: 并行素数计算程序
// 编译: make healer_seastar_prime
// 运行: ./healer_seastar_prime -t 100 -n 100000 -c 4

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/future.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/log.hh>
#include <boost/program_options.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <thread>

namespace bpo = boost::program_options;

// Seastar 日志器
static seastar::logger applog("healer_prime");

// ═══════════════════════════════════════════════════════════════════
// 全局配置参数（只读，跨核心安全）
// ═══════════════════════════════════════════════════════════════════
static int64_t g_max_number    = 2000000000LL;
static int     g_chunk_size    = 100000;
static int     g_num_workers   = 20;
static std::string g_csv_path  = "healer_seastar_prime.csv";

// 原子变量（跨核心安全）
static std::atomic<int64_t> g_total_primes{0};
static std::atomic<int64_t> g_tasks_completed{0};
static int g_total_tasks = 0;

// 每核心的结果缓冲区
thread_local std::string t_output_buffer;

// ═══════════════════════════════════════════════════════════════════
// 试除法判定素数（6k±1 优化）
// ═══════════════════════════════════════════════════════════════════
bool is_prime(int64_t n) {
    if (n < 2)          return false;
    if (n == 2 || n == 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (int64_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// 计算区间内的素数
// ═══════════════════════════════════════════════════════════════════
std::vector<int64_t> find_primes_in_range(int64_t start, int64_t end) {
    std::vector<int64_t> primes;
    primes.reserve((end - start) / 10);
    
    // 跳过偶数优化
    if (start <= 2) {
        primes.push_back(2);
        start = 3;
    }
    if (start % 2 == 0) start++;
    
    for (int64_t num = start; num <= end; num += 2) {
        if (is_prime(num)) {
            primes.push_back(num);
        }
    }
    return primes;
}

// ═══════════════════════════════════════════════════════════════════
// 工作线程：处理分配的任务列表
// ═══════════════════════════════════════════════════════════════════
seastar::future<> worker_loop(std::vector<std::tuple<int, int64_t, int64_t>> my_tasks, unsigned core_id) {
    size_t task_idx = 0;
    t_output_buffer.clear();
    t_output_buffer.reserve(1024 * 1024);
    
    return seastar::repeat([my_tasks = std::move(my_tasks), &task_idx, core_id]() mutable {
        if (task_idx >= my_tasks.size()) {
            return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::yes);
        }
        
        auto [task_id, start, end] = my_tasks[task_idx++];
        
        // CPU 密集计算
        auto primes = find_primes_in_range(start, end);
        
        // 更新统计
        g_total_primes.fetch_add(static_cast<int64_t>(primes.size()), std::memory_order_relaxed);
        int64_t done = g_tasks_completed.fetch_add(1, std::memory_order_relaxed) + 1;
        
        // 构建输出行
        std::ostringstream oss;
        oss << start << "-" << end << "," << core_id;
        for (auto p : primes) {
            oss << "," << p;
        }
        oss << "\n";
        t_output_buffer += oss.str();
        
        // 进度报告
        if (done % 2000 == 0 || done == g_total_tasks) {
            std::cout << "[进度] 核 " << std::setw(2) << core_id 
                      << " | 已完成 " << std::setw(6) << done 
                      << " / " << g_total_tasks
                      << " 任务 | 本区间素数 " << primes.size() << " 个" << std::endl;
        }
        
        return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::no);
    }).then([core_id] {
        // 将结果写入文件
        std::ofstream outfile(g_csv_path, std::ios::out | std::ios::app);
        if (outfile.is_open()) {
            outfile << t_output_buffer;
            outfile.flush();
        }
        return seastar::make_ready_future<>();
    });
}

// ═══════════════════════════════════════════════════════════════════
// 主函数
// ═══════════════════════════════════════════════════════════════════
seastar::future<> seastar_main(const bpo::variables_map& config) {
    // 读取配置
    int num_tasks = config["tasks"].as<int>();
    g_chunk_size = config["chunk-size"].as<int>();
    g_csv_path = config["output"].as<std::string>();
    std::string log_level_str = config["log-level"].as<std::string>();
    
    // 设置日志级别
    if (log_level_str == "debug") {
        applog.set_level(seastar::log_level::debug);
    } else if (log_level_str == "info") {
        applog.set_level(seastar::log_level::info);
    } else if (log_level_str == "error") {
        applog.set_level(seastar::log_level::error);
    } else if (log_level_str == "trace") {
        applog.set_level(seastar::log_level::trace);
    } else {
        applog.set_level(seastar::log_level::error);
    }
    
    // 计算最大数值：任务数 × 区间大小
    g_max_number = static_cast<int64_t>(num_tasks) * g_chunk_size;
    g_total_tasks = num_tasks;
    
    g_num_workers = static_cast<int>(seastar::smp::count);
    
    applog.info("启动配置: 任务数={}, 区间大小={}, 输出文件={}, 日志级别={}, 核心数={}", 
                num_tasks, g_chunk_size, g_csv_path, log_level_str, g_num_workers);

    // 初始化输出文件
    {
        std::ofstream outfile(g_csv_path, std::ios::out | std::ios::trunc);
        if (outfile.is_open()) {
            outfile << "\xEF\xBB\xBF";  // UTF-8 BOM
            outfile << "range,core_id,primes...\n";
            outfile.flush();
        }
    }

    std::cout << "============================================" << std::endl;
    std::cout << "  [初始化] 任务队列已创建" << std::endl;
    std::cout << "  [初始化] 总任务数:   " << num_tasks << std::endl;
    std::cout << "  [初始化] 区间大小:   " << g_chunk_size << std::endl;
    std::cout << "  [初始化] 计算范围:   [2, " << g_max_number << "]" << std::endl;
    std::cout << "  [初始化] CPU 核数:   " << g_num_workers << std::endl;
    std::cout << "============================================" << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();

    // 为每个核心预分配任务列表
    unsigned num_cores = g_num_workers;
    std::vector<std::vector<std::tuple<int, int64_t, int64_t>>> core_task_lists(num_cores);
    
    for (int task_id = 0; task_id < num_tasks; ++task_id) {
        // 与其他 benchmark 程序保持一致
        int64_t start = static_cast<int64_t>(task_id) * g_chunk_size + 2;
        int64_t end = std::min(static_cast<int64_t>(task_id + 1) * g_chunk_size, g_max_number);
        unsigned target_core = task_id % num_cores;
        core_task_lists[target_core].push_back({task_id, start, end});
    }

    // 启动工作线程
    std::cout << "[调度] 使用 submit_to 将 " << g_num_workers 
              << " 个工作线程分配到各 CPU 核" << std::endl;

    std::vector<seastar::future<>> futures;
    for (unsigned core = 0; core < num_cores; ++core) {
        if (core_task_lists[core].empty()) continue;
        
        futures.push_back(
            seastar::smp::submit_to(core, [my_tasks = std::move(core_task_lists[core]), core] {
                return seastar::async([my_tasks = std::move(my_tasks), core] {
                    worker_loop(std::move(my_tasks), core).get();
                });
            })
        );
    }

    return seastar::when_all(futures.begin(), futures.end()).discard_result().then(
        [start_time] {
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

            std::cout << "\n";
            std::cout << "╔════════════════════════════════════════╗\n";
            std::cout << "║           计 算 完 成 汇 总            ║\n";
            std::cout << "╠════════════════════════════════════════╣\n";
            std::cout << "║  总任务数:       " << std::setw(12) 
                      << g_tasks_completed.load() << "         ║\n";
            std::cout << "║  素数总数:       " << std::setw(12) 
                      << g_total_primes.load() << "         ║\n";
            std::cout << "║  计算耗时:       " << std::setw(12) 
                      << duration.count() << " ms      ║\n";
            std::cout << "║  CPU 核数:       " << std::setw(12) 
                      << g_num_workers << "         ║\n";
            std::cout << "║  区间大小:       " << std::setw(12) 
                      << g_chunk_size << "         ║\n";
            std::cout << "║  输出文件:       " << std::setw(19) 
                      << std::left << g_csv_path << "║\n";
            std::cout << "╚════════════════════════════════════════╝\n";
            
            applog.info("程序执行完成，总耗时: {} 毫秒", duration.count());
        });
}

int main(int argc, char** argv) {
    seastar::app_template app;

    app.add_options()
        ("tasks,t", bpo::value<int>()->default_value(100), "任务数（总区间数）")
        ("chunk-size,n", bpo::value<int>()->default_value(100000), "每个任务区间的大小")
        ("output,o", bpo::value<std::string>()->default_value("healer_seastar_prime.csv"), "输出CSV文件路径")
        ("log-level,l", bpo::value<std::string>()->default_value("error"), "日志级别 (debug/info/error/trace)");

    return app.run(argc, argv, [&app] {
        return seastar_main(app.configuration());
    });
}
