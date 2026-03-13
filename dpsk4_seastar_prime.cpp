// dpsk4_seastar_prime: 并行素数计算程序 - 使用 Seastar 框架
// 工作模式：静态任务分配，每个核心处理固定任务

#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/future.hh>
#include <seastar/core/file.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/log.hh>
#include <boost/program_options.hpp>

#include <vector>
#include <iostream>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <mutex>

namespace po = boost::program_options;
using namespace seastar;

// Seastar 日志器
static seastar::logger applog("dpsk4_prime");

// 全局配置
int g_num_tasks = 20;
int g_chunk_size = 100000;

// 任务结果结构
struct TaskResult {
    int task_id;
    uint64_t start;
    uint64_t end;
    unsigned core_id;
    std::vector<uint64_t> primes;
};

// 全局结果存储
std::vector<TaskResult> g_results;
std::mutex g_results_mutex;

// 原子计数器
std::atomic<int> g_completed_tasks{0};
std::atomic<uint64_t> g_total_primes{0};

// 判断是否为素数
bool is_prime(uint64_t n) {
    if (n <= 1) return false;
    if (n <= 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    
    for (uint64_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) {
            return false;
        }
    }
    return true;
}

// 计算区间内的素数
std::vector<uint64_t> find_primes_in_range(uint64_t start, uint64_t end) {
    std::vector<uint64_t> primes;
    primes.reserve((end - start) / 20);
    
    if (start <= 2) {
        primes.push_back(2);
        start = 3;
    } else if (start % 2 == 0) {
        start++;
    }
    
    for (uint64_t num = start; num <= end; num += 2) {
        if (is_prime(num)) {
            primes.push_back(num);
        }
    }
    
    return primes;
}

// 初始化全局状态
void initialize_global_state(int num_tasks, int chunk_size) {
    g_num_tasks = num_tasks;
    g_chunk_size = chunk_size;
    g_completed_tasks.store(0);
    g_total_primes.store(0);
    g_results.clear();
    g_results.reserve(num_tasks);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "任务队列初始化完成" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "总任务数: " << num_tasks << std::endl;
    std::cout << "CPU核心数: " << seastar::smp::count << std::endl;
    std::cout << "========================================\n" << std::endl;
}

// 异步输出结果到CSV文件
seastar::future<> output_results(const std::string& filename) {
    std::cout << "\n正在写入结果文件: " << filename << std::endl;
    
    std::sort(g_results.begin(), g_results.end(), 
              [](const TaskResult& a, const TaskResult& b) {
                  return a.task_id < b.task_id;
              });
    
    std::string content;
    content.reserve(g_results.size() * 100);
    
    for (const auto& result : g_results) {
        content += std::to_string(result.start) + "-" + std::to_string(result.end);
        content += "," + std::to_string(result.core_id);
        for (uint64_t prime : result.primes) {
            content += "," + std::to_string(prime);
        }
        content += "\n";
    }
    
    return seastar::open_file_dma(filename, seastar::open_flags::wo | seastar::open_flags::create | seastar::open_flags::truncate).then(
        [content = std::move(content), filename](seastar::file f) {
            return seastar::do_with(std::move(f), std::move(content), [filename](seastar::file& f, const std::string& content) {
                return f.dma_write(0, content.data(), content.size()).then(
                    [&f, filename, size = content.size()](size_t written) {
                        if (written != size) {
                            applog.error("写入不完整: {} / {}", written, size);
                        }
                        std::cout << "结果已写入: " << filename << std::endl;
                        return f.close();
                    });
            });
        });
}

// 在核心上使用 repeat 循环处理任务（任务列表通过值传递）
seastar::future<> worker_loop(std::vector<std::tuple<int, uint64_t, uint64_t>> my_tasks, unsigned core_id) {
    size_t task_idx = 0;
    size_t total_tasks = my_tasks.size();
    
    return seastar::repeat([my_tasks = std::move(my_tasks), core_id, total_tasks, task_idx]() mutable {
        if (task_idx >= total_tasks) {
            return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::yes);
        }
        
        auto [task_id, start, end] = my_tasks[task_idx];
        task_idx++;
        
        // 使用 seastar::async 执行 CPU 密集型计算
        return seastar::async([start, end] {
            return find_primes_in_range(start, end);
        }).then([task_id, start, end, core_id](std::vector<uint64_t> primes) {
            size_t count = primes.size();
            
            int completed = g_completed_tasks.fetch_add(1) + 1;
            g_total_primes.fetch_add(count);
            
            if (completed % 10 == 0 || completed == g_num_tasks) {
                double progress = 100.0 * completed / g_num_tasks;
                std::cout << "\r进度: " << std::fixed << std::setprecision(1)
                          << progress << "% (" << completed << "/" << g_num_tasks 
                          << " 任务, 素数: " << g_total_primes.load() << ")" << std::flush;
            }
            
            {
                std::lock_guard<std::mutex> lock(g_results_mutex);
                g_results.push_back(TaskResult{task_id, start, end, core_id, std::move(primes)});
            }
            
            return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::no);
        });
    });
}

// 打印统计结果
void print_statistics(long duration_ms) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "         计算结果统计" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "已完成任务: " << g_completed_tasks.load() << "/" << g_num_tasks << std::endl;
    std::cout << "素数总数:   " << g_total_primes.load() << std::endl;
    std::cout << "计算耗时:   " << duration_ms << " ms" << std::endl;
    
    uint64_t total_numbers = static_cast<uint64_t>(g_num_tasks) * g_chunk_size;
    double prime_density = 100.0 * g_total_primes.load() / total_numbers;
    
    std::cout << "素数密度:   " << std::fixed << std::setprecision(4) << prime_density << "%" << std::endl;
    std::cout << "计算速度:   " << std::fixed << std::setprecision(0) 
              << static_cast<double>(total_numbers) / duration_ms << " 数/毫秒" << std::endl;
    std::cout << "========================================" << std::endl;
}

// Seastar 主函数
seastar::future<> seastar_main(const po::variables_map& config) {
    applog.set_level(seastar::log_level::error);
    
    int num_tasks = config["tasks"].as<int>();
    int chunk_size = config["chunk"].as<int>();
    
    if (config.count("log-level")) {
        std::string level = config["log-level"].as<std::string>();
        if (level == "debug") applog.set_level(seastar::log_level::debug);
        else if (level == "info") applog.set_level(seastar::log_level::info);
        else if (level == "trace") applog.set_level(seastar::log_level::trace);
    }
    
    if (num_tasks <= 0) num_tasks = 20;
    if (chunk_size <= 0) chunk_size = 100000;
    
    std::string output_file = config["output"].as<std::string>();
    unsigned num_cores = seastar::smp::count;
    
    applog.info("启动配置: 任务数={}, 区间大小={}, 输出文件={}, 核心数={}", 
                num_tasks, chunk_size, output_file, num_cores);
    
    // 在 core 0 上初始化全局状态
    initialize_global_state(num_tasks, chunk_size);
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 为每个核心准备任务列表（值拷贝）
    std::vector<std::vector<std::tuple<int, uint64_t, uint64_t>>> core_task_lists(num_cores);
    for (int task_id = 0; task_id < num_tasks; ++task_id) {
        unsigned target_core = task_id % num_cores;
        uint64_t start = static_cast<uint64_t>(task_id) * chunk_size + 2;
        uint64_t end = static_cast<uint64_t>(task_id + 1) * chunk_size;
        core_task_lists[target_core].push_back({task_id, start, end});
    }
    
    // 为每个核心创建并行任务
    std::vector<seastar::future<>> futures;
    futures.reserve(num_cores);
    
    for (unsigned core = 0; core < num_cores; ++core) {
        futures.push_back(
            seastar::smp::submit_to(core, 
                [my_tasks = std::move(core_task_lists[core]), core] {
                    return worker_loop(std::move(my_tasks), core);
                })
        );
    }
    
    return seastar::when_all(futures.begin(), futures.end()).discard_result().then(
        [start_time, output_file] {
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            print_statistics(duration.count());
            
            return output_results(output_file).then([duration] {
                applog.info("程序执行完成，总耗时: {} 毫秒", duration.count());
            });
        });
}

int main(int argc, char** argv) {
    seastar::app_template app;
    
    app.add_options()
        ("tasks,t", po::value<int>()->default_value(20), "任务总数")
        ("chunk,n", po::value<int>()->default_value(100000), "每个任务的区间大小")
        ("output,o", po::value<std::string>()->default_value("dpsk4_seastar_prime.csv"), "输出CSV文件路径")
        ("log-level,l", po::value<std::string>(), "日志级别 (debug/info/error/trace)");
    
    return app.run(argc, argv, [&app] {
        return seastar_main(app.configuration());
    });
}
