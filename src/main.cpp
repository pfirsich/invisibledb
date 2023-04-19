#include <iostream>
#include <thread>

#include "invisibledb.hpp"

void client(std::string_view clientId, invisibledb::Server* server)
{
    invisibledb::SyncedMemoryRegion region(server, "test", 16);

    struct TestStruct {
        std::pmr::string str;
        unsigned int num;
    };

    region.lock();
    auto vec = region.getObject<std::pmr::vector<TestStruct>>("vec");
    region.unlock();

    unsigned int counter = 0;
    while (true) {
        region.lock();

        std::pmr::string name("clientId=", region.getAllocator());
        name.append(clientId);
        vec->push_back(TestStruct { std::move(name), counter++ });

        std::cout << "client " << clientId << "\n";
        for (const auto& elem : *vec) {
            std::cout << " " << elem.str << ": " << elem.num << "\n";
        }

        region.unlock();

        ::usleep(1000 * 100);

        if (counter >= 25) {
            break;
        }
    }
}

#include <thread>

int main(int, char**)
{
    invisibledb::Server server;
    server.connect("localhost:6969");

    std::thread t1 { [&server]() { client("FOOBAR", &server); } };
    ::usleep(1000 * 50);
    std::thread t2 { [&server]() { client("BAZBAZ", &server); } };

    t1.join();
    t2.join();
}
