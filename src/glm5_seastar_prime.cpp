// glm5_seastar_prime: 并行素数计算程序
// 调度策略：core 0 集中初始化任务数组 + 收集聚合结果，atomic counter 无锁分发
// 编译: ninja glm5_seastar_prime
// 运行: ./glm5_seastar_prime -t 100 -n 100000

#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/file.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/log.hh>
#include <boost/program_options.hpp>

#include <iostream>
#include <vector>
#include <iomanip>
#include <string>
#include <chrono>
#include <algorithm>
#include <atomic>

#include "prime_sieve.hpp"

namespace ss = seastar;
namespace po = boost::program_options;
static ss::logger app_log("glm5turbo_seastar");

struct Task {
    uint64_t start;
    uint64_t end;
};

struct TaskResult {
    uint64_t start;
    uint64_t end;
    unsigned int core_id;
    std::vector<uint64_t> primes;

    TaskResult() = default;
    TaskResult(uint64_t s, uint64_t e, unsigned int c, std::vector<uint64_t> p)
        : start(s), end(e), core_id(c), primes(std::move(p)) {}
    TaskResult(TaskResult&&) noexcept = default;
    TaskResult& operator=(TaskResult&&) noexcept = default;
};

// core 0 持有的任务存储 + atomic 分发计数器
class TaskStore {
    std::vector<Task> _tasks;
    std::atomic<size_t> _next{0};
public:
    void init(uint64_t max_num, uint64_t chunk_size) {
        uint64_t num_tasks = max_num / chunk_size;
        _tasks.clear();
        _tasks.reserve(num_tasks);
        for (uint64_t i = 0; i < num_tasks; ++i) {
            uint64_t start = (i == 0) ? 2 : i * chunk_size;
            uint64_t end = (i + 1) * chunk_size;
            _tasks.push_back({start, end});
        }
        _next.store(0, std::memory_order_relaxed);
    }

    struct slot { size_t begin; size_t count; };

    slot grab_batch(size_t n) {
        size_t begin = _next.fetch_add(n, std::memory_order_relaxed);
        if (begin >= _tasks.size()) return {begin, 0};
        size_t count = std::min(n, _tasks.size() - begin);
        return {begin, count};
    }

    const Task* data() const { return _tasks.data(); }
    size_t size() const { return _tasks.size(); }
};

constexpr size_t kMaxCores = 128;
struct alignas(64) PaddedResults { std::vector<TaskResult> results; };
static PaddedResults g_shard_results[kMaxCores];

// core 0 持有的全局状态
struct alignas(64) PaddedTaskStore { TaskStore store; };
static PaddedTaskStore g_task_store;

// Worker 循环：通过 atomic 从共享任务数组批量取任务，结果存本地
static ss::future<> worker_loop(unsigned shard_id) {
    return ss::repeat([shard_id] {
        TaskStore::slot s = g_task_store.store.grab_batch(32);
        if (s.count == 0) {
            return ss::make_ready_future<ss::stop_iteration>(ss::stop_iteration::yes);
        }
        const Task* tasks = g_task_store.store.data() + s.begin;
        auto& local = g_shard_results[shard_id].results;
        local.reserve(local.size() + s.count);
        for (size_t i = 0; i < s.count; ++i) {
            local.emplace_back(tasks[i].start, tasks[i].end, shard_id,
                               prime::segmented_sieve(tasks[i].start, tasks[i].end));
        }
        return ss::make_ready_future<ss::stop_iteration>(ss::stop_iteration::no);
    });
}

ss::future<> output_results(const std::string& filename, uint64_t max_num, long duration_ms) {
    unsigned num_cores = ss::smp::count;

    // core 0 聚合所有 shard 的结果
    std::vector<TaskResult> all_results;
    size_t total_primes = 0;
    {
        size_t total = 0;
        for (size_t i = 0; i < num_cores; ++i) total += g_shard_results[i].results.size();
        all_results.reserve(total);
        for (size_t i = 0; i < num_cores; ++i) {
            for (auto& r : g_shard_results[i].results) {
                total_primes += r.primes.size();
                all_results.push_back(std::move(r));
            }
            g_shard_results[i].results.clear();
        }
    }

    size_t total_tasks = g_task_store.store.size();

    std::sort(all_results.begin(), all_results.end(),
              [](const TaskResult& a, const TaskResult& b) {
                  return a.start < b.start;
              });

    return ss::async([filename, results = std::move(all_results)]() mutable {
        auto f = ss::open_file_dma(filename,
            ss::open_flags::wo | ss::open_flags::create | ss::open_flags::truncate).get();
        ss::file_output_stream_options opts;
        opts.buffer_size = 4 * 1024 * 1024;
        auto out = ss::make_file_output_stream(std::move(f), opts).get();
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
            append_u64(r.core_id);
            for (uint64_t prime : r.primes) {
                line.push_back(',');
                append_u64(prime);
            }
            line.push_back('\n');
            out.write(line.data(), line.size()).get();
        }
        out.flush().get();
        out.close().get();
    }).then([filename, max_num, duration_ms, total_primes, total_tasks]() {
        std::cout << "结果已写入: " << filename << std::endl;

        double prime_density = 100.0 * total_primes / max_num;
        std::cout << "\n========================================" << std::endl;
        std::cout << "         计算结果统计" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "已完成任务: " << total_tasks << "/" << total_tasks << std::endl;
        std::cout << "素数总数:   " << total_primes << std::endl;
        std::cout << "计算耗时:   " << duration_ms << " ms" << std::endl;
        std::cout << "素数密度:   " << std::fixed << std::setprecision(4) << prime_density << "%" << std::endl;
        if (duration_ms > 0) {
            std::cout << "计算速度:   " << std::fixed << std::setprecision(0)
                      << static_cast<double>(max_num) / duration_ms << " 数/毫秒" << std::endl;
            std::cout << "素数发现率: " << std::fixed << std::setprecision(2)
                      << static_cast<double>(total_primes) / duration_ms << " 素数/毫秒" << std::endl;
        } else {
            std::cout << "计算速度:   N/A (耗时太短)" << std::endl;
            std::cout << "素数发现率: N/A (耗时太短)" << std::endl;
        }
        std::cout << "========================================" << std::endl;
    });
}

ss::future<> seastar_main(const po::variables_map& config) {
    app_log.set_level(ss::log_level::error);

    int num_tasks = config["tasks"].as<int>();
    int chunk_size = config["chunk"].as<int>();
    std::string output_file = config["output"].as<std::string>();

    if (config.count("log-level")) {
        std::string level = config["log-level"].as<std::string>();
        if (level == "debug") app_log.set_level(ss::log_level::debug);
        else if (level == "info") app_log.set_level(ss::log_level::info);
        else if (level == "trace") app_log.set_level(ss::log_level::trace);
    }

    if (num_tasks <= 0) num_tasks = 20;
    if (chunk_size <= 0) chunk_size = 100000;

    uint64_t max_num = static_cast<uint64_t>(num_tasks) * chunk_size;

    auto start_time = std::chrono::high_resolution_clock::now();

    // core 0 初始化任务存储（分发任务）
    return ss::smp::submit_to(0, [max_num, chunk_size] {
        g_task_store.store.init(max_num, chunk_size);

        for (size_t i = 0; i < ss::smp::count; ++i) {
            g_shard_results[i].results.clear();
        }

        std::cout << "\n========================================" << std::endl;
        std::cout << "       任务队列初始化完成" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "计算范围: 2 - " << max_num << std::endl;
        std::cout << "总任务数: " << g_task_store.store.size() << std::endl;
        std::cout << "区间大小: " << chunk_size << std::endl;
        std::cout << "CPU核心数: " << ss::smp::count << std::endl;
        std::cout << "========================================\n" << std::endl;
        std::cout << "开始并行计算...\n" << std::endl;
        return ss::make_ready_future<>();

    }).then([] {
        std::vector<ss::future<>> futures;
        futures.reserve(ss::smp::count);
        for (unsigned i = 0; i < ss::smp::count; ++i) {
            futures.push_back(
                ss::smp::submit_to(i, [i] {
                    return worker_loop(i);
                })
            );
        }
        return ss::when_all(futures.begin(), futures.end()).discard_result();

    }).then([start_time, max_num, output_file] {
        // core 0 收集聚合结果
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        return output_results(output_file, max_num, duration.count());
    });
}

int main(int argc, char** argv) {
    ss::app_template app;

    app.add_options()
        ("tasks,t", po::value<int>()->default_value(20), "任务总数")
        ("chunk,n", po::value<int>()->default_value(100000), "每个任务的区间大小")
        ("output,o", po::value<std::string>()->default_value("glm5_seastar_prime.csv"), "输出CSV文件路径")
        ("log-level,l", po::value<std::string>(), "日志级别 (debug/info/error/trace)");

    return app.run(argc, argv, [&app] {
        return seastar_main(app.configuration());
    });
}
