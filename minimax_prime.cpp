// minimax_prime: 并行素数计算程序 - 使用 libfork 框架
// 工作模式：主线程创建任务队列，多个工作线程从队列中获取任务执行（工作窃取）

#include <libfork/core.hpp>
#include <libfork/schedule.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <queue>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <string>
#include <optional>
#include <iomanip>
#include <getopt.h>

// 全局配置
int g_num_tasks = 20;           // 任务总数
int g_chunk_size = 100000;      // 每个任务的区间大小
int g_num_threads = 4;          // 使用线程数

// 任务结构
struct Task {
    int task_id;
    uint64_t start;
    uint64_t end;
};

// 线程安全的任务队列
class TaskQueue {
private:
    std::queue<Task> queue_;
    std::mutex mutex_;
    std::atomic<int> next_task_id_{0};
    int total_tasks_;

public:
    TaskQueue(int total_tasks) : total_tasks_(total_tasks), next_task_id_(0) {}

    // 从队列中获取下一个任务（线程安全）
    std::optional<Task> getNextTask() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (next_task_id_ >= total_tasks_) {
            return std::nullopt;
        }
        
        Task task;
        task.task_id = next_task_id_++;
        // 从2开始（1不是素数）
        task.start = static_cast<uint64_t>(task.task_id) * g_chunk_size + 2;
        task.end = static_cast<uint64_t>(task.task_id + 1) * g_chunk_size;
        
        return task;
    }

    // 检查是否还有任务
    bool hasTasks() {
        std::lock_guard<std::mutex> lock(mutex_);
        return next_task_id_ < total_tasks_;
    }

    // 获取剩余任务数
    int remainingTasks() {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_tasks_ - next_task_id_;
    }
};

// 全局任务队列
TaskQueue* g_task_queue = nullptr;

// 任务结果结构
struct TaskResult {
    int task_id;
    uint64_t start;
    uint64_t end;
    int core_id;
    std::vector<uint64_t> primes;
};

// 结果收集
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
    primes.reserve((end - start) / 10);  // 预估素数密度约10%
    
    for (uint64_t n = start; n <= end; ++n) {
        if (isPrime(n)) {
            primes.push_back(n);
        }
    }
    return primes;
}

// libfork 任务：处理单个任务
// 使用 co_await 实现工作窃取模式
inline constexpr auto processTaskLibfork = 
    [](auto self, int core_id) -> lf::task<void> {
    
    while (true) {
        // 从任务队列获取下一个任务
        auto task_opt = g_task_queue->getNextTask();
        
        if (!task_opt) {
            // 没有更多任务，退出
            co_return;
        }
        
        Task task = *task_opt;
        
        // 计算该区间的素数
        std::vector<uint64_t> primes = computePrimesInRange(task.start, task.end);
        
        // 先统计素数（因为primes会被move）
        size_t count = primes.size();
        
        // 收集结果
        {
            std::lock_guard<std::mutex> lock(g_results_mutex);
            TaskResult result;
            result.task_id = task.task_id;
            result.start = task.start;
            result.end = task.end;
            result.core_id = core_id;
            result.primes = std::move(primes);
            g_results.push_back(std::move(result));
        }
        
        g_completed_tasks.fetch_add(1);
        g_total_primes.fetch_add(count);
        
        // 打印进度
        if (g_completed_tasks % 10 == 0 || g_completed_tasks == g_num_tasks) {
            double progress = 100.0 * g_completed_tasks / g_num_tasks;
            std::cout << "进度: " << std::fixed << std::setprecision(2) 
                      << progress << "% (" << g_completed_tasks << "/" << g_num_tasks 
                      << " 任务, 素数: " << g_total_primes << ")" << std::endl;
        }
    }
};

// 工作线程函数
void workerThread(int core_id, lf::lazy_pool* pool) {
    lf::sync_wait(*pool, processTaskLibfork, core_id);
}

// 初始化任务队列
void initTaskQueue(int num_tasks, int chunk_size) {
    g_num_tasks = num_tasks;
    g_chunk_size = chunk_size;
    g_task_queue = new TaskQueue(num_tasks);
    
    std::cout << "=== 任务队列初始化完成 ===" << std::endl;
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "总任务数: " << num_tasks << std::endl;
    std::cout << "工作线程数: " << g_num_threads << std::endl;
    std::cout << "==============================" << std::endl;
}

// 输出计算结果到CSV文件
void outputResults(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: 无法打开输出文件 " << filename << std::endl;
        return;
    }
    
    std::cout << "\n=== 开始写入结果文件 ===" << std::endl;
    std::cout << "输出文件: " << filename << std::endl;
    
    // 按任务ID排序输出
    std::sort(g_results.begin(), g_results.end(), 
              [](const TaskResult& a, const TaskResult& b) {
                  return a.task_id < b.task_id;
              });
    
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
    
    std::cout << "=== 计算完成 ===" << std::endl;
    std::cout << "已完成任务: " << g_completed_tasks << "/" << g_num_tasks << std::endl;
    std::cout << "素数总数: " << g_total_primes << std::endl;
}

int main(int argc, char** argv) {
    // 解析命令行参数
    int num_tasks = 20;
    int chunk_size = 100000;
    int num_threads = 4;
    
    int opt;
    while ((opt = getopt(argc, argv, "t:n:c:")) != -1) {
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
            default:
                std::cerr << "用法: " << argv[0] << " [-t 任务数] [-n 区间大小] [-c 线程数]" << std::endl;
                std::cout << "\n参数说明:" << std::endl;
                std::cout << "  -t <N>   任务数 (默认: 20)" << std::endl;
                std::cout << "  -n <N>   区间大小，每任务计算的数字范围，不超过10万 (默认: 100000)" << std::endl;
                std::cout << "  -c <N>   CPU核数/线程数 (默认: 4)" << std::endl;
                std::cout << "\n示例:" << std::endl;
                std::cout << "  " << argv[0] << " -t 100 -n 100000 -c 8   # 100任务, 每任务10万, 8核" << std::endl;
                std::cout << "  " << argv[0] << " -t 200 -n 50000 -c 16  # 200任务, 每任务5万, 16核" << std::endl;
                std::cout << "  " << argv[0] << " -t 320 -n 10000 -c 32  # 320任务, 每任务1万, 32核" << std::endl;
                return 1;
        }
    }
    
    if (num_tasks <= 0) num_tasks = 20;
    if (chunk_size <= 0) chunk_size = 100000;
    if (num_threads <= 0) num_threads = 4;
    
    if (chunk_size > 100000) {
        std::cerr << "警告: 区间大小超过10万，已调整为10万" << std::endl;
        chunk_size = 100000;
    }
    
    g_num_threads = num_threads;
    
    // 初始化任务队列
    initTaskQueue(num_tasks, chunk_size);
    
    // 记录开始时间
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 创建 libfork 线程池
    lf::lazy_pool pool(static_cast<size_t>(num_threads));
    
    std::cout << "\n=== 开始计算 ===" << std::endl;
    
    // 启动工作线程
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(workerThread, i, &pool);
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }
    
    // 记录结束时间
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // 输出结果到CSV
    std::string output_file = "primes_" + std::to_string(num_tasks) + "_" + 
                               std::to_string(chunk_size) + ".csv";
    outputResults(output_file);
    
    std::cout << "总耗时: " << duration.count() << "ms" << std::endl;
    
    // 清理
    delete g_task_queue;
    
    return 0;
}
