/*
 * sonnet46_seastar_prime.cc
 *
 * Concurrently computes prime numbers over a given range using the Seastar
 * framework on a multi-core machine (designed for 32 cores).
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/log.hh>
#include <seastar/core/file.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/iostream.hh>

#include <algorithm>
#include <iomanip>
#include <atomic>
#include <string>
#include <vector>
#include <memory>

#include "prime_sieve.hpp"

static seastar::logger applog("seastar_prime");

struct Task {
    uint64_t start;
    uint64_t end;
};

struct TaskResult {
    uint64_t              start;
    uint64_t              end;
    unsigned              core_id;
    std::vector<uint64_t> primes;

    TaskResult() = default;
    TaskResult(uint64_t s, uint64_t e, unsigned c, std::vector<uint64_t> p) noexcept
        : start(s), end(e), core_id(c), primes(std::move(p)) {}
    TaskResult(TaskResult&&) noexcept = default;
    TaskResult& operator=(TaskResult&&) noexcept = default;
};

// ---------------------------------------------------------------------------
// C1: padded atomics — each on its own cache line to prevent false sharing
// ---------------------------------------------------------------------------
struct alignas(64) AlignedAtomicSize { std::atomic<size_t> value{0}; };

static AlignedAtomicSize g_next_task_idx;
static AlignedAtomicSize g_total_tasks;

// ---------------------------------------------------------------------------
// C2: per-core result vectors — no mutex needed, each core writes its own slot
// ---------------------------------------------------------------------------
constexpr size_t kMaxCores = 128;
struct alignas(64) PaddedResults { std::vector<TaskResult> results; };
static PaddedResults g_results_per_core[kMaxCores];

// ---------------------------------------------------------------------------
// CSV output using util::fast_uint64_to_str — no stringstream allocation
// ---------------------------------------------------------------------------
static seastar::future<> write_results_csv_async(const std::string& path,
                                                 std::vector<TaskResult> results) {
    // Sort by task start so rows appear in ascending numeric order
    std::sort(results.begin(), results.end(),
              [](const TaskResult& a, const TaskResult& b) {
                  return a.start < b.start;
              });

    return seastar::async([path, results = std::move(results)]() mutable {
        auto f = seastar::open_file_dma(path,
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
                char* end = ::util::fast_uint64_to_str(v, tmp);
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
    }).handle_exception([path](std::exception_ptr e) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            applog.error("Failed to write results to '{}': {}", path, ex.what());
        }
    });
}

// ---------------------------------------------------------------------------
// Dynamic task dispatch — lock-free atomic index, per-core result storage
// ---------------------------------------------------------------------------
static seastar::future<> dispatch_task(unsigned core_id,
                                       uint64_t range_start,
                                       uint64_t range_end_val,
                                       uint64_t interval,
                                       size_t total_tasks) {
    return seastar::repeat([core_id, range_start, range_end_val, interval, total_tasks]() mutable {
        // M1: relaxed memory order — only need atomicity, no ordering
        size_t task_idx = g_next_task_idx.value.fetch_add(1, std::memory_order_relaxed);

        if (task_idx >= total_tasks) [[unlikely]] {
            return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::yes);
        }

        uint64_t start = range_start + task_idx * interval;
        uint64_t end = std::min(start + interval, range_end_val);

        applog.debug("Core {} picked up task {} [{}, {})", core_id, task_idx, start, end);

        return seastar::async([start, end, core_id]() -> TaskResult {
            TaskResult r;
            r.start   = start;
            r.end     = end;
            r.core_id = core_id;
            r.primes  = prime::segmented_sieve(start, end);
            return r;
        }).then([core_id](TaskResult r) mutable {
            applog.debug("Core {} finished [{}, {}): {} primes found",
                         core_id, r.start, r.end, r.primes.size());
            // C2: per-core result — no mutex
            g_results_per_core[core_id].results.push_back(std::move(r));
            return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::no);
        });
    });
}

// ---------------------------------------------------------------------------
// Merge per-core results into a single sorted vector
// ---------------------------------------------------------------------------
static std::vector<TaskResult> merge_results(unsigned num_cores) {
    std::vector<TaskResult> all;
    size_t total = 0;
    for (unsigned i = 0; i < num_cores; ++i)
        total += g_results_per_core[i].results.size();
    all.reserve(total);
    for (unsigned i = 0; i < num_cores; ++i) {
        for (auto& r : g_results_per_core[i].results)
            all.push_back(std::move(r));
        g_results_per_core[i].results.clear();
    }
    return all;
}

// ---------------------------------------------------------------------------
// Application entry point (runs on shard 0 after Seastar initialises)
// ---------------------------------------------------------------------------
static seastar::future<>
app_main(const boost::program_options::variables_map& cfg) {

    // --- Read parameters ---
    uint64_t range_start;
    uint64_t range_end;
    uint64_t interval;
    std::string out_path;

    if (cfg.count("tasks") && cfg.count("chunk")) {
        int tasks = cfg["tasks"].as<int>();
        int chunk = cfg["chunk"].as<int>();
        if (tasks <= 0 || chunk <= 0) {
            applog.error("tasks and chunk must be positive; aborting");
            return seastar::make_ready_future<>();
        }
        range_start = 2;
        range_end = static_cast<uint64_t>(tasks) * chunk;
        interval = chunk;
    } else if (cfg.count("range-start") && cfg.count("range-end") && cfg.count("interval")) {
        range_start = cfg["range-start"].as<uint64_t>();
        range_end = cfg["range-end"].as<uint64_t>();
        interval = cfg["interval"].as<uint64_t>();
    } else {
        range_start = 2;
        range_end = 1'000'000;
        interval = 100'000;
    }

    if (cfg.count("output")) {
        out_path = cfg["output"].as<std::string>();
    } else {
        out_path = "primes.csv";
    }

    if (range_start >= range_end) [[unlikely]] {
        applog.error("range-start ({}) must be strictly less than range-end ({}); aborting", range_start, range_end);
        return seastar::make_ready_future<>();
    }

    if (interval == 0) [[unlikely]] {
        applog.error("interval is 0, which is invalid; defaulting to 100,000");
        interval = 100'000;
    }

    uint64_t total_numbers = range_end - range_start;
    size_t num_tasks = (total_numbers + interval - 1) / interval;

    g_next_task_idx.value.store(0, std::memory_order_relaxed);
    g_total_tasks.value.store(num_tasks, std::memory_order_relaxed);

    unsigned num_cores = seastar::smp::count;

    for (size_t i = 0; i < num_cores; ++i)
        g_results_per_core[i].results.clear();

    applog.info("Dispatching across {} cores; total tasks: {}", num_cores, num_tasks);

    std::vector<seastar::future<>> futures;
    futures.reserve(num_cores);
    for (unsigned c = 0; c < num_cores; ++c) {
        futures.push_back(seastar::smp::submit_to(c, [c, range_start, range_end, interval, num_tasks]() {
            return dispatch_task(c, range_start, range_end, interval, num_tasks);
        }));
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    return seastar::when_all(futures.begin(), futures.end())
    .then([out_path, start_time, num_cores, num_tasks, range_end](std::vector<seastar::future<>>) mutable {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // Merge per-core results
        auto all_results = merge_results(num_cores);

        // Compute stats before moving results
        size_t total_primes = 0;
        for (const auto& r : all_results)
            total_primes += r.primes.size();
        size_t all_size = all_results.size();
        uint64_t max_range = range_end;

        applog.info("All cores finished. Writing {} results.", all_size);
        return write_results_csv_async(out_path, std::move(all_results))
        .then([duration, num_tasks, all_size, total_primes, max_range]() {
            std::cout << "\n========================================" << std::endl;
            std::cout << "          计算结果统计" << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "已完成任务: " << all_size << "/" << num_tasks << std::endl;
            std::cout << "素数总数:   " << total_primes << std::endl;
            std::cout << "计算耗时:   " << duration.count() << " ms" << std::endl;

            double prime_density = 100.0 * total_primes / max_range;
            std::cout << "素数密度:   " << std::fixed << std::setprecision(4) << prime_density << "%" << std::endl;

            if (duration.count() > 0) [[likely]] {
                std::cout << "计算速度:   " << std::fixed << std::setprecision(0)
                          << static_cast<double>(max_range) / duration.count() << " 数/毫秒" << std::endl;
                std::cout << "素数发现率: " << std::fixed << std::setprecision(2)
                          << static_cast<double>(total_primes) / duration.count() << " 素数/毫秒" << std::endl;
            } else [[unlikely]] {
                std::cout << "计算速度:   N/A (耗时太短)" << std::endl;
                std::cout << "素数发现率: N/A (耗时太短)" << std::endl;
            }
            std::cout << "========================================" << std::endl;

            return seastar::make_ready_future<>();
        });
    });
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    seastar::app_template app;

    auto opts = app.add_options();
    opts
        ("tasks,t", boost::program_options::value<int>()->default_value(4), "Number of tasks")
        ("chunk,n", boost::program_options::value<int>()->default_value(100000), "Size of each partition (max 100000)")
        ("output,o", boost::program_options::value<std::string>()->default_value("primes.csv"), "Path for the output CSV file")
        ("range-start",
         boost::program_options::value<uint64_t>()->default_value(2),
         "Inclusive lower bound of the prime search range (legacy)")
        ("range-end",
         boost::program_options::value<uint64_t>()->default_value(1'000'000),
         "Exclusive upper bound of the prime search range (legacy)")
        ("interval",
         boost::program_options::value<uint64_t>()->default_value(100'000),
         "Width of each sub-task interval (clamped to 100,000) (legacy)");

    return app.run(argc, argv, [&app]() {
        return app_main(app.configuration());
    });
}
