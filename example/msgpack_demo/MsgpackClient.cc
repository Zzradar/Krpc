#include "Krpcapplication.h"
#include "Krpcmsgpack_channel.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    KrpcApplication::Init(argc, argv);

    KrpcMsgpackChannel channel;
    int sum = channel.Call<int>("Math", "Add", 3, 4);
    std::string hello = channel.Call<std::string>("Echo", "Hello", std::string("krpc"));

    std::cout << "sum=" << sum << std::endl;
    std::cout << "hello=" << hello << std::endl;

    try {
        int slow_ms = channel.Call<int>("Math", "Slow", 2000);
        std::cout << "slow=" << slow_ms << std::endl;
    } catch (const std::exception &e) {
        std::cout << "slow failed: " << e.what() << std::endl;
    }

    return 0;
}
