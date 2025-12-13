#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/print.hh>
#include <iostream>

using namespace seastar;

int main(int argc, char** argv) {
    app_template app;
    
    app.add_options()(
        "help,h", "显示帮助信息");
    
    return app.run(argc, argv, [&app] {
        auto& args = app.configuration();
        
        if (args.count("help")) {
            std::cout << "简单测试程序\n";
            std::cout << "选项:\n";
            std::cout << "  -h, --help  显示此帮助信息\n";
            return make_ready_future<>();
        }
        
        std::cout << "程序运行中...\n";
        return make_ready_future<>();
    });
}
