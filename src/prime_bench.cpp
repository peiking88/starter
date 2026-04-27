// prime_bench: 素数计算性能基准测试
// 依次调用外部程序：sequence、minimax_libfork、glm5_libfork、minimax_seastar、sonnet46_seastar、kimi_seastar
// 使用 fork+execvp 替代 popen，避免 shell 注入

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct BenchmarkResult {
    std::string name;
    size_t primes = 0;
    long duration_ms = 0;
    int exit_status = 0;
};

static bool isProgramNameSafe(const std::string& program) {
    return !program.empty() &&
           std::all_of(program.begin(), program.end(),
               [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}

static BenchmarkResult runProgram(const std::string& program, const std::vector<std::string>& args) {
    BenchmarkResult result;
    result.name = program;

    if (!isProgramNameSafe(program)) {
        std::cerr << "错误: 非法程序名 " << program << std::endl;
        return result;
    }

    std::string path = "./build/release/" + program;
    std::vector<std::string> all_args;
    all_args.push_back(path);
    for (const auto& a : args) all_args.push_back(a);

    std::vector<char*> argv;
    for (auto& s : all_args) argv.push_back(s.data());
    argv.push_back(nullptr);

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::cerr << "错误: pipe 创建失败 for " << program << std::endl;
        return result;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        std::cerr << "错误: fork 失败 for " << program << std::endl;
        return result;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);

    FILE* pipe_out = fdopen(pipefd[0], "r");
    if (!pipe_out) {
        close(pipefd[0]);
        std::cerr << "错误: fdopen 失败 for " << program << std::endl;
        waitpid(pid, nullptr, 0);
        return result;
    }

    char buffer[4096];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe_out) != nullptr) {
        output += buffer;
    }
    fclose(pipe_out);

    int status;
    waitpid(pid, &status, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    if (WIFEXITED(status)) {
        result.exit_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_status = -WTERMSIG(status);
    }

    if (result.exit_status != 0) {
        std::cerr << "警告: " << program << " 异常退出 (status=" << result.exit_status << ")" << std::endl;
        std::cerr << "前200字符输出: " << output.substr(0, 200) << std::endl;
        return result;
    }

    if (output.find("素数总数") == std::string::npos) {
        std::cerr << "警告: " << program << " 输出中未找到'素数总数'" << std::endl;
        std::cerr << "前200字符输出: " << output.substr(0, 200) << std::endl;
    }

    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("素数总数:") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string num_str = line.substr(pos + 1);
                num_str.erase(std::remove_if(num_str.begin(), num_str.end(),
                    [](unsigned char c) { return std::isspace(c); }), num_str.end());
                try {
                    result.primes = std::stoull(num_str);
                } catch (const std::exception& e) {
                    std::cerr << "错误: 无法解析素数总数 '" << num_str << "' from " << program << std::endl;
                }
                break;
            }
        }
    }

    return result;
}

static void printSeparator(char c = '=', int width = 60) {
    std::cout << std::string(width, c) << std::endl;
}

static void printResults(const std::vector<BenchmarkResult>& results) {
    printSeparator();
    std::cout << "性能比较结果" << std::endl;
    printSeparator();

    std::cout << std::left << std::setw(24) << "框架"
              << std::right << std::setw(18) << "素数总数"
              << std::right << std::setw(12) << "耗时(ms)" << std::endl;
    printSeparator('-');

    for (const auto& r : results) {
        std::cout << std::left << std::setw(24) << r.name
                  << std::right << std::setw(18) << r.primes
                  << std::right << std::setw(12) << r.duration_ms << std::endl;
    }
    printSeparator('-');

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

    const BenchmarkResult* seq_result = nullptr;
    for (const auto& r : results) {
        if (r.name == "sequence_prime") {
            seq_result = &r;
            break;
        }
    }

    if (seq_result && seq_result->duration_ms > 0) {
        std::cout << "\n加速比（以sequence为基准）:" << std::endl;
        for (const auto& r : results) {
            if (r.name != "sequence_prime" && r.duration_ms > 0) {
                double speedup = static_cast<double>(seq_result->duration_ms) / r.duration_ms;
                std::cout << "  " << std::left << std::setw(20) << r.name
                          << ": " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
            }
        }
    }

    if (!results.empty()) {
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

int main(int argc, char** argv) {
    int num_tasks = 32;
    int chunk_size = 100000;
    int num_threads = 32;

    int opt;
    while ((opt = getopt(argc, argv, "t:n:c:h")) != -1) {
        switch (opt) {
            case 't':
                try { num_tasks = std::stoi(optarg); }
                catch (const std::exception&) { std::cerr << "错误: 无效的 -t 参数" << std::endl; return 1; }
                break;
            case 'n':
                try { chunk_size = std::stoi(optarg); }
                catch (const std::exception&) { std::cerr << "错误: 无效的 -n 参数" << std::endl; return 1; }
                break;
            case 'c':
                try { num_threads = std::stoi(optarg); }
                catch (const std::exception&) { std::cerr << "错误: 无效的 -c 参数" << std::endl; return 1; }
                break;
            case 'h':
            default:
                std::cout << "用法: " << argv[0] << " [-t 任务数] [-n 区间大小] [-c 线程数]\n" << std::endl;
                std::cout << "参数说明:" << std::endl;
                std::cout << "  -t <N>   任务总数 (默认: 32)" << std::endl;
                std::cout << "  -n <N>   区间大小 (默认: 100000)" << std::endl;
                std::cout << "  -c <N>   线程数 (默认: 32)" << std::endl;
                std::cout << "\n示例:" << std::endl;
                std::cout << "  " << argv[0] << " -t 10 -n 100000 -c 8" << std::endl;
                std::cout << "  " << argv[0] << " -t 20 -n 100000 -c 16" << std::endl;
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (num_tasks <= 0) num_tasks = 32;
    if (chunk_size <= 0) chunk_size = 100000;
    if (num_threads <= 0) num_threads = 32;

    std::vector<std::string> base_args = {
        "-t", std::to_string(num_tasks),
        "-n", std::to_string(chunk_size),
        "-c", std::to_string(num_threads)
    };

    std::vector<std::string> seastar_args = {
        "-c", std::to_string(num_threads),
        "-t", std::to_string(num_tasks),
        "-n", std::to_string(chunk_size),
        "--logger-ostream-type", "none"
    };

    printSeparator();
    std::cout << "素数计算性能基准测试" << std::endl;
    printSeparator();
    std::cout << "计算范围: 2 - " << static_cast<uint64_t>(num_tasks) * chunk_size << std::endl;
    std::cout << "任务数:   " << num_tasks << std::endl;
    std::cout << "区间大小: " << chunk_size << std::endl;
    std::cout << "线程数:   " << num_threads << std::endl;
    printSeparator();

    std::string output_dir = "./output";
    if (mkdir(output_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "警告: 无法创建输出目录: " << std::strerror(errno) << std::endl;
    }

    std::vector<BenchmarkResult> results;

    std::cout << "\n[1/8] 运行 sequence_prime (顺序计算)..." << std::endl;
    results.push_back(runProgram("sequence_prime", base_args));
    std::rename("sequence_prime.csv", "./output/sequence_prime.csv");

    std::cout << "[2/8] 运行 minimax_libfork_prime (libfork工作窃取)..." << std::endl;
    results.push_back(runProgram("minimax_libfork_prime", base_args));
    std::rename("minimax_libfork_prime.csv", "./output/minimax_libfork_prime.csv");

    std::cout << "[3/8] 运行 glm5_libfork_prime (libfork fork-join)..." << std::endl;
    results.push_back(runProgram("glm5_libfork_prime", base_args));
    std::rename("glm5_libfork_prime.csv", "./output/glm5_libfork_prime.csv");

    std::cout << "[4/8] 运行 minimax_seastar_prime (Seastar工作窃取)..." << std::endl;
    auto sargs4 = seastar_args;
    sargs4.push_back("-o");
    sargs4.push_back("./output/minimax_seastar_primes.csv");
    results.push_back(runProgram("minimax_seastar_prime", sargs4));

    std::cout << "[5/8] 运行 glm5_seastar_prime (Seastar框架)..." << std::endl;
    auto sargs5 = seastar_args;
    sargs5.push_back("-o");
    sargs5.push_back("./output/glm5_seastar_primes.csv");
    results.push_back(runProgram("glm5_seastar_prime", sargs5));

    std::cout << "[6/8] 运行 sonnet46_seastar_prime (Seastar分段筛法)..." << std::endl;
    auto sargs6 = seastar_args;
    sargs6.push_back("-o");
    sargs6.push_back("./output/sonnet46_seastar_primes.csv");
    results.push_back(runProgram("sonnet46_seastar_prime", sargs6));

    std::cout << "[7/8] 运行 kimi_seastar_prime (Seastar集中式队列)..." << std::endl;
    auto sargs7 = seastar_args;
    sargs7.push_back("-o");
    sargs7.push_back("./output/kimi_seastar_primes.csv");
    results.push_back(runProgram("kimi_seastar_prime", sargs7));

    std::cout << "[8/8] 运行 dk4_seastar_prime (Seastar集中式队列+async)..." << std::endl;
    auto sargs8 = seastar_args;
    sargs8.push_back("-o");
    sargs8.push_back("./output/dk4_seastar_primes.csv");
    results.push_back(runProgram("dk4_seastar_prime", sargs8));

    if (results.empty()) {
        std::cerr << "错误: 无基准测试结果" << std::endl;
        return 1;
    }

    printResults(results);

    return 0;
}
