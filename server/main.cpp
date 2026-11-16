// RDR2 Co-op dedicated server.
//
//     rdr2coop-server [--port 27960] [--data ./rdr2coop-data] [--mode shared|host]
//                     [--password SECRET] [--max-players 8] [--barrier-timeout 120]
//                     [--no-barrier] [--no-shared-fail] [--memory]
//
// Players set  server = <this machine>:<port>  and a campaign name in RDR2Coop.ini.
// Campaign saves, the journal and the players are kept under the data folder.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "server.h"

static void usage(const char* exe) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--data DIR] [--mode shared|host] [--password P] [--max-players N]\n"
                 "          [--barrier-timeout SECONDS] [--no-barrier] [--no-shared-fail] [--memory]\n",
                 exe);
}

int main(int argc, char** argv) {
    int port = coop::kDefaultPort;
    std::string data = "rdr2coop-data";
    bool memory = false;
    coop::ServerConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") port = std::atoi(next());
        else if (a == "--data") data = next();
        else if (a == "--mode") {
            std::string m = next();
            if (m == "shared") config.storyMode = coop::StoryMode::Shared;
            else if (m == "host") config.storyMode = coop::StoryMode::HostWorld;
            else return usage(argv[0]), 2;
        } else if (a == "--password") config.password = next();
        else if (a == "--max-players") config.maxPlayers = std::atoi(next());
        else if (a == "--barrier-timeout") config.barrierTimeoutMs = static_cast<uint32_t>(std::atoi(next())) * 1000u;
        else if (a == "--no-barrier") config.cutsceneBarrier = false;
        else if (a == "--no-shared-fail") config.sharedFail = false;
        else if (a == "--memory") memory = true;
        else if (std::isdigit(static_cast<unsigned char>(a[0]))) port = std::atoi(a.c_str());  // "rdr2coop-server 27960" also works
        else return usage(argv[0]), 2;
    }
    if (port <= 0 || port > 65535 || config.maxPlayers < 1) return usage(argv[0]), 2;

    coop::UdpSocket socket;
    std::string error;
    if (!socket.open(static_cast<uint16_t>(port), &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::unique_ptr<coop::IServerStorage> storage;
    if (memory) storage = std::make_unique<coop::MemoryStorage>();
    else storage = std::make_unique<coop::FileStorage>(data);

    coop::Server server(socket, config, *storage);
    std::printf("RDR2 Co-op server on UDP port %d (protocol v%d), story mode %s, %s\n", port, coop::kProtocolVersion,
                config.storyMode == coop::StoryMode::Shared ? "shared" : "host world",
                memory ? "nothing stored" : ("data in " + data).c_str());
    std::fflush(stdout);

    auto start = std::chrono::steady_clock::now();
    std::vector<std::string> last;
    for (;;) {
        socket.waitReadable(5);
        auto elapsed = std::chrono::steady_clock::now() - start;
        server.pump(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
        std::vector<std::string> now = server.describe();
        if (now != last) {
            if (now.empty()) std::printf("no players\n");
            for (const auto& line : now) std::printf("%s\n", line.c_str());
            std::fflush(stdout);
            last = now;
        }
    }
}
