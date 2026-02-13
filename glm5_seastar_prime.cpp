// Seastar 并行素数计算程序

#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/file.hh>
#include <seastar/core/seastar.hh>
#include <seastar/util/log.hh>
#include <boost/program_options.hpp>

#include <iostream>
#include <vector>
#include <queue>
#include <optional>
#include <cmath>
#include <iomanip>
#include <string>
#include <chrono>
#include <algorithm>

namespace ss = seastar;
namespace po = boost::program_options;
static ss::logger app_log("glm5_seastar");

// 任务结构
struct Task {
    uint64_t start;
    uint64_t end;
};

// 任务结果结构
struct TaskResult {
    uint64_t start;
    uint64_t end;
    unsigned int core_id;
    std::vector<uint64_t> primes;
};

// 全局状态管理器（仅在 core 0 上访问）
class GlobalState {
public:
    std::queue<Task> task_queue;              // 任务队列
    std::vector<TaskResult> results;          // 结果收集（内存）
    uint64_t total_tasks = 0;                 // 总任务数
    uint64_t completed_tasks = 0;             // 已完成任务数
    uint64_t total_primes = 0;                // 素数总数
    
    static GlobalState& instance() {
        static GlobalState instance;
        return instance;
    }
    
private:
    GlobalState() = default;
};

// ==================== 素数计算函数 ====================

inline bool is_prime(uint64_t n) {
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

// 计算区间内的所有素数（分批处理，避免阻塞reactor）
std::vector<uint64_t> compute_primes_in_range(uint64_t start, uint64_t end) {
    std::vector<uint64_t> primes;
    primes.reserve(end - start > 10000 ? 10000 : end - start);
    
    constexpr uint64_t BATCH_SIZE = 1000;
    uint64_t current = start;
    
    while (current <= end) {
        uint64_t batch_end = std::min(current + BATCH_SIZE - 1, end);
        
        for (uint64_t n = current; n <= batch_end; ++n) {
            if (is_prime(n)) {
                primes.push_back(n);
            }
        }
        
        current = batch_end + 1;
        
        // 每处理完一个批次就让出控制权
        if (seastar::thread::running_in_thread()) {
            seastar::thread::yield();
        }
    }
    
    return primes;
}

// ==================== 任务队列初始化函数 ====================

void init_task_queue(uint64_t max_num, uint64_t chunk_size) {
    auto& state = GlobalState::instance();

    while (!state.task_queue.empty()) {
        state.task_queue.pop();
    }

    uint64_t num_tasks = max_num / chunk_size;
    for (uint64_t task_id = 0; task_id < num_tasks; ++task_id) {
        uint64_t start = task_id * chunk_size + 2;
        uint64_t end = (task_id + 1) * chunk_size;
        state.task_queue.push({start, end});
    }

    state.total_tasks = state.task_queue.size();
    state.completed_tasks = 0;

    std::cout << "\n========================================" << std::endl;
    std::cout << "       任务队列初始化完成" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "计算范围: 2 - " << max_num << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "总任务数: " << state.total_tasks << std::endl;
    std::cout << "CPU核心数: " << seastar::smp::count << std::endl;
    std::cout << "========================================\n" << std::endl;
}

// ==================== 异步输出结果函数 ====================

seastar::future<> output_results(const std::string& filename, uint64_t max_num, long duration_ms) {
    auto& state = GlobalState::instance();
    
    std::cout << "\n正在写入结果文件: " << filename << std::endl;
    
    // 按任务起始位置排序
    std::sort(state.results.begin(), state.results.end(),
              [](const TaskResult& a, const TaskResult& b) {
                  return a.start < b.start;
              });
    
    // 构建输出内容
    std::string content;
    content.reserve(state.results.size() * 100);
    
    for (const auto& result : state.results) {
        content += std::to_string(result.start) + "-" + std::to_string(result.end);
        content += "," + std::to_string(result.core_id);
        for (uint64_t prime : result.primes) {
            content += "," + std::to_string(prime);
        }
        content += "\n";
    }
    
    // 使用 Seastar 异步 DMA I/O 写入文件
    return seastar::open_file_dma(filename, seastar::open_flags::wo | seastar::open_flags::create | seastar::open_flags::truncate).then(
        [content = std::move(content), filename, max_num, duration_ms](seastar::file f) {
            return seastar::do_with(std::move(f), std::move(content), [filename, max_num, duration_ms](seastar::file& f, const std::string& content) {
                return f.dma_write(0, content.data(), content.size()).then(
                    [&f, filename, max_num, duration_ms, size = content.size()](size_t written) {
                        if (written != size) {
                            app_log.error("写入不完整: {} / {}", written, size);
                        }
                        std::cout << "结果已写入: " << filename << std::endl;
                        
                        auto& state = GlobalState::instance();
                        double prime_density = 100.0 * state.total_primes / max_num;
                        
                        std::cout << "\n========================================" << std::endl;
                        std::cout << "         计算结果统计" << std::endl;
                        std::cout << "========================================" << std::endl;
                        std::cout << "已完成任务: " << state.completed_tasks << "/" << state.total_tasks << std::endl;
                        std::cout << "素数总数:   " << state.total_primes << std::endl;
                        std::cout << "计算耗时:   " << duration_ms << " ms" << std::endl;
                        std::cout << "素数密度:   " << std::fixed << std::setprecision(4) << prime_density << "%" << std::endl;
                        std::cout << "计算速度:   " << std::fixed << std::setprecision(0) 
                                  << static_cast<double>(max_num) / duration_ms << " 数/毫秒" << std::endl;
                        std::cout << "素数发现率: " << std::fixed << std::setprecision(2) 
                                  << static_cast<double>(state.total_primes) / duration_ms << " 素数/毫秒" << std::endl;
                        std::cout << "========================================" << std::endl;
                        
                        return f.close();
                    });
            });
        });
}

// ==================== 任务管理函数 ====================

seastar::future<std::optional<Task>> get_next_task() {
    auto& state = GlobalState::instance();
    
    if (state.task_queue.empty()) {
        return seastar::make_ready_future<std::optional<Task>>(std::nullopt);
    }
    
    Task task = state.task_queue.front();
    state.task_queue.pop();
    
    return seastar::make_ready_future<std::optional<Task>>(task);
}

seastar::future<> collect_result(uint64_t start, uint64_t end,
                                   unsigned int core_id,
                                   std::vector<uint64_t> primes) {
    auto& state = GlobalState::instance();
    
    // 收集结果到内存
    state.results.push_back(TaskResult{start, end, core_id, std::move(primes)});
    
    state.completed_tasks++;
    state.total_primes += state.results.back().primes.size();
    
    // 打印进度
    if (state.completed_tasks % 100 == 0 ||
        state.completed_tasks == state.total_tasks) {
        double progress = 100.0 * state.completed_tasks / state.total_tasks;
        std::cout << "\r进度: " << std::fixed << std::setprecision(1)
                  << progress << "% (" << state.completed_tasks << "/" << state.total_tasks 
                  << " 任务, 素数: " << state.total_primes << ")" << std::flush;
    }
    
    return seastar::make_ready_future<>();
}

// ==================== 核心任务处理函数 ====================

seastar::future<> process_tasks_on_core(unsigned int core_id) {
    return seastar::repeat([core_id] {
        return seastar::smp::submit_to(0, [] {
            return get_next_task();
        }).then([core_id](std::optional<Task> task_opt) 
                -> seastar::future<seastar::stop_iteration> {
            if (!task_opt) {
                return seastar::make_ready_future<seastar::stop_iteration>(
                    seastar::stop_iteration::yes);
            }
            
            Task task = *task_opt;
            
            // 使用 seastar::async 在后台线程中计算素数
            return seastar::async([task, core_id] {
                return compute_primes_in_range(task.start, task.end);
            }).then([task, core_id](std::vector<uint64_t> primes) {
                return seastar::smp::submit_to(0, 
                    [start = task.start, end = task.end, core_id, 
                     primes = std::move(primes)]() mutable {
                        return collect_result(start, end, core_id, 
                                              std::move(primes));
                    });
            }).then([] {
                return seastar::make_ready_future<seastar::stop_iteration>(
                    seastar::stop_iteration::no);
            });
        });
    });
}

// ==================== 主函数 ====================

seastar::future<> seastar_main(const po::variables_map& config) {
    app_log.set_level(seastar::log_level::error);
    
    int num_tasks = config["tasks"].as<int>();
    int chunk_size = config["chunk"].as<int>();
    std::string output_file = config["output"].as<std::string>();
    
    if (config.count("log-level")) {
        std::string level = config["log-level"].as<std::string>();
        if (level == "debug") app_log.set_level(seastar::log_level::debug);
        else if (level == "info") app_log.set_level(seastar::log_level::info);
        else if (level == "trace") app_log.set_level(seastar::log_level::trace);
    }
    
    if (num_tasks <= 0) num_tasks = 20;
    if (chunk_size <= 0) chunk_size = 100000;
    
    uint64_t max_num = static_cast<uint64_t>(num_tasks) * chunk_size;
    
    app_log.info("启动配置: 任务数={}, 区间大小={}, 输出文件={}, 核心数={}", 
                 num_tasks, chunk_size, output_file, seastar::smp::count);
    
    auto start_time = std::chrono::high_resolution_clock::now();

    return seastar::smp::submit_to(0, [max_num, chunk_size] {
        init_task_queue(max_num, chunk_size);
        GlobalState::instance().results.clear();
        std::cout << "开始并行计算...\n" << std::endl;
        return seastar::make_ready_future<>();
        
    }).then([] {
        std::vector<seastar::future<>> futures;
        
        for (unsigned int i = 0; i < seastar::smp::count; ++i) {
            futures.push_back(
                seastar::smp::submit_to(i, [i] {
                    return process_tasks_on_core(i);
                })
            );
        }
        
        return seastar::when_all(futures.begin(), futures.end()).discard_result();
        
    }).then([start_time, max_num, output_file] {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // 使用异步 DMA I/O 写入结果
        return output_results(output_file, max_num, duration.count()).then([duration] {
            app_log.info("计算完成: 素数总数={}, 耗时={}ms", 
                         GlobalState::instance().total_primes, duration.count());
        });
    });
}

int main(int argc, char** argv) {
    seastar::app_template app;
    
    app.add_options()
        ("tasks,t", po::value<int>()->default_value(20), "任务总数")
        ("chunk,n", po::value<int>()->default_value(100000), "每个任务的区间大小")
        ("output,o", po::value<std::string>()->default_value("glm5_seastar_prime.csv"), "输出CSV文件路径")
        ("log-level,l", po::value<std::string>(), "日志级别 (debug/info/error/trace)");
    
    return app.run(argc, argv, [&app] {
        return seastar_main(app.configuration());
    });
}
