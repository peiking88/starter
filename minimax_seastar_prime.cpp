// minimax_seastar_prime: 并行素数计算程序 - 使用 Seastar 框架
// 工作模式：使用seastar::future和.then()调用链，submit_to提交任务到CPU核心
// 动态工作窃取模式：当某核任务完成后从队列获取新任务

#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/file.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/log.hh>
#include <boost/program_options.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <atomic>
#include <optional>
#include <iomanip>
#include <cmath>
#include <cstdlib>

namespace po = boost::program_options;

// Seastar日志器
static seastar::logger applog("minimax_prime");

// 全局配置
int g_num_tasks = 20;           // 任务总数
int g_chunk_size = 100000;      // 每个任务的区间大小
int g_num_cores = 4;           // 使用CPU核数

// 任务结构
struct Task {
    int task_id;
    uint64_t start;
    uint64_t end;
};

// 任务结果结构
struct TaskResult {
    int task_id;
    uint64_t start;
    uint64_t end;
    int core_id;
    std::vector<uint64_t> primes;
};

// 全局任务队列 - 使用atomic实现工作窃取
std::atomic<int> g_next_task_id{0};
int g_total_tasks = 0;

// 全局结果收集
std::vector<TaskResult> g_results;
std::mutex g_results_mutex;
std::atomic<int> g_completed_tasks{0};
std::atomic<int> g_total_primes{0};

// 优化的素数检查函数
bool isPrime(uint64_t n) {
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
    primes.reserve((end - start) / 10);
    
    for (uint64_t n = start; n <= end; ++n) {
        if (isPrime(n)) {
            primes.push_back(n);
        }
    }
    return primes;
}

// 工作核心函数：使用 seastar::repeat 循环处理任务
seastar::future<> workerCoreLoop(int core_id) {
    return seastar::repeat([core_id] {
        // 从全局队列获取下一个任务（工作窃取）
        int task_id = g_next_task_id.fetch_add(1, std::memory_order_relaxed);
        
        if (task_id >= g_total_tasks) {
            // 没有更多任务，停止循环
            return seastar::make_ready_future<seastar::stop_iteration>(
                seastar::stop_iteration::yes);
        }
        
        Task task;
        task.task_id = task_id;
        task.start = static_cast<uint64_t>(task_id) * g_chunk_size + 2;
        task.end = static_cast<uint64_t>(task_id + 1) * g_chunk_size;
        
        // 使用 seastar::async 在后台线程中计算素数，避免阻塞 reactor
        return seastar::async([task, core_id] {
            return computePrimesInRange(task.start, task.end);
        }).then([task, core_id](std::vector<uint64_t> primes) -> seastar::future<seastar::stop_iteration> {
            size_t count = primes.size();
            
            // 更新统计
            int completed = g_completed_tasks.fetch_add(1) + 1;
            g_total_primes.fetch_add(static_cast<int>(count));
            
            // 打印进度
            if (completed % 10 == 0 || completed == g_num_tasks) {
                double progress = 100.0 * completed / g_num_tasks;
                std::cout << "\r进度: " << std::fixed << std::setprecision(1) 
                          << progress << "% (" << completed << "/" << g_num_tasks 
                          << " 任务, 素数: " << g_total_primes.load() << ")" << std::flush;
            }
            
            applog.debug("核心 {} 完成任务 {} [{}-{}], 找到 {} 个素数", 
                         core_id, task.task_id, task.start, task.end, count);
            
            // 收集结果
            {
                std::lock_guard<std::mutex> lock(g_results_mutex);
                g_results.push_back(TaskResult{task.task_id, task.start, task.end, core_id, std::move(primes)});
            }
            
            // 继续处理下一个任务
            return seastar::make_ready_future<seastar::stop_iteration>(
                seastar::stop_iteration::no);
        });
    });
}

// 初始化任务队列函数 - 消除重复代码
void initTaskQueue(int num_tasks, int chunk_size, int num_cores) {
    g_num_tasks = num_tasks;
    g_chunk_size = chunk_size;
    g_num_cores = num_cores;
    g_total_tasks = num_tasks;
    g_next_task_id.store(0);
    g_completed_tasks.store(0);
    g_total_primes.store(0);
    g_results.clear();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "任务队列初始化完成" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "总任务数: " << num_tasks << std::endl;
    std::cout << "CPU核心数: " << num_cores << std::endl;
    std::cout << "========================================\n" << std::endl;
}

// 输出计算结果到CSV文件函数 - 使用Seastar异步I/O避免阻塞reactor
seastar::future<> outputResults(const std::string& filename) {
    std::cout << "\n正在写入结果文件: " << filename << std::endl;
    
    // 按任务ID排序（在主线程完成，但排序时间较短）
    std::sort(g_results.begin(), g_results.end(), 
              [](const TaskResult& a, const TaskResult& b) {
                  return a.task_id < b.task_id;
              });
    
    // 构建输出内容（分批处理避免单次大内存分配）
    std::string content;
    content.reserve(g_results.size() * 100);  // 预分配空间
    
    for (const auto& result : g_results) {
        content += std::to_string(result.start) + "-" + std::to_string(result.end);
        content += "," + std::to_string(result.core_id);
        for (uint64_t prime : result.primes) {
            content += "," + std::to_string(prime);
        }
        content += "\n";
    }
    
    // 使用Seastar异步文件I/O
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

// 打印统计结果
void printStatistics(long duration_ms) {
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

// Seastar应用主函数
seastar::future<> seastar_main(const po::variables_map& config) {
    // 设置日志级别 - 默认error
    applog.set_level(seastar::log_level::error);
    
    // 从命令行参数获取配置
    g_num_tasks = config["tasks"].as<int>();
    g_chunk_size = config["chunk"].as<int>();
    // 使用 Seastar 框架的 -c/--smp 参数设置的核心数
    g_num_cores = static_cast<int>(seastar::smp::count);
    
    if (config.count("log-level")) {
        std::string level = config["log-level"].as<std::string>();
        if (level == "debug") applog.set_level(seastar::log_level::debug);
        else if (level == "info") applog.set_level(seastar::log_level::info);
        else if (level == "trace") applog.set_level(seastar::log_level::trace);
    }
    
    if (g_num_cores <= 0) g_num_cores = 1;
    
    // 从命令行参数获取输出文件名
    std::string output_file = "minimax_seastar_prime.csv";
    if (config.count("output")) {
        output_file = config["output"].as<std::string>();
    }
    
    applog.info("启动配置: 任务数={}, 区间大小={}, 核心数={}, 输出文件={}", 
                g_num_tasks, g_chunk_size, g_num_cores, output_file);
    
    // 调用函数初始化任务队列
    initTaskQueue(g_num_tasks, g_chunk_size, g_num_cores);
    
    // 记录开始时间
    auto start_time = std::chrono::high_resolution_clock::now();
    
    applog.info("开始并行计算...");
    std::cout << "开始并行计算...\n" << std::endl;
    std::cout << std::flush;
    
    // 创建所有核心的任务future - 使用submit_to提交到各核心
    // 使用when_all等待所有核心完成任务
    std::vector<seastar::future<>> all_futures;
    all_futures.reserve(g_num_cores);
    
    for (int i = 0; i < g_num_cores; ++i) {
        // 使用submit_to将任务提交到指定核心
        all_futures.push_back(seastar::smp::submit_to(i, [i]() {
            return workerCoreLoop(i);
        }));
    }
    
    // 使用when_all_succeed等待所有任务完成，然后使用.then()链处理后续操作
    return seastar::when_all_succeed(all_futures.begin(), all_futures.end()).then(
        [start_time, output_file]() {
            std::cout << std::endl;
            
            // 记录结束时间
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // 调用函数输出结果到CSV文件（异步I/O）
            return outputResults(output_file).then([duration, start_time]() {
                // 打印统计结果
                printStatistics(duration.count());
                applog.info("计算完成: 任务数={}, 素数总数={}, 耗时={}ms", 
                            g_completed_tasks.load(), g_total_primes.load(), duration.count());
                return seastar::make_ready_future<>();
            });
        });
}

int main(int argc, char** argv) {
    seastar::app_template app;
    
    // 添加命令行选项（核心数使用 Seastar 框架的 -c/--smp 参数）
    app.add_options()
        ("tasks,t", po::value<int>()->default_value(20), "任务总数")
        ("chunk,n", po::value<int>()->default_value(100000), "每个任务的区间大小")
        ("output,o", po::value<std::string>()->default_value("minimax_seastar_prime.csv"), "输出CSV文件路径")
        ("log-level,l", po::value<std::string>(), "日志级别 (debug/info/error/trace)");
    
    return app.run(argc, argv, [&app] {
        return seastar_main(app.configuration());
    });
}
