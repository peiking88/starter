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
#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/log.hh>
#include <boost/program_options.hpp>

#include <iostream>
#include <vector>
#include <atomic>
#include <iomanip>
#include <cstdlib>
#include <memory>

#include "prime_sieve.hpp"

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

    TaskResult() = default;
    TaskResult(int tid, uint64_t s, uint64_t e, int cid, std::vector<uint64_t> p)
        : task_id(tid), start(s), end(e), core_id(cid), primes(std::move(p)) {}
    TaskResult(TaskResult&&) noexcept = default;
    TaskResult& operator=(TaskResult&&) noexcept = default;
};

// C1: Cache-line padded atomic 类型，避免 false sharing
struct alignas(64) AlignedAtomicInt { std::atomic<int> value{0}; };
struct alignas(64) AlignedAtomicU64 { std::atomic<uint64_t> value{0}; };

// 全局任务队列 - 使用 padded atomic 实现工作窃取
static AlignedAtomicInt g_next_task_id;      // 任务分发
static int g_total_tasks = 0;
static AlignedAtomicInt g_completed_tasks;   // 完成计数
static AlignedAtomicU64 g_total_primes;      // 素数总数

// C2: Per-core 结果数组，消除 mutex
constexpr size_t kMaxCores = 128;
struct alignas(64) PaddedResults { std::vector<TaskResult> results; };
static PaddedResults g_results_per_core[kMaxCores];

// 工作核心函数：使用 seastar::repeat 循环处理任务
seastar::future<> workerCoreLoop(int core_id) {
    return seastar::repeat([core_id] {
        // 从全局队列获取下一个任务（工作窃取）
        // M1: 使用 relaxed memory order
        int task_id = g_next_task_id.value.fetch_add(1, std::memory_order_relaxed);

        // L1: 循环终止条件加 [[unlikely]]
        if (task_id >= g_total_tasks) [[unlikely]] {
            // 没有更多任务，停止循环
            return seastar::make_ready_future<seastar::stop_iteration>(
                seastar::stop_iteration::yes);
        }

        Task task;
        task.task_id = task_id;
        // 连续区间：第一个任务从2开始，后续任务紧接前一个任务
        task.start = (task_id == 0) ? 2 : static_cast<uint64_t>(task_id) * g_chunk_size;
        task.end = static_cast<uint64_t>(task_id + 1) * g_chunk_size;

        // 使用 seastar::async 在后台线程中计算素数，避免阻塞 reactor
        return seastar::async([task, core_id] {
            return prime::segmented_sieve(task.start, task.end);
        }).then([task, core_id](std::vector<uint64_t> primes) -> seastar::future<seastar::stop_iteration> {
            size_t count = primes.size();

            // M1: 使用 relaxed memory order 更新统计
            g_completed_tasks.value.fetch_add(1, std::memory_order_relaxed);
            // H3: uint64_t 素数总数
            g_total_primes.value.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);

            // C2: 直接写入 per-core 结果数组，无需 mutex
            g_results_per_core[core_id].results.push_back(TaskResult{task.task_id, task.start, task.end, core_id, std::move(primes)});

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
    g_next_task_id.value.store(0, std::memory_order_relaxed);
    g_completed_tasks.value.store(0, std::memory_order_relaxed);
    g_total_primes.value.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < static_cast<size_t>(num_cores); ++i) {
        g_results_per_core[i].results.clear();
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "任务队列初始化完成" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "总任务数: " << num_tasks << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "CPU核心数: " << num_cores << std::endl;
    std::cout << "========================================\n" << std::endl;
}

// 输出计算结果到CSV文件函数 - 使用POSIX I/O避免Seastar内存分配限制
seastar::future<> outputResults(const std::string& filename, int num_cores) {
    std::cout << "\n正在写入结果文件: " << filename << std::endl;

    // C2: 合并 per-core 结果
    std::vector<TaskResult> all_results;
    {
        size_t total = 0;
        for (size_t i = 0; i < static_cast<size_t>(num_cores); ++i) total += g_results_per_core[i].results.size();
        all_results.reserve(total);
        for (size_t i = 0; i < static_cast<size_t>(num_cores); ++i) {
            for (auto& r : g_results_per_core[i].results) all_results.push_back(std::move(r));
            g_results_per_core[i].results.clear();
        }
    }

    // 按任务ID排序
    std::sort(all_results.begin(), all_results.end(),
              [](const TaskResult& a, const TaskResult& b) {
                  return a.task_id < b.task_id;
              });

    return seastar::async([filename, results = std::move(all_results)]() mutable {
        auto f = seastar::open_file_dma(filename,
            seastar::open_flags::wo | seastar::open_flags::create | seastar::open_flags::truncate).get();
        seastar::file_output_stream_options opts;
        opts.buffer_size = 4 * 1024 * 1024;
        auto out = seastar::make_file_output_stream(std::move(f), opts).get();
        char tmp[21];
        std::string line;
        line.reserve(128 * 1024);
        for (const auto& r : results) {
            line.clear();
            auto append_u64 = [&](uint64_t v) {
                char* end = util::fast_uint64_to_str(v, tmp);
                line.append(tmp, end - tmp);
            };
            append_u64(r.start);
            line.push_back('-');
            append_u64(r.end);
            line.push_back(',');
            append_u64(static_cast<uint64_t>(r.core_id));
            for (uint64_t prime : r.primes) {
                line.push_back(',');
                append_u64(prime);
            }
            line.push_back('\n');
            out.write(line.data(), line.size()).get();
        }
        out.flush().get();
        out.close().get();
    });
}

// 打印统计结果
void printStatistics(long duration_ms) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "         计算结果统计" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "已完成任务: " << g_completed_tasks.value.load(std::memory_order_relaxed) << "/" << g_num_tasks << std::endl;
    std::cout << "素数总数:   " << g_total_primes.value.load(std::memory_order_relaxed) << std::endl;
    std::cout << "计算耗时:   " << duration_ms << " ms" << std::endl;

    uint64_t total_numbers = static_cast<uint64_t>(g_num_tasks) * g_chunk_size;
    double prime_density = 100.0 * g_total_primes.value.load(std::memory_order_relaxed) / total_numbers;

    std::cout << "素数密度:   " << std::fixed << std::setprecision(4) << prime_density << "%" << std::endl;
    if (duration_ms > 0) {
        std::cout << "计算速度:   " << std::fixed << std::setprecision(0)
                  << static_cast<double>(total_numbers) / duration_ms << " 数/毫秒" << std::endl;
    } else {
        std::cout << "计算速度:   N/A (耗时太短)" << std::endl;
    }
    std::cout << "========================================" << std::endl;
}

// Seastar应用主函数
seastar::future<> seastar_main(const po::variables_map& config) {
    // 设置日志级别 - 默认error
    applog.set_level(seastar::log_level::error);

    // 从命令行参数获取配置
    g_num_tasks = config["tasks"].as<int>();
    g_chunk_size = config["chunk"].as<int>();

    if (g_num_tasks <= 0) g_num_tasks = 20;
    if (g_chunk_size <= 0) g_chunk_size = 100000;
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

    // 调用函数初始化任务队列
    initTaskQueue(g_num_tasks, g_chunk_size, g_num_cores);

    // 记录开始时间
    auto start_time = std::chrono::high_resolution_clock::now();

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
            return outputResults(output_file, g_num_cores).then([duration, start_time]() {
                // 打印统计结果
                printStatistics(duration.count());
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
