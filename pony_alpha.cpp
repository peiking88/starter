// Seastar 并行素数计算程序

#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/loop.hh>

#include <iostream>
#include <fstream>
#include <vector>
#include <queue>
#include <optional>
#include <cmath>
#include <iomanip>
#include <string>

// 配置常量
constexpr uint64_t MAX_NUM = 2000000000ULL;      // 20亿
constexpr uint64_t CHUNK_SIZE = 100000ULL;       // 每个区间最大10万
constexpr const char* OUTPUT_FILE = "primes.csv";

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
    std::queue<Task> task_queue;          // 任务队列
    std::ofstream output_file;            // 输出文件流
    uint64_t total_tasks = 0;             // 总任务数
    uint64_t completed_tasks = 0;         // 已完成任务数
    
    static GlobalState& instance() {
        static GlobalState instance;
        return instance;
    }
    
private:
    GlobalState() = default;
};

// ==================== 素数计算函数 ====================

// 优化的素数检查函数
bool is_prime(uint64_t n) {
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
std::vector<uint64_t> compute_primes_in_range(uint64_t start, uint64_t end) {
    std::vector<uint64_t> primes;
    primes.reserve(end - start > 10000 ? 10000 : end - start);
    
    for (uint64_t n = start; n <= end; ++n) {
        if (is_prime(n)) {
            primes.push_back(n);
        }
    }
    return primes;
}

// ==================== 任务队列初始化函数 ====================

void init_task_queue() {
    auto& state = GlobalState::instance();
    
    // 清空队列（如果有的话）
    while (!state.task_queue.empty()) {
        state.task_queue.pop();
    }
    
    // 生成所有任务区间
    for (uint64_t start = 2; start <= MAX_NUM; start += CHUNK_SIZE) {
        uint64_t end = std::min(start + CHUNK_SIZE - 1, MAX_NUM);
        state.task_queue.push({start, end});
    }
    
    state.total_tasks = state.task_queue.size();
    state.completed_tasks = 0;
    
    std::cout << "=== 任务队列初始化完成 ===" << std::endl;
    std::cout << "计算范围: 2 - " << MAX_NUM << std::endl;
    std::cout << "区间大小: " << CHUNK_SIZE << std::endl;
    std::cout << "总任务数: " << state.total_tasks << std::endl;
    std::cout << "CPU核心数: " << seastar::smp::count << std::endl;
    std::cout << "========================" << std::endl;
}

// ==================== 输出计算结果函数 ====================

void write_results_to_csv() {
    auto& state = GlobalState::instance();
    
    state.output_file.open(OUTPUT_FILE);
    if (!state.output_file.is_open()) {
        std::cerr << "Error: 无法打开输出文件 " << OUTPUT_FILE << std::endl;
        return;
    }
    
    std::cout << "\n=== 开始写入结果文件 ===" << std::endl;
    std::cout << "输出文件: " << OUTPUT_FILE << std::endl;
}

void close_output_file() {
    auto& state = GlobalState::instance();
    
    if (state.output_file.is_open()) {
        state.output_file.close();
    }
    
    std::cout << "\n=== 计算完成 ===" << std::endl;
    std::cout << "已完成任务: " << state.completed_tasks << "/" << state.total_tasks << std::endl;
    std::cout << "结果已保存至: " << OUTPUT_FILE << std::endl;
}

// ==================== 任务管理函数 ====================

// 获取下一个任务（在 core 0 上执行）
seastar::future<std::optional<Task>> get_next_task() {
    auto& state = GlobalState::instance();
    
    if (state.task_queue.empty()) {
        return seastar::make_ready_future<std::optional<Task>>(std::nullopt);
    }
    
    Task task = state.task_queue.front();
    state.task_queue.pop();
    
    return seastar::make_ready_future<std::optional<Task>>(task);
}

// 写入单条结果到CSV（在 core 0 上执行）
seastar::future<> write_result_to_csv(uint64_t start, uint64_t end, 
                                       unsigned int core_id, 
                                       std::vector<uint64_t> primes) {
    auto& state = GlobalState::instance();
    
    // 写入CSV行：任务范围, CPU核编号, 素数列表
    state.output_file << start << "-" << end << "," << core_id;
    for (uint64_t prime : primes) {
        state.output_file << "," << prime;
    }
    state.output_file << "\n";
    
    // 更新进度
    state.completed_tasks++;
    
    // 每1000个任务或完成时显示进度
    if (state.completed_tasks % 1000 == 0 || 
        state.completed_tasks == state.total_tasks) {
        double progress = 100.0 * state.completed_tasks / state.total_tasks;
        std::cout << "\r进度: " << std::fixed << std::setprecision(2) 
                  << progress << "% (" << state.completed_tasks 
                  << "/" << state.total_tasks << " 任务)" << std::flush;
    }
    
    return seastar::make_ready_future<>();
}

// ==================== 核心任务处理函数 ====================

// 在单个核心上处理任务（使用 .then() 调用链）
seastar::future<> process_tasks_on_core(unsigned int core_id) {
    return seastar::repeat([core_id] {
        // 步骤1: 从 core 0 的任务队列获取下一个任务
        return seastar::smp::submit_to(0, [] {
            return get_next_task();
        }).then([core_id](std::optional<Task> task_opt) 
                -> seastar::future<seastar::stop_iteration> {
            // 步骤2: 检查是否还有任务
            if (!task_opt) {
                // 没有更多任务，停止循环
                return seastar::make_ready_future<seastar::stop_iteration>(
                    seastar::stop_iteration::yes);
            }
            
            Task task = *task_opt;
            
            // 步骤3: 在当前核心上计算素数（CPU密集型任务）
            std::vector<uint64_t> primes = compute_primes_in_range(
                task.start, task.end);
            
            // 步骤4: 将结果提交到 core 0 写入文件
            return seastar::smp::submit_to(0, 
                [start = task.start, end = task.end, core_id, 
                 primes = std::move(primes)]() mutable {
                    return write_result_to_csv(start, end, core_id, 
                                               std::move(primes));
                }).then([] {
                // 步骤5: 继续处理下一个任务
                return seastar::make_ready_future<seastar::stop_iteration>(
                    seastar::stop_iteration::no);
            });
        });
    });
}

// ==================== 主函数 ====================

int main(int argc, char** argv) {
    seastar::app_template app;
    
    // 配置应用程序选项
    app.add_options()
        ("output,o", 
         boost::program_options::value<std::string>()->default_value(OUTPUT_FILE),
         "Output CSV file path");
    
    return app.run(argc, argv, [] {
        // 确保在 core 0 上执行初始化
        return seastar::smp::submit_to(0, [] {
            // 1. 初始化任务队列
            init_task_queue();
            
            // 2. 初始化输出文件
            write_results_to_csv();
            
        }).then([] {
            // 3. 在所有核心上启动任务处理
            std::vector<seastar::future<>> futures;
            
            for (unsigned int i = 0; i < seastar::smp::count; ++i) {
                futures.push_back(
                    seastar::smp::submit_to(i, [i] {
                        return process_tasks_on_core(i);
                    })
                );
            }
            
            // 4. 等待所有核心完成任务
            return seastar::when_all(futures.begin(), futures.end()).discard_result();
            
        }).then([] {
            // 5. 关闭输出文件并显示完成信息
            return seastar::smp::submit_to(0, [] {
                close_output_file();
            });
        });
    });
}
