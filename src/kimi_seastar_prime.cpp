// kimi_seastar_prime: 并行素数计算程序 - 使用 Seastar 框架
// 工作模式：集中式任务队列，atomic fetch_add 无锁分发
// Worker 直接从共享任务数组取任务，无需跨 shard 调度

#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/future.hh>
#include <seastar/core/do_with.hh>
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
#include <algorithm>
#include <atomic>
#include <memory>
#include <iomanip>
#include <chrono>
#include "prime_sieve.hpp"

namespace po = boost::program_options;

static seastar::logger applog("kimi_prime");

struct range_task {
    uint64_t start;
    uint64_t end;
};

struct task_result {
    range_task task;
    unsigned shard_id;
    std::vector<uint64_t> primes;
};

class task_queue {
    std::vector<range_task> _tasks;
    std::atomic<size_t> _next{0};
public:
    void add_task(range_task task) {
        _tasks.push_back(std::move(task));
    }

    struct slot { size_t begin; size_t count; };

    slot pop_tasks(size_t n) {
        size_t begin = _next.fetch_add(n, std::memory_order_relaxed);
        if (begin >= _tasks.size()) return {begin, 0};
        size_t count = std::min(n, _tasks.size() - begin);
        return {begin, count};
    }

    const range_task* data() const { return _tasks.data(); }
};

constexpr size_t kMaxCores = 128;
struct alignas(64) PaddedResults { std::vector<task_result> results; };
static PaddedResults g_shard_results[kMaxCores];

static seastar::future<> worker_loop(task_queue* queue, unsigned shard_id, size_t batch_size) {
    return seastar::repeat([queue, shard_id, batch_size] {
        task_queue::slot s = queue->pop_tasks(batch_size);
        if (s.count == 0) {
            return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::yes);
        }
        const range_task* tasks = queue->data() + s.begin;
        auto& local = g_shard_results[shard_id].results;
        local.reserve(local.size() + s.count);
        for (size_t i = 0; i < s.count; ++i) {
            local.push_back({tasks[i], shard_id, prime::segmented_sieve(tasks[i].start, tasks[i].end)});
        }
        return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::no);
    });
}

static std::unique_ptr<task_queue> initialize_task_queue(int num_tasks, int chunk_size) {
    auto queue = std::make_unique<task_queue>();
    for (int i = 0; i < num_tasks; ++i) {
        uint64_t start = (i == 0) ? 2 : static_cast<uint64_t>(i) * chunk_size;
        uint64_t end = static_cast<uint64_t>(i + 1) * chunk_size;
        queue->add_task({start, end});
    }
    applog.info("Initialized task queue with {} tasks (range: 2-{}, chunk_size: {})",
                num_tasks, static_cast<uint64_t>(num_tasks) * chunk_size, chunk_size);
    return queue;
}

static seastar::future<> output_results(const std::string& filename,
                                         int num_tasks, int chunk_size, long duration_ms) {
    // Aggregate per-shard results
    std::vector<task_result> all_results;
    size_t total_primes = 0;
    {
        size_t total = 0;
        unsigned num_cores = seastar::smp::count;
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

    // Sort by range start
    std::sort(all_results.begin(), all_results.end(),
              [](const task_result& a, const task_result& b) {
                  return a.task.start < b.task.start;
              });

    // Print statistics
    std::cout << "\n========================================" << std::endl;
    std::cout << "         计算结果统计" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "已完成任务: " << all_results.size() << "/" << num_tasks << std::endl;
    std::cout << "素数总数:   " << total_primes << std::endl;
    std::cout << "计算耗时:   " << duration_ms << " ms" << std::endl;

    uint64_t total_numbers = static_cast<uint64_t>(num_tasks) * chunk_size;
    double prime_density = 100.0 * total_primes / total_numbers;
    std::cout << "素数密度:   " << std::fixed << std::setprecision(4) << prime_density << "%" << std::endl;

    if (duration_ms > 0) {
        std::cout << "计算速度:   " << std::fixed << std::setprecision(0)
                  << static_cast<double>(total_numbers) / duration_ms << " 数/毫秒" << std::endl;
    } else {
        std::cout << "计算速度:   N/A (耗时太短)" << std::endl;
    }
    std::cout << "========================================" << std::endl;

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
            append_u64(r.task.start);
            line.push_back('-');
            append_u64(r.task.end);
            line.push_back(',');
            append_u64(r.shard_id);
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

static seastar::future<> seastar_main(const po::variables_map& config) {
    applog.set_level(seastar::log_level::error);

    int num_tasks = config["tasks"].as<int>();
    int chunk_size = config["chunk"].as<int>();
    unsigned num_cores = seastar::smp::count;

    if (num_tasks <= 0) num_tasks = 20;
    if (chunk_size <= 0) chunk_size = 100000;

    if (config.count("log-level")) {
        std::string level = config["log-level"].as<std::string>();
        if (level == "debug") applog.set_level(seastar::log_level::debug);
        else if (level == "info") applog.set_level(seastar::log_level::info);
        else if (level == "trace") applog.set_level(seastar::log_level::trace);
    }

    std::string output_file = config.count("output") ? config["output"].as<std::string>() : "kimi_seastar_prime.csv";

    // Clear per-shard results
    for (size_t i = 0; i < num_cores; ++i) {
        g_shard_results[i].results.clear();
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "任务队列初始化完成" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "总任务数: " << num_tasks << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "CPU核心数: " << num_cores << std::endl;
    std::cout << "========================================\n" << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();

    return seastar::do_with(
        initialize_task_queue(num_tasks, chunk_size),
        [num_tasks, chunk_size, output_file, start_time](std::unique_ptr<task_queue>& queue) {
            std::vector<seastar::future<>> workers;
            workers.reserve(seastar::smp::count);
            for (unsigned i = 0; i < seastar::smp::count; ++i) {
                workers.push_back(
                    seastar::smp::submit_to(i, [queue = queue.get(), i] {
                        return worker_loop(queue, i, 32);
                    })
                );
            }

            return seastar::when_all(workers.begin(), workers.end()).then(
                [num_tasks, chunk_size, output_file, start_time](std::vector<seastar::future<>> results) mutable {
                    for (auto& f : results) {
                        if (f.failed()) {
                            return seastar::make_exception_future<>(f.get_exception());
                        }
                    }
                    auto end_time = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                    return output_results(output_file, num_tasks, chunk_size, duration.count());
                }
            );
        }
    );
}

int main(int argc, char** argv) {
    seastar::app_template app;

    app.add_options()
        ("tasks,t", po::value<int>()->default_value(20), "任务总数")
        ("chunk,n", po::value<int>()->default_value(100000), "每个任务的区间大小")
        ("output,o", po::value<std::string>()->default_value("kimi_seastar_prime.csv"), "输出CSV文件路径")
        ("log-level,l", po::value<std::string>(), "日志级别 (debug/info/error/trace)");

    return app.run(argc, argv, [&app] {
        return seastar_main(app.configuration());
    });
}
