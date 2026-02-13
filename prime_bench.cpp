// prime_bench: 素数计算性能基准测试
// 依次调用外部程序：sequence、minimax_libfork、glm5_libfork、glm5_seastar、minimax_seastar

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <getopt.h>

// ============================================================================
// 配置
// ============================================================================
struct Config {
    int num_tasks = 4;       // 任务总数
    int chunk_size = 100000;   // 每个任务的区间大小
    int num_threads = 4;      // 使用线程数
};

Config g_config;

// ============================================================================
// 结果结构
// ============================================================================
struct BenchmarkResult {
    std::string name;
    size_t primes;
    long duration_ms;
};

// ============================================================================
// 运行外部程序并解析结果
// ============================================================================
BenchmarkResult runProgram(const std::string& program, const std::string& args) {
    BenchmarkResult result;
    result.name = program;
    
    // 构建命令
    std::string cmd = "./" + program + " " + args + " 2>/dev/null";
    
    // 记录开始时间
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 运行程序
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::cerr << "Error: 无法运行 " << program << std::endl;
        result.primes = result.duration_ms = 0;
        -1;
        return result;
    }
    
    // 读取输出
    char buffer[128];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    
    // 关闭管道
    pclose(pipe);
    
    // 记录结束时间
    auto end_time = std::chrono::high_resolution_clock::now();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    
    // 解析素数总数（从输出中提取）
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("素数总数:") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string num_str = line.substr(pos + 1);
                // 去除空格
                num_str.erase(std::remove_if(num_str.begin(), num_str.end(), ::isspace), num_str.end());
                result.primes = std::stoull(num_str);
                break;
            }
        }
    }
    
    return result;
}

// ============================================================================
// 打印分隔线
// ============================================================================
void printSeparator(char c = '=', int width = 60) {
    std::cout << std::string(width, c) << std::endl;
}

// ============================================================================
// 打印结果表格
// ============================================================================
void printResults(const std::vector<BenchmarkResult>& results) {
    printSeparator();
    std::cout << "性能比较结果" << std::endl;
    printSeparator();
    
    // 表头（使用固定宽度格式）
    printf("%-24s %18s %12s\n", "框架", "素数总数", "耗时(ms)");
    printSeparator('-');
    
    // 数据行
    for (const auto& r : results) {
        printf("%-24s %18lu %12ld\n", r.name.c_str(), r.primes, r.duration_ms);
    }
    printSeparator('-');
    
    // 结果一致性检查
    bool all_consistent = true;
    size_t expected_primes = results[0].primes;
    for (size_t i = 1; i < results.size(); ++i) {
        if (results[i].primes != expected_primes) {
            all_consistent = false;
            break;
        }
    }
    
    std::cout << "结果一致性: " << (all_consistent ? "✓ 通过" : "✗ 失败") << std::endl;
    printSeparator();
    
    // 加速比（以顺序版本为基准）
    // 找到顺序版本的结果
    const BenchmarkResult* seq_result = nullptr;
    for (const auto& r : results) {
        if (r.name == "sequence") {
            seq_result = &r;
            break;
        }
    }
    
    if (seq_result && seq_result->duration_ms > 0) {
        std::cout << "\n加速比（以sequence为基准）:" << std::endl;
        for (const auto& r : results) {
            if (r.name != "sequence" && r.duration_ms > 0) {
                double speedup = static_cast<double>(seq_result->duration_ms) / r.duration_ms;
                std::cout << "  " << std::left << std::setw(20) << r.name 
                          << ": " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
            }
        }
    }
    
    // 最快框架
    if (results.size() > 0) {
        auto fastest = std::min_element(results.begin(), results.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.duration_ms > 0 && b.duration_ms > 0 && a.duration_ms < b.duration_ms;
            });
        if (fastest != results.end() && fastest->duration_ms > 0) {
            std::cout << "\n最快框架: " << fastest->name 
                      << " (" << fastest->duration_ms << "ms)" << std::endl;
        }
    }
    
    printSeparator();
}

// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char** argv) {
    // 默认参数
    int num_tasks = 4;
    int chunk_size = 100000;
    int num_threads = 4;
    
    // 解析命令行参数
    int opt;
    while ((opt = getopt(argc, argv, "t:n:c:h")) != -1) {
        switch (opt) {
            case 't':
                num_tasks = std::atoi(optarg);
                break;
            case 'n':
                chunk_size = std::atoi(optarg);
                break;
            case 'c':
                num_threads = std::atoi(optarg);
                break;
            case 'h':
            default:
                std::cout << "用法: " << argv[0] << " [-t 任务数] [-n 区间大小] [-c 线程数]\n" << std::endl;
                std::cout << "参数说明:" << std::endl;
                std::cout << "  -t <N>   任务总数 (默认: 4)" << std::endl;
                std::cout << "  -n <N>   区间大小，每任务计算的数字范围 (默认: 100000)" << std::endl;
                std::cout << "  -c <N>   线程数 (默认: 4)" << std::endl;
                std::cout << "\n示例:" << std::endl;
                std::cout << "  " << argv[0] << " -t 10 -n 100000 -c 8" << std::endl;
                std::cout << "  " << argv[0] << " -t 20 -n 100000 -c 16" << std::endl;
                return (opt == 'h') ? 0 : 1;
        }
    }
    
    // 参数校验
    if (num_tasks <= 0) num_tasks = 4;
    if (chunk_size <= 0) chunk_size = 100000;
    if (num_threads <= 0) num_threads = 4;
    
    // 构建参数字符串
    std::ostringstream args;
    args << "-t " << num_tasks << " -n " << chunk_size << " -c " << num_threads;
    std::string args_str = args.str();
    
    // 打印配置信息
    printSeparator();
    std::cout << "素数计算性能基准测试" << std::endl;
    printSeparator();
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "任务数:   " << num_tasks << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "线程数:   " << num_threads << std::endl;
    printSeparator();
    
    // 存储结果
    std::vector<BenchmarkResult> results;
    
    // 1. 运行 sequence 版本
    std::cout << "\n[1/5] 运行 sequence (顺序计算)..." << std::endl;
    results.push_back(runProgram("sequence_prime", args_str));
    
    // 2. 运行 minimax_libfork 版本
    std::cout << "[2/5] 运行 minimax_libfork (libfork工作窃取)..." << std::endl;
    results.push_back(runProgram("minimax_libfork_prime", args_str));
    
    // 3. 运行 glm5_libfork 版本
    std::cout << "[3/5] 运行 glm5_libfork (libfork fork-join)..." << std::endl;
    results.push_back(runProgram("glm5_libfork_prime", args_str));
    
    // 4. 运行 glm5_seastar 版本
    std::cout << "[4/5] 运行 glm5_seastar (Seastar框架)..." << std::endl;
    // Seastar 版本使用 -t 和 -n 参数，-c 是给 seastar 用的
    // 添加 --logger-ostream-type none 关闭 Seastar 日志
    std::ostringstream seastar_args;
    seastar_args << "-t " << num_tasks << " -n " << chunk_size << " -c" << num_threads 
                 << " --logger-ostream-type none";
    results.push_back(runProgram("glm5_seastar_prime", seastar_args.str()));
    
    // 5. 运行 minimax_seastar 版本
    std::cout << "[5/5] 运行 minimax_seastar (Seastar工作窃取)..." << std::endl;
    // minimax_seastar_prime 使用环境变量，需要构建环境变量参数
    std::ostringstream minimax_seastar_args;
    minimax_seastar_args << "--logger-ostream-type none";
    std::string minimax_seastar_env = "NUM_TASKS=" + std::to_string(num_tasks) + 
                                       " CHUNK_SIZE=" + std::to_string(chunk_size) + 
                                       " NUM_CORES=" + std::to_string(num_threads);
    std::string minimax_seastar_cmd = minimax_seastar_env + " ./minimax_seastar_prime " + minimax_seastar_args.str();
    
    BenchmarkResult minimax_seastar_result;
    minimax_seastar_result.name = "minimax_seastar";
    
    auto start_time = std::chrono::high_resolution_clock::now();
    FILE* pipe = popen(minimax_seastar_cmd.c_str(), "r");
    if (pipe) {
        char buffer[128];
        std::string output;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        pclose(pipe);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        minimax_seastar_result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        
        // 解析素数总数
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("素数总数:") != std::string::npos) {
                size_t pos = line.find(":");
                if (pos != std::string::npos) {
                    std::string num_str = line.substr(pos + 1);
                    num_str.erase(std::remove_if(num_str.begin(), num_str.end(), ::isspace), num_str.end());
                    minimax_seastar_result.primes = std::stoull(num_str);
                    break;
                }
            }
        }
    }
    results.push_back(minimax_seastar_result);
    
    // 打印结果
    printResults(results);
    
    return 0;
}
