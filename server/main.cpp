// RDR2 Co-op dedicated server.
//
//     rdr2coop-server [--port 27960] [--data ./rdr2coop-data] [--mode shared|host]
//                     [--password SECRET] [--max-players 8] [--barrier-timeout 120]
//                     [--no-barrier] [--no-shared-fail] [--memory]
//
// Players set  server = <this machine>:<port>  and a campaign name in RDR2Coop.ini.
// Campaign saves, the journal and the players are kept under the data folder.
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "server.h"

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

void usage(const char* exe) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--data DIR] [--mode shared|host] [--password P] [--max-players N]\n"
                 "          [--barrier-timeout SECONDS] [--no-barrier] [--no-shared-fail] [--memory]\n",
                 exe);
}

// A whole decimal number within [lo, hi], or false.
bool parseInt(const char* s, long lo, long hi, long& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (*end || v < lo || v > hi) return false;
    out = v;
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    long port = coop::kDefaultPort;
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
        auto number = [&](long lo, long hi) {
            long v = 0;
            const char* s = next();
            if (!parseInt(s, lo, hi, v)) {
                std::fprintf(stderr, "error: %s needs a number from %ld to %ld, not \"%s\"\n", a.c_str(), lo, hi, s);
                std::exit(2);
            }
            return v;
        };
        if (a == "--port") port = number(1, 65535);
        else if (a == "--data") data = next();
        else if (a == "--mode") {
            std::string m = next();
            if (m == "shared") config.storyMode = coop::StoryMode::Shared;
            else if (m == "host") config.storyMode = coop::StoryMode::HostWorld;
            else return usage(argv[0]), 2;
        } else if (a == "--password") config.password = next();
        else if (a == "--max-players") config.maxPlayers = static_cast<int>(number(1, coop::kMaxPlayers));
        else if (a == "--barrier-timeout") config.barrierTimeoutMs = static_cast<uint32_t>(number(1, 3600)) * 1000u;
        else if (a == "--no-barrier") config.cutsceneBarrier = false;
        else if (a == "--no-shared-fail") config.sharedFail = false;
        else if (a == "--memory") memory = true;
        else if (a == "--help" || a == "-h") return usage(argv[0]), 0;
        else if (!a.empty() && std::isdigit(static_cast<unsigned char>(a[0])) && parseInt(a.c_str(), 1, 65535, port)) {
            // "rdr2coop-server 27960" also works
        } else return usage(argv[0]), 2;
    }

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
    std::printf("RDR2 Co-op server on UDP port %ld (protocol v%d), story mode %s, %s%s\n", port, coop::kProtocolVersion,
                config.storyMode == coop::StoryMode::Shared ? "shared" : "host world",
                memory ? "nothing stored" : ("data in " + data).c_str(),
                config.password.empty() ? ", no password" : "");
    std::fflush(stdout);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto start = std::chrono::steady_clock::now();
    auto clock = [&] {
        auto elapsed = std::chrono::steady_clock::now() - start;
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    };
    uint32_t shown = server.revision() - 1;
    while (!g_stop) {
        socket.waitReadable(5);
        server.pump(clock());
        // Only when somebody joined, left or took the world over: describe() builds strings.
        if (server.revision() != shown) {
            shown = server.revision();
            std::vector<std::string> now = server.describe();
            if (now.empty()) std::printf("no players\n");
            for (const auto& line : now) std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
    }
    server.shutdown(clock());  // players hear it now instead of after a timeout
    coop::ServerStats s = server.stats();
    std::printf("stopped (%llu datagrams, %llu dropped, %llu rate limited, %llu resets, %llu reconnects on a new port or "
                "session, %llu stale hits)\n",
                static_cast<unsigned long long>(s.datagrams), static_cast<unsigned long long>(s.dropped),
                static_cast<unsigned long long>(s.rateLimited), static_cast<unsigned long long>(s.resets),
                static_cast<unsigned long long>(s.takeovers), static_cast<unsigned long long>(s.staleDamage));
    return 0;
}
