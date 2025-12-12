/**
 * @file parallel_prime_calculator.cpp
 * @brief 使用Seastar框架的并行素数计算程序
 *
 * 该程序利用Seastar的高并发特性，在20核计算机上高效计算20亿以内的素数。
 * 采用动态任务分配机制，确保负载均衡和最大化CPU利用率。
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/file.hh>
#include <seastar/core/future.hh>
#include <seastar/core/print.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/log.hh>

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <vector>

using namespace seastar;

// 日志记录器
static logger plog("parallel-prime");

// 常量定义
constexpr uint64_t MAX_NUMBER = 2000000000ULL;  // 20亿
constexpr uint64_t TASK_CHUNK_SIZE = 100000ULL; // 每个任务处理10万个数
constexpr uint64_t MAX_TASKS = MAX_NUMBER / TASK_CHUNK_SIZE + 1;

// 任务结构体
struct PrimeTask {
    uint64_t start;
    uint64_t end;
    uint32_t task_id;
    bool completed;

    PrimeTask(uint64_t s, uint64_t e, uint32_t id)
      : start(s)
      , end(e)
      , task_id(id)
      , completed(false) {
    }
};

// 任务结果结构体
struct TaskResult {
    uint64_t start;
    uint64_t end;
    uint32_t cpu_core;
    std::vector<uint64_t> primes;

    TaskResult(uint64_t s, uint64_t e, uint32_t core)
      : start(s)
      , end(e)
      , cpu_core(core) {
    }
};

// 全局任务队列管理器
class TaskQueueManager {
private:
    std::queue<std::shared_ptr<PrimeTask>> task_queue;
    mutable std::mutex queue_mutex;
    std::atomic<uint32_t> next_task_id{0};
    std::atomic<uint32_t> completed_tasks{0};
    std::atomic<uint32_t> total_tasks{0};

public:
    // 初始化任务队列
    future<> initialize_tasks() {
        return seastar::async([this] {
            uint64_t current_start = 2; // 素数从2开始
            uint32_t task_count = 0;

            while (current_start <= MAX_NUMBER) {
                uint64_t current_end = std::min(
                  current_start + TASK_CHUNK_SIZE - 1, MAX_NUMBER);

                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    task_queue.push(
                      std::make_shared<PrimeTask>(
                        current_start, current_end, next_task_id.fetch_add(1)));
                    task_count++;
                }

                current_start = current_end + 1;
            }

            total_tasks.store(task_count);
            plog.info("初始化完成: 共创建 {} 个任务", task_count);
        });
    }

    // 获取下一个任务（线程安全）
    std::shared_ptr<PrimeTask> get_next_task() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (task_queue.empty()) {
            return nullptr;
        }

        auto task = task_queue.front();
        task_queue.pop();
        return task;
    }

    // 标记任务完成
    void mark_task_completed() {
        completed_tasks.fetch_add(1);
    }

    // 获取进度
    float get_progress() const {
        uint32_t total = total_tasks.load();
        uint32_t completed = completed_tasks.load();
        return total > 0 ? (static_cast<float>(completed) / total) * 100.0f
                         : 0.0f;
    }

    // 检查是否所有任务都已完成
    bool all_tasks_completed() const {
        return completed_tasks.load() >= total_tasks.load();
    }

    // 获取剩余任务数
    uint32_t get_remaining_tasks() const {
        std::lock_guard<std::mutex> lock(queue_mutex);
        return static_cast<uint32_t>(task_queue.size());
    }
};

// 优化的素数检查函数（用于小范围）
bool is_prime_optimized(uint64_t n) {
    if (n <= 1)
        return false;
    if (n <= 3)
        return true;
    if (n % 2 == 0 || n % 3 == 0)
        return false;

    // 检查6k±1形式的数
    for (uint64_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) {
            return false;
        }
    }
    return true;
}

// 分段筛法实现（用于大范围）
std::vector<uint64_t> segmented_sieve(uint64_t start, uint64_t end) {
    std::vector<uint64_t> primes;

    // 如果范围很小，使用简单检查
    if (end - start <= TASK_CHUNK_SIZE) {
        for (uint64_t n = start; n <= end; ++n) {
            if (is_prime_optimized(n)) {
                primes.push_back(n);
            }
        }
        return primes;
    }

    // 计算sqrt(end)以内的素数
    uint64_t limit = static_cast<uint64_t>(std::sqrt(end)) + 1;
    std::vector<bool> is_prime_small(limit + 1, true);
    std::vector<uint64_t> small_primes;

    // 标准埃拉托斯特尼筛法
    for (uint64_t i = 2; i <= limit; ++i) {
        if (is_prime_small[i]) {
            small_primes.push_back(i);
            for (uint64_t j = i * i; j <= limit; j += i) {
                is_prime_small[j] = false;
            }
        }
    }

    // 分段筛法
    uint64_t segment_size = std::min(TASK_CHUNK_SIZE, end - start + 1);

    for (uint64_t low = start; low <= end; low += segment_size) {
        uint64_t high = std::min(low + segment_size - 1, end);
        std::vector<bool> segment(high - low + 1, true);

        // 使用小素数标记合数
        for (uint64_t prime : small_primes) {
            uint64_t lo_lim = std::max(
              prime * prime, (low + prime - 1) / prime * prime);
            for (uint64_t j = lo_lim; j <= high; j += prime) {
                if (j >= low) {
                    segment[j - low] = false;
                }
            }
        }

        // 收集素数
        for (uint64_t i = 0; i < segment.size(); ++i) {
            if (segment[i]) {
                uint64_t n = low + i;
                if (n >= 2 && is_prime_optimized(n)) {
                    primes.push_back(n);
                }
            }
        }
    }

    return primes;
}

// 处理单个任务
future<std::shared_ptr<TaskResult>>
process_task(std::shared_ptr<PrimeTask> task, uint32_t cpu_core) {
    return seastar::async([task, cpu_core] {
        auto start_time = std::chrono::high_resolution_clock::now();

        // 计算素数
        auto primes = segmented_sieve(task->start, task->end);

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          end_time - start_time);

        plog.debug(
          "CPU核心 {} 完成任务 {}: [{}, {}], 找到 {} 个素数, 耗时 {}ms",
          cpu_core,
          task->task_id,
          task->start,
          task->end,
          primes.size(),
          duration.count());

        auto result = std::make_shared<TaskResult>(
          task->start, task->end, cpu_core);
        result->primes = std::move(primes);

        return result;
    });
}

// 结果收集器
class ResultCollector {
private:
    std::vector<std::shared_ptr<TaskResult>> results;
    mutable std::mutex results_mutex;
    std::ofstream output_file;
    semaphore write_semaphore{1}; // 确保顺序写入

public:
    // 初始化输出文件
    future<> initialize_output(const std::string& filename) {
        return seastar::async([this, filename] {
            output_file.open(filename);
            if (!output_file.is_open()) {
                throw std::runtime_error("无法打开输出文件: " + filename);
            }

            // 写入CSV头部
            output_file << "task_range,cpu_core,primes\n";
            output_file.flush();

            plog.info("输出文件已初始化: {}", filename);
        });
    }

    // 添加结果（线程安全）
    future<> add_result(std::shared_ptr<TaskResult> result) {
        return write_semaphore.wait().then([this, result] {
            return seastar::async([this, result] {
                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    results.push_back(result);
                }

                // 写入CSV行
                std::stringstream ss;
                ss << result->start << "-" << result->end << ",";
                ss << result->cpu_core << ",";

                // 写入素数列表
                for (size_t i = 0; i < result->primes.size(); ++i) {
                    if (i > 0)
                        ss << ";";
                    ss << result->primes[i];
                }
                ss << "\n";

                output_file << ss.str();
                output_file.flush();

                write_semaphore.signal();
            });
        });
    }

    // 获取结果统计
    std::tuple<uint64_t, uint64_t> get_statistics() const {
        std::lock_guard<std::mutex> lock(results_mutex);

        uint64_t total_primes = 0;
        uint64_t max_primes_in_task = 0;

        for (const auto& result : results) {
            total_primes += result->primes.size();
            max_primes_in_task = std::max(
              max_primes_in_task, static_cast<uint64_t>(result->primes.size()));
        }

        return {total_primes, max_primes_in_task};
    }

    // 关闭输出文件
    void close_output() {
        if (output_file.is_open()) {
            output_file.close();
        }
    }

    ~ResultCollector() {
        close_output();
    }
};

// 工作线程函数
future<> worker_thread(
  TaskQueueManager& task_queue,
  ResultCollector& result_collector,
  uint32_t cpu_core) {
    return seastar::repeat([&task_queue, &result_collector, cpu_core] {
        // 获取下一个任务
        auto task = task_queue.get_next_task();
        if (!task) {
            // 没有更多任务
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        }

        // 处理任务
        return process_task(task, cpu_core)
          .then([&result_collector,
                 &task_queue](std::shared_ptr<TaskResult> result) {
              // 保存结果
              return result_collector.add_result(result).then([&task_queue] {
                  // 标记任务完成
                  task_queue.mark_task_completed();
                  return make_ready_future<>();
              });
          })
          .then([] {
              // 继续处理下一个任务
              return make_ready_future<stop_iteration>(stop_iteration::no);
          });
    });
}

// 进度监控函数
future<> progress_monitor(TaskQueueManager& task_queue) {
    return seastar::repeat([&task_queue] {
        return seastar::sleep(std::chrono::seconds(5)).then([&task_queue] {
            float progress = task_queue.get_progress();
            uint32_t remaining = task_queue.get_remaining_tasks();

            plog.info("进度: {:.2f}%, 剩余任务: {}", progress, remaining);

            if (task_queue.all_tasks_completed()) {
                return make_ready_future<stop_iteration>(stop_iteration::yes);
            }

            return make_ready_future<stop_iteration>(stop_iteration::no);
        });
    });
}

// 全局异常处理函数
void global_exception_handler() {
    std::set_terminate([]() {
        std::cerr << "\n=== Uncaught exception ===" << std::endl;
        std::cerr << "Stack trace:" << std::endl;
        std::cerr << seastar::current_backtrace() << std::endl;

        try {
            // 尝试重新抛出当前异常
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            std::cerr << "Exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown exception type" << std::endl;
        }

        std::_Exit(1);
    });
}

// 信号处理器
void setup_signal_handlers() {
    auto handler = [](int sig) {
        std::cerr << "\n=== Signal " << sig << " received ===" << std::endl;
        std::cerr << "Stack trace:" << std::endl;
        std::cerr << seastar::current_backtrace() << std::endl;
        std::_Exit(1);
    };

    signal(SIGSEGV, handler);
    signal(SIGABRT, handler);
    signal(SIGILL, handler);
    signal(SIGFPE, handler);
}

// 主函数
int main(int argc, char** argv) {
    app_template app;

    // 添加命名空间别名
    namespace bpo = boost::program_options;

    global_exception_handler();
    setup_signal_handlers();

    app.add_options()(
      "output,o",
      bpo::value<std::string>()->default_value("primes_output.csv"),
      "输出CSV文件名")("help,h", "显示帮助信息");
    try {
        return app.run(argc, argv, [&app] {
            auto& args = app.configuration();

            if (args.count("help")) {
                std::cout << "并行素数计算程序\n";
                std::cout << "计算20亿以内的所有素数\n";
                std::cout << "选项:\n";
                std::cout << "  -o, --output FILE  指定输出文件名 (默认: "
                             "primes_output.csv)\n";
                std::cout << "  -h, --help         显示此帮助信息\n";
                return make_ready_future<>();
            }

            std::string output_filename = args["output"].as<std::string>();

            plog.info("开始并行素数计算");
            plog.info("计算范围: 2 - {}", MAX_NUMBER);
            plog.info("任务大小: {} 个数/任务", TASK_CHUNK_SIZE);
            plog.info("CPU核心数: {}", smp::count);
            plog.info("输出文件: {}", output_filename);

            auto start_time = std::chrono::high_resolution_clock::now();

            // 创建任务队列管理器和结果收集器
            auto task_queue = std::make_unique<TaskQueueManager>();
            auto result_collector = std::make_unique<ResultCollector>();

            return task_queue->initialize_tasks()
              .then([&result_collector, output_filename] {
                  return result_collector->initialize_output(output_filename);
              })
              .then([task_queue = task_queue.get(),
                     result_collector = result_collector.get()] {
                  // 启动工作线程（每个CPU核心一个）
                  std::vector<future<>> worker_futures;
                  for (unsigned cpu_core = 0; cpu_core < smp::count;
                       ++cpu_core) {
                      worker_futures.push_back(
                        smp::submit_to(
                          cpu_core, [task_queue, result_collector, cpu_core] {
                              return worker_thread(
                                *task_queue, *result_collector, cpu_core);
                          }));
                  }

                  // 启动进度监控
                  auto progress_future = progress_monitor(*task_queue);

                  // 等待所有工作线程完成
                  return when_all_succeed(
                           worker_futures.begin(), worker_futures.end())
                    .then(
                      [progress_future = std::move(progress_future)]() mutable {
                          return std::move(progress_future);
                      });
              })
              .then([task_queue = task_queue.get(),
                     result_collector = result_collector.get(),
                     start_time] {
                  auto end_time = std::chrono::high_resolution_clock::now();
                  auto duration
                    = std::chrono::duration_cast<std::chrono::seconds>(
                      end_time - start_time);

                  // 获取统计信息
                  auto [total_primes, max_primes_in_task]
                    = result_collector->get_statistics();

                  plog.info("计算完成!");
                  plog.info("总耗时: {} 秒", duration.count());
                  plog.info("找到素数总数: {}", total_primes);
                  plog.info("单个任务最大素数数量: {}", max_primes_in_task);

                  return make_ready_future<>();
              })
              .handle_exception([](std::exception_ptr e) {
                  try {
                      std::rethrow_exception(e);
                  } catch (const std::exception& ex) {
                      plog.error("程序执行出错: {}", ex.what());
                  }
                  return make_ready_future<>();
              });
        });
    } catch (const std::exception& e) {
        std::cerr << "Exception in main: " << e.what() << std::endl;
        std::cerr << "Stack trace:" << std::endl;
        std::cerr << seastar::current_backtrace() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception in main" << std::endl;
        std::cerr << "Stack trace:" << std::endl;
        std::cerr << seastar::current_backtrace() << std::endl;
        return 1;
    }
}