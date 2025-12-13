#include <seastar/core/app-template.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/log.hh>

#include <atomic>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>

namespace ss = seastar;
static ss::logger app_log("test_simple");

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

// 每个shard计算素数个数（map阶段）
ss::future<size_t> count_primes_on_shard(int shard_id) {
    return ss::async([shard_id] {
        const int base_numbers = 100000000;
        
        // shard 0 不参与素数计算，只返回0
        if (shard_id == 0) {
            app_log.info("Shard {:2} 负责协调任务，不参与素数计算", shard_id);
            return size_t(0);
        }
        
        // 其他shard计算素数
        const int numbers_per_shard = base_numbers * 2;
        const int batch_size = 1000; // 分批处理，避免阻塞

        // 计算当前shard负责的区间
        // shard 1: [1, 1000000], shard 2: [1000001, 2000000], 以此类推
        int start = base_numbers * (shard_id - 1) + 1;
        int end = start + numbers_per_shard - 1;

        size_t total_primes = 0;
        
        // 记录开始时间
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // 分批处理，每批完成后检查是否需要yield
        for (int batch_start = start; batch_start <= end; batch_start += batch_size) {
            int batch_end = std::min(batch_start + batch_size - 1, end);
            
            auto primes = find_primes(batch_start, batch_end);
            total_primes += primes.size();
            
            // 每处理完一批，检查是否需要yield控制权
            if ((batch_start - start) % (batch_size * 10) == 0) {
                // 让出控制权，避免阻塞反应器
                ss::thread::yield();
            }
        }
        
        // 计算执行时间
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        app_log.info("Shard {:2} [{:8}, {:8}] 发现{:6}个素数，耗时{:5}ms", 
                     shard_id, start, end, total_primes, duration.count());
        return total_primes;
    });
}

// 基于map-reduce的并行素数统计
ss::future<> async_task() {
    // 记录协调开始时间
    auto coordination_start = std::chrono::high_resolution_clock::now();
    
    // 创建一个shard范围 [1, smp::count-1]（跳过shard 0）
    boost::integer_range<int> shards(1, ss::smp::count);

    return ss::map_reduce(
             shards,
             // mapper函数 - 在每个shard上计算素数个数
             [](int shard_id) {
                 // 将任务提交到目标shard执行，避免在shard 0上协调
                 return ss::smp::submit_to(shard_id, [shard_id] {
                     return count_primes_on_shard(shard_id);
                 });
             },
             // reduce函数 - 累加所有shard的素数个数
             size_t(0),
             [](size_t total, size_t count) { return total + count; })
      .then([coordination_start](size_t total_primes) {
          // 计算协调任务耗时
          auto coordination_end = std::chrono::high_resolution_clock::now();
          auto coordination_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
              coordination_end - coordination_start);
          
          app_log.info("=== 素数统计结果 ===");
          app_log.info("总共找到 {} 个素数", total_primes);
          app_log.info("协调任务耗时: {}ms", coordination_duration.count());

          return ss::make_ready_future<>();
      });
}

int main(int argc, char** argv) {
    ss::app_template app;

    try {
        return app.run(argc, argv, [] {
            app_log.info("程序启动");

            return async_task()
              .then([] {
                  app_log.info("任务完成");
                  return ss::make_ready_future<>();
              })
              .handle_exception([](std::exception_ptr eptr) {
                  app_log.error("异常: {}", seastar::current_backtrace());
                  // 执行解析
                  return ss::make_ready_future<>();
              });
        });
    } catch (...) {
        app_log.error("启动异常: {}", ss::current_backtrace());
        return 1;
    }
}
