#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/util/log.hh>
#include <seastar/util/backtrace.hh>
#include <cmath>
#include <vector>
#include <atomic>

namespace ss = seastar;
static ss::logger app_log("test_simple");

// 计算两个整数之间的素数
std::vector<int> find_primes(int start, int end) {
    if (start > end) {
        throw std::invalid_argument("起始值不能大于结束值");
    }
    if (start < 2) start = 2;
    
    std::vector<int> primes;
    
    // 处理特殊情况：2是唯一的偶素数
    if (start <= 2 && end >= 2) {
        primes.push_back(2);
    }

    // 从奇数开始检查，跳过所有偶数
    int actual_start = (start % 2 == 0) ? start + 1 : start;
    if (actual_start < 3) actual_start = 3;

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
        if (is_prime) primes.push_back(i);
    }
    return primes;
}


// 每个shard计算素数个数（map阶段）
ss::future<size_t> count_primes_on_shard(int shard_id) {
    return ss::async([shard_id] {
        const int numbers_per_shard = 100000;
        
        // 计算当前shard负责的区间
        int start = shard_id * numbers_per_shard + 1;
        int end = (shard_id + 1) * numbers_per_shard;
        
        auto primes = find_primes(start, end);
        //app_log.info("Shard {} [{}, {}]找到 {} 个素数", shard_id, start, end, primes.size());

        return primes.size();
    });
}

// 基于map-reduce的并行素数统计
ss::future<> async_task() {
    // 创建一个shard范围 [0, smp::count-1]
    boost::integer_range<int> shards(0, ss::smp::count);
    
    return ss::map_reduce(shards,
    // mapper函数 - 在每个shard上计算素数个数
    [](int shard_id) {
        //app_log.info("Shard {}/{} 启动", shard_id, ss::smp::count);
        return count_primes_on_shard(shard_id);
    },
    // reduce函数 - 累加所有shard的素数个数
    size_t(0),
    [](size_t total, size_t count) { return total + count; }
    ).then([](size_t total_primes) {
        app_log.info("=== 素数统计结果 ===");
        app_log.info("总共找到 {} 个素数", total_primes);
       
        return ss::make_ready_future<>();
    });
}

int main(int argc, char** argv) {
    ss::app_template app;
    
    try {
        return app.run(argc, argv, [] {
            app_log.info("程序启动");
            
            return async_task().then([] {
                app_log.info("任务完成");
                return ss::make_ready_future<>();
            }).handle_exception([] (std::exception_ptr eptr) {
                app_log.error("异常: {}", ss::current_backtrace());
                return ss::make_ready_future<>();
            });
        });
    } catch (...) {
        app_log.error("启动异常: {}", ss::current_backtrace());
        return 1;
    }
}
