// Standard library headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <numeric>
#include <iomanip>

// Third-party library headers
#include <libfork/core.hpp>
#include <libfork/schedule.hpp>

#include <seastar/core/app-template.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/log.hh>

namespace ss = seastar;
namespace lf = ::lf;
static ss::logger app_log("prime_bench");

// 全局任务计数器（线程安全）
std::atomic<int> next_task_id{0};
int total_tasks = 200;               // 总任务数（通过命令行参数设置）
int numbers_per_task = 100000;       // 每个任务处理的数字数量（通过命令行参数设置）

// 线程安全的任务获取函数
int get_next_task() {
    return next_task_id.fetch_add(1);
}

// 顺序计算质数（简单版，仅计数）
inline size_t count_primes_in_range(int start, int end) {
    if (start > end) return 0;
    if (start < 2) start = 2;

    size_t count = 0;

    if (start <= 2 && end >= 2) {
        count++;
    }

    int actual_start = (start % 2 == 0) ? start + 1 : start;
    if (actual_start < 3) actual_start = 3;

    for (int i = actual_start; i <= end; i += 2) {
        bool is_prime = true;
        int limit = static_cast<int>(std::sqrt(static_cast<double>(i)));

        for (int j = 3; j <= limit; j += 2) {
            if (i % j == 0) {
                is_prime = false;
                break;
            }
        }
        if (is_prime) count++;
    }
    return count;
}

// libfork 并行计算 - 使用 fork-join 模式处理任务
inline constexpr auto parallel_prime_count_libfork =
    [](auto self, int start_task, int end_task, int chunk_size) -> lf::task<size_t> {
    int num_tasks = end_task - start_task;
    
    if (num_tasks == 0) {
        co_return 0;
    }
    
    if (num_tasks == 1) {
        // 只有一个任务，直接计算
        int start = start_task * chunk_size + 1;
        int end = (start_task + 1) * chunk_size;
        co_return count_primes_in_range(start, end);
    }
    
    // 多个任务，分割为两部分
    int mid = start_task + num_tasks / 2;
    
    size_t left_count = 0;
    size_t right_count = 0;
    
    // 并行执行左右两部分
    co_await lf::fork[&left_count, self](start_task, mid, chunk_size);
    co_await lf::call[&right_count, self](mid, end_task, chunk_size);
    
    co_await lf::join;
    
    co_return left_count + right_count;
};

// 顺序计算质数（与test_sequential相同的功能）
ss::future<std::pair<size_t, long>> sequential_prime_count(int max_number) {
    return ss::async([max_number] {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        size_t primes_count = count_primes_in_range(2, max_number);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        app_log.info("=== 顺序计算结果 ===");
        app_log.info("质数总数: {}", primes_count);
        app_log.info("计算耗时: {}ms", duration.count());
        
        return std::make_pair(primes_count, duration.count());
    });
}

// 真正的工作窃取模式：每个shard独立处理任务，避免在shard 0上集中创建任务
ss::future<size_t> count_primes_on_shard(int shard_id) {
    return ss::async([shard_id] {
        size_t total_primes = 0;
        size_t tasks_completed = 0;

        // app_log.info("Shard {:2} 开始工作窃取", shard_id);

        // 真正的工作窃取：每个shard独立获取任务
        while (true) {
            int task_id = get_next_task();

            // 如果所有任务都已分配，停止
            if (task_id >= total_tasks) {
                break;
            }

            tasks_completed++;

            // 计算任务区间
            int start = task_id * numbers_per_task + 1;
            int end = (task_id + 1) * numbers_per_task;

            // 根据任务区间调整计算粒度（大数区间素数密度低，计算更快）
            int batch_size = 1000;
            if (start > 10000000) {
                // 大数区间素数密度低，可以加大批次
                batch_size = 2000;
            }

            // 分批处理，避免长时间阻塞Reactor
            size_t batch_primes = 0;
            for (int batch_start = start; batch_start <= end;
                 batch_start += batch_size) {
                int batch_end = std::min(batch_start + batch_size - 1, end);
                batch_primes += count_primes_in_range(batch_start, batch_end);

                // 每处理完一个批次就让出控制权
                ss::thread::yield();
            }

            total_primes += batch_primes;

            // 每完成5个任务输出一次进度
            // if (tasks_completed % 5 == 0) {
            //     app_log.info(
            //       "Shard {:2} 已完成 {:3} 个任务，当前累计素数: {:8}",
            //       shard_id,
            //       tasks_completed,
            //       total_primes);
            // }
        }

        return total_primes;
    });
}

// 基于工作窃取模式的并行素数统计
ss::future<> async_task(int total_tasks_param, int numbers_per_task_param) {
    // 设置总任务数
    total_tasks = total_tasks_param;
    // 设置每个任务处理的数字数量
    numbers_per_task = numbers_per_task_param;

    // 记录程序开始时间
    auto program_start = std::chrono::high_resolution_clock::now();

    app_log.info("=== 工作窃取模式启动 ===");
    app_log.info("总任务数: {}, 每个任务处理数字数: {}", total_tasks, numbers_per_task);
    app_log.info("总计算范围: [1, {}]", total_tasks * numbers_per_task);
    app_log.info("可用shard数量: {}", ss::smp::count);

    // 重置任务计数器
    next_task_id.store(0);

    // 包含所有shard（包括shard 0）
    std::vector<int> shards(ss::smp::count);
    std::iota(shards.begin(), shards.end(), 0);

    return ss::map_reduce(
             shards,
             // mapper函数 - 在每个shard上执行工作窃取
             [](int shard_id) {
                 // 将任务提交到目标shard执行，避免在shard 0上集中处理
                 return ss::smp::submit_to(shard_id, [shard_id] {
                     return count_primes_on_shard(shard_id);
                 });
             },
             // reduce函数 - 累加所有shard的素数个数
             size_t(0),
             [](size_t total, size_t count) { return total + count; })
      .then([program_start](size_t total_primes) {
          // 计算程序总耗时
          auto program_end = std::chrono::high_resolution_clock::now();
          auto program_duration
            = std::chrono::duration_cast<std::chrono::milliseconds>(
              program_end - program_start);

          // 计算素数密度
          double prime_density = static_cast<double>(total_primes)
                                 / (total_tasks * numbers_per_task) * 100;

          app_log.info("");
          app_log.info("=== 工作窃取模式统计结果 ===");
          app_log.info("总计算范围: [1, {}]", total_tasks * numbers_per_task);
          app_log.info("总任务数: {}", total_tasks);
          app_log.info("总共找到素数: {}", total_primes);
          app_log.info("素数密度: {:.6f}%", prime_density);
          app_log.info("程序总耗时: {}ms", program_duration.count());
          app_log.info("计算性能: {:.2f} 个数字/毫秒",
            static_cast<double>(total_tasks * numbers_per_task) / program_duration.count());
          app_log.info("素数发现率: {:.2f} 个素数/毫秒",
            static_cast<double>(total_primes) / program_duration.count());

          return ss::make_ready_future<>();
      });
}

// 执行 libfork 版本计算
size_t run_libfork_version(int num_tasks, int chunk_size, int num_threads) {
    lf::lazy_pool pool(static_cast<std::size_t>(num_threads));
    size_t primes = lf::sync_wait(pool, parallel_prime_count_libfork, 0, num_tasks, chunk_size);
    return primes;
}

// 执行顺序版本计算
size_t run_sequential_version(int max_number) {
    return count_primes_in_range(2, max_number);
}

// 执行三种版本的性能测试：Seastar、libfork、Sequential
ss::future<> compare_all_implementations(int num_tasks, int chunk_size) {
    int max_number = num_tasks * chunk_size;
    int num_threads = static_cast<int>(std::thread::hardware_concurrency());
    
    app_log.info("=== 综合性能比较测试 ===");
    app_log.info("计算范围: [1, {}]", max_number);
    app_log.info("任务数: {}", num_tasks);
    app_log.info("区间大小: {}", chunk_size);
    app_log.info("工作线程数: {}", num_threads);
    app_log.info("");
    
    // 存储三种实现的结果和耗时
    struct Result {
        size_t primes;
        long duration_ms;
        std::string name;
    };
    
    std::vector<Result> results;
    
    // 1. Seastar 版本
    app_log.info("开始 Seastar 并行计算...");
    auto seastar_start = std::chrono::high_resolution_clock::now();
    
    total_tasks = num_tasks;
    numbers_per_task = chunk_size;
    next_task_id.store(0);
    
    std::vector<int> shards(ss::smp::count);
    std::iota(shards.begin(), shards.end(), 0);
    
    size_t seastar_primes = 0;
    auto f = ss::map_reduce(
        shards,
        [](int shard_id) {
            return ss::smp::submit_to(shard_id, [shard_id] {
                return count_primes_on_shard(shard_id);
            });
        },
        size_t(0),
        [](size_t total, size_t count) { return total + count; })
    .then([&seastar_primes](size_t result) {
        seastar_primes = result;
    });
    
    f.wait();
    
    auto seastar_end = std::chrono::high_resolution_clock::now();
    auto seastar_duration = std::chrono::duration_cast<std::chrono::milliseconds>(seastar_end - seastar_start);
    
    results.push_back({seastar_primes, seastar_duration.count(), "Seastar"});
    
    app_log.info("=== Seastar 计算结果 ===");
    app_log.info("质数总数: {}", seastar_primes);
    app_log.info("计算耗时: {}ms", seastar_duration.count());
    app_log.info("");
    
    // 2. libfork 版本
    app_log.info("开始 libfork 并行计算...");
    auto libfork_start = std::chrono::high_resolution_clock::now();
    
    lf::lazy_pool pool(static_cast<std::size_t>(num_threads));
    size_t libfork_primes = lf::sync_wait(pool, parallel_prime_count_libfork, 0, num_tasks, chunk_size);
    
    auto libfork_end = std::chrono::high_resolution_clock::now();
    auto libfork_duration = std::chrono::duration_cast<std::chrono::milliseconds>(libfork_end - libfork_start);
    
    results.push_back({libfork_primes, libfork_duration.count(), "libfork"});
    
    app_log.info("=== libfork 计算结果 ===");
    app_log.info("质数总数: {}", libfork_primes);
    app_log.info("计算耗时: {}ms", libfork_duration.count());
    app_log.info("");
    
    // 3. 顺序版本
    app_log.info("开始顺序计算...");
    auto sequential_start = std::chrono::high_resolution_clock::now();
    size_t sequential_primes = count_primes_in_range(2, max_number);
    auto sequential_end = std::chrono::high_resolution_clock::now();
    auto sequential_duration = std::chrono::duration_cast<std::chrono::milliseconds>(sequential_end - sequential_start);
    
    results.push_back({sequential_primes, sequential_duration.count(), "Sequential"});
    
    app_log.info("=== 顺序计算结果 ===");
    app_log.info("质数总数: {}", sequential_primes);
    app_log.info("计算耗时: {}ms", sequential_duration.count());
    app_log.info("");
    
    // 结果比较和总结
    app_log.info("=== 性能比较总结 ===");
    app_log.info("计算范围: [1, {}]", max_number);
    app_log.info("任务数: {}, 区间大小: {}", num_tasks, chunk_size);
    app_log.info("");
    
    // 检查一致性
    bool all_consistent = true;
    for (size_t i = 1; i < results.size(); i++) {
        if (results[i].primes != results[0].primes) {
            all_consistent = false;
            break;
        }
    }
    app_log.info("结果一致性: {}", all_consistent ? "通过" : "失败");
    for (const auto& result : results) {
        app_log.info("  - {}: {}", result.name, result.primes);
    }
    app_log.info("");
    
    // 性能对比
    for (const auto& result : results) {
        app_log.info("{}: {}ms", result.name, result.duration_ms);
    }
    app_log.info("");
    
    // 计算加速比
    if (results[2].duration_ms > 0) {  // 以顺序版本为基准
        for (size_t i = 0; i < 2; i++) {
            double speedup = static_cast<double>(results[2].duration_ms) / results[i].duration_ms;
            app_log.info("{} 加速比: {:.2f}x", results[i].name, speedup);
            
            if (speedup > 1.0) {
                app_log.info("{} 比顺序计算快 {:.2f} 倍", results[i].name, speedup);
            } else if (speedup < 1.0) {
                app_log.info("{} 比顺序计算慢 {:.2f} 倍", results[i].name, 1.0 / speedup);
            }
        }
    }
    
    // 并行版本之间的对比（Seastar vs libfork）
    if (results[0].duration_ms > 0 && results[1].duration_ms > 0) {
        const double SIGNIFICANT_THRESHOLD = 0.05; // 5%的差异阈值
        
        double diff = ((results[0].duration_ms - results[1].duration_ms) / results[1].duration_ms) * 100;
        
        if (std::abs(diff) > SIGNIFICANT_THRESHOLD * 100) {
            if (diff > 0) {
                app_log.info("libfork 比 Seastar 快 {:.2f}%", diff);
            } else {
                app_log.info("Seastar 比 libfork 快 {:.2f}%", -diff);
            }
        } else {
            app_log.info("Seastar 和 libfork 性能相当");
        }
    }
    
    return ss::make_ready_future<>();
}

int main(int argc, char** argv) {
    // 使用 seastar 的 app_template 来解析部分参数，但使用自己的方式处理 -t 和 -n
    // 因为 seastar 的 app_template 和 libfork 参数有冲突
    
    if (argc < 2) {
        std::cout << "用法: " << argv[0] << " -t <任务数> -n <区间大小> [seastar 参数]\n";
        std::cout << "\n参数说明:\n";
        std::cout << "  -t, --tasks <N>         任务数 (默认: 4)\n";
        std::cout << "  -n, --chunk-size <N>    区间大小 (默认: 5000000)\n";
        std::cout << "\n说明: 程序将测试 Seastar、libfork 和 Sequential 三种实现\n";
        std::cout << "\n示例:\n";
        std::cout << "  " << argv[0] << " -t 8 -n 2500000\n";
        std::cout << "  " << argv[0] << " -t 4 -n 5000000 -c4 -m1G  (4个core, 1G内存限制)\n";
        return 1;
    }
    
    // 手动解析 -t 和 -n 参数
    int num_tasks = 4;
    int chunk_size = 5000000;
    
    // 创建新参数列表，去掉 -t 和 -n 供 seastar 使用
    std::vector<char*> seastar_args;
    seastar_args.push_back(argv[0]);
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-t" || arg == "--tasks") && i + 1 < argc) {
            num_tasks = std::atoi(argv[++i]);
        } else if ((arg == "-n" || arg == "--chunk-size") && i + 1 < argc) {
            chunk_size = std::atoi(argv[++i]);
        } else {
            seastar_args.push_back(argv[i]);
        }
    }
    
    if (num_tasks <= 0) num_tasks = 4;
    if (chunk_size <= 0) chunk_size = 5000000;
    
    // 将 seastar 参数转换回 char**
    std::vector<char*> seastar_argv = seastar_args;
    int seastar_argc = seastar_argv.size();
    
    // 运行 seastar 应用
    return seastar::app_template().run(seastar_argc, seastar_argv.data(), [num_tasks, chunk_size]() -> ss::future<int> {
        return ss::async([num_tasks, chunk_size] {
            // 比较三种实现的性能
            auto f = compare_all_implementations(num_tasks, chunk_size);
            f.wait();
            return 0;  // 成功返回
        }).handle_exception([](std::exception_ptr eptr) {
            app_log.error("异常: {}", seastar::current_backtrace());
            return 1;  // 错误返回
        });
    });
}