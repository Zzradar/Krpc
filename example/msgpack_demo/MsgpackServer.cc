#include "Krpcapplication.h"
#include "Krpcmsgpack_provider.h"

#include <chrono>
#include <string>
#include <thread>

int main(int argc, char **argv) {
    KrpcApplication::Init(argc, argv);

    KrpcMsgpackProvider provider;
    provider.Bind("Math", "Add", [](int a, int b) { return a + b; });
    provider.Bind("Math", "Slow", [](int ms) {
        if (ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        return ms;
    });
    provider.Bind("Echo", "Hello", [](const std::string &name) {
        return std::string("hi ") + name;
    });

    provider.Run();
    return 0;
}
