/**
 * @file parallel_prime_calculator.cpp
 * @brief 使用Seastar框架的并行素数计算程序
 *
 * 该程序利用Seastar的高并发特性，在20核计算机上高效计算20亿以内的素数。
 * 采用动态任务分配机制，确保负载均衡和最大化CPU利用率。
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/log.hh>

#include <atomic>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>
#include <queue>
#include <mutex>
#include <iostream>
#include <string>

namespace ss = seastar;
static ss::logger app_log("test_simple");

// 工作窃取模式的任务队列
struct Task {
    int start;
    int end;
    int task_id;
};

// 全局任务队列（线程安全）
std::atomic<int> next_task_id{0};
int total_tasks = 200; // 总任务数（通过命令行参数设置）
const int numbers_per_task = 100000; // 每个任务处理的数字数量

// 线程安全的任务获取函数
int get_next_task() {
    return next_task_id.fetch_add(1);
}

// 计算两个整数之间的素数
std::vector<int> find_primes(int start, int end) {
    if (start > end) {
        throw std::invalid_argument("起始值不能大于结束值");
    }
    if (start < 2)
        start = 2;

    std::vector<int> primes;

    // 处理特殊情况：2是唯一的偶素数
    if (start <= 2 && end >= 2) {
        primes.push_back(2);
    }

    // 从奇数开始检查，跳过所有偶数
    int actual_start = (start % 2 == 0) ? start + 1 : start;
    if (actual_start < 3)
        actual_start = 3;

    for (int i = actual_start; i <= end; i += 2) {
        bool is_prime = true;
        int limit = static_cast<int>(std::sqrt(i));

        // 只检查奇数因子
        for (int j = 3; j <= limit; j += 2) {
            if (i % j == 0) {
                is_prime = false;
                break;
            }
        }
        if (is_prime)
            primes.push_back(i);
    }
    return primes;
}

// 真正的工作窃取模式：每个shard独立处理任务，避免在shard 0上集中创建任务
ss::future<size_t> count_primes_on_shard(int shard_id) {
    return ss::async([shard_id] {
        size_t total_primes = 0;
        size_t tasks_completed = 0;
        auto start_time = std::chrono::high_resolution_clock::now();
        
        //app_log.info("Shard {:2} 开始工作窃取", shard_id);
        
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
            for (int batch_start = start; batch_start <= end; batch_start += batch_size) {
                int batch_end = std::min(batch_start + batch_size - 1, end);
                batch_primes += find_primes(batch_start, batch_end).size();
                
                // 每处理完一个批次就让出控制权
                ss::thread::yield();
            }
            
            total_primes += batch_primes;
            
            // 每完成5个任务输出一次进度
            if (tasks_completed % 5 == 0) {
                app_log.info("Shard {:2} 已完成 {:3} 个任务，当前累计素数: {:8}", 
                            shard_id, tasks_completed, total_primes);
            }
        }
        
        // 计算执行时间
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        app_log.info("Shard {:2} 完成 {:3} 个任务，总计素数: {:8}, 耗时: {:6}ms, 平均每个任务: {:4}ms", 
                    shard_id, tasks_completed, total_primes, duration.count(), 
                    tasks_completed > 0 ? duration.count() / tasks_completed : 0);
        
        return total_primes;
    });
}

// 基于工作窃取模式的并行素数统计
ss::future<> async_task(int total_tasks_param) {
    // 设置总任务数
    total_tasks = total_tasks_param;
    
    // 记录程序开始时间
    auto program_start = std::chrono::high_resolution_clock::now();
    
    app_log.info("=== 工作窃取模式启动 ===");
    app_log.info("总任务数: {}, 每个任务处理数字数: {}", total_tasks, numbers_per_task);
    app_log.info("总计算范围: [1, {}]", total_tasks * numbers_per_task);
    app_log.info("可用shard数量: {}", ss::smp::count);
    
    // 重置任务计数器
    next_task_id.store(0);
    
    // 包含所有shard（包括shard 0）
    boost::integer_range<int> shards(0, ss::smp::count);

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
          auto program_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
              program_end - program_start);
          
          // 计算素数密度
          double prime_density = static_cast<double>(total_primes) / (total_tasks * numbers_per_task) * 100;
          
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

int main(int argc, char** argv) {
    ss::app_template app;
    
    // 添加命令行选项
    app.add_options()
        ("tasks,t", boost::program_options::value<int>()->default_value(200), "总任务数");

    try {
        return app.run(argc, argv, [&app] {
            auto& config = app.configuration();
            
            // 获取总任务数
            int task_count = config["tasks"].as<int>();
            
            if (task_count <= 0) {
                std::cerr << "错误: 任务数必须大于0\n";
                return ss::make_ready_future<>();
            }
            
            app_log.info("程序启动，总任务数: {}", task_count);

            return async_task(task_count)
              .then([] {
                  app_log.info("任务完成");
                  return ss::make_ready_future<>();
              })
              .handle_exception([](std::exception_ptr eptr) {
                  app_log.error("异常: {}", seastar::current_backtrace());
                  return ss::make_ready_future<>();
              });
        });
    } catch (...) {
        app_log.error("启动异常: {}", ss::current_backtrace());
        return 1;
    }
}
