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
#include <latch>
#include <seastar/core/app-template.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/log.hh>

namespace ss = seastar;
static ss::logger app_log("sequential_test");

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

    int processed_since_yield = 0;
    const int YIELD_INTERVAL = 1000; // 每处理1000个数字让出一次控制权

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
        
        processed_since_yield++;
        if (processed_since_yield >= YIELD_INTERVAL) {
            ss::thread::yield();  // 让出控制权给事件循环
            processed_since_yield = 0;
        }
    }
    return count;
}

// 顺序计算测试
ss::future<int> test_sequential() {
    int max_number = 20000000;  // 与之前的测试相同
    
    app_log.info("开始顺序计算测试...");
    auto start_time = std::chrono::high_resolution_clock::now();
    
    size_t primes_count = count_primes_in_range(2, max_number);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    app_log.info("=== 顺序计算结果 ===");
    app_log.info("质数总数: {}", primes_count);
    app_log.info("计算耗时: {}ms", duration.count());
    
    return ss::make_ready_future<int>(0);
}

int main(int argc, char** argv) {
    return seastar::app_template().run(argc, argv, []() -> ss::future<int> {
        return test_sequential();
    });
}