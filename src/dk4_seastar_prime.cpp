// dk4_seastar_prime: 并行素数计算程序 - 使用 Seastar 框架
// 工作模式：集中式任务队列 + seastar::async workers + per-shard 结果累积
// Worker 使用 seastar::async 在线程上下文中执行 CPU 密集型筛选
// 与其他程序保持接口兼容：-t tasks -n chunk-size -c cores -o output.csv

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

#include <vector>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstdint>
#include <algorithm>

#include "prime_sieve.hpp"

namespace po = boost::program_options;

static seastar::logger applog("dk4_prime");

struct task_result {
    uint64_t start;
    uint64_t end;
    unsigned shard_id;
    std::vector<uint64_t> primes;
};

// Padded atomic to prevent false sharing
struct alignas(64) AlignedAtomic { std::atomic<size_t> value{0}; };
static AlignedAtomic g_next_task;

// Per-shard result storage
constexpr size_t kMaxCores = 128;
struct alignas(64) PaddedResults { std::vector<task_result> results; };
static PaddedResults g_shard_results[kMaxCores];

static seastar::future<> worker_loop(unsigned shard_id, uint64_t range_start,
                                     uint64_t range_end, uint64_t interval,
                                     size_t total_tasks) {
    return seastar::repeat([shard_id, range_start, range_end, interval, total_tasks]() mutable {
        size_t idx = g_next_task.value.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total_tasks) [[unlikely]] {
            return seastar::make_ready_future<seastar::stop_iteration>(
                seastar::stop_iteration::yes);
        }

        uint64_t start = range_start + idx * interval;
        uint64_t end = std::min(start + interval, range_end);

        return seastar::async([start, end, shard_id]() -> task_result {
            return {start, end, shard_id, prime::segmented_sieve(start, end)};
        }).then([shard_id](task_result r) mutable {
            g_shard_results[shard_id].results.push_back(std::move(r));
            return seastar::make_ready_future<seastar::stop_iteration>(
                seastar::stop_iteration::no);
        });
    });
}

static seastar::future<> output_results(const std::string& filename,
                                         int num_tasks, int chunk_size,
                                         long duration_ms) {
    std::vector<task_result> all_results;
    size_t total_primes = 0;
    {
        size_t total = 0;
        unsigned num_cores = seastar::smp::count;
        for (size_t i = 0; i < num_cores; ++i) {
            total += g_shard_results[i].results.size();
        }
        all_results.reserve(total);
        for (size_t i = 0; i < num_cores; ++i) {
            for (auto& r : g_shard_results[i].results) {
                total_primes += r.primes.size();
                all_results.push_back(std::move(r));
            }
            g_shard_results[i].results.clear();
        }
    }

    std::sort(all_results.begin(), all_results.end(),
              [](const task_result& a, const task_result& b) {
                  return a.start < b.start;
              });

    uint64_t total_numbers = static_cast<uint64_t>(num_tasks) * chunk_size;
    std::cout << "\n========================================" << std::endl;
    std::cout << "          计算结果统计" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "已完成任务: " << all_results.size() << "/" << num_tasks << std::endl;
    std::cout << "素数总数:   " << total_primes << std::endl;
    std::cout << "计算耗时:   " << duration_ms << " ms" << std::endl;

    double prime_density = 100.0 * total_primes / total_numbers;
    std::cout << "素数密度:   " << std::fixed << std::setprecision(4)
              << prime_density << "%" << std::endl;

    if (duration_ms > 0) {
        std::cout << "计算速度:   " << std::fixed << std::setprecision(0)
                  << static_cast<double>(total_numbers) / duration_ms << " 数/毫秒" << std::endl;
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
            append_u64(r.start);
            line.push_back('-');
            append_u64(r.end);
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
    if (num_tasks <= 0) num_tasks = 32;
    if (chunk_size <= 0) chunk_size = 100000;

    if (config.count("log-level")) {
        std::string level = config["log-level"].as<std::string>();
        if (level == "debug") applog.set_level(seastar::log_level::debug);
        else if (level == "info") applog.set_level(seastar::log_level::info);
        else if (level == "trace") applog.set_level(seastar::log_level::trace);
    }

    std::string output_file = config.count("output")
        ? config["output"].as<std::string>()
        : "dk4_seastar_prime.csv";

    uint64_t range_start = 2;
    uint64_t range_end = static_cast<uint64_t>(num_tasks) * chunk_size;
    uint64_t interval = chunk_size;

    g_next_task.value.store(0, std::memory_order_relaxed);

    unsigned num_cores = seastar::smp::count;
    for (size_t i = 0; i < num_cores; ++i) {
        g_shard_results[i].results.clear();
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "          dk4_seastar_prime" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "计算范围: 2 - " << range_end << std::endl;
    std::cout << "总任务数: " << num_tasks << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "CPU核心数: " << num_cores << std::endl;
    std::cout << "========================================\n" << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<seastar::future<>> workers;
    workers.reserve(num_cores);
    for (unsigned i = 0; i < num_cores; ++i) {
        workers.push_back(
            seastar::smp::submit_to(i, [i, range_start, range_end, interval,
                                        total_tasks = static_cast<size_t>(num_tasks)] {
                return worker_loop(i, range_start, range_end, interval, total_tasks);
            })
        );
    }

    return seastar::when_all(workers.begin(), workers.end()).then(
        [num_tasks, chunk_size, output_file, start_time](std::vector<seastar::future<>> results) {
            for (auto& f : results) {
                if (f.failed()) {
                    return seastar::make_exception_future<>(f.get_exception());
                }
            }
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_time - start_time).count();
            return output_results(output_file, num_tasks, chunk_size, duration);
        }
    );
}

int main(int argc, char** argv) {
    seastar::app_template app;

    app.add_options()
        ("tasks,t", po::value<int>()->default_value(32), "任务总数")
        ("chunk,n", po::value<int>()->default_value(100000), "每个任务的区间大小")
        ("output,o", po::value<std::string>()->default_value("dk4_seastar_prime.csv"), "输出CSV文件路径")
        ("log-level,l", po::value<std::string>(), "日志级别 (trace/debug/info/warn/error)");

    return app.run(argc, argv, [&app] {
        return seastar_main(app.configuration());
    });
}
