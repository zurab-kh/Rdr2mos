// RDR2 Co-op server: the shared hub every player connects to.
//
// The game world itself (AI, physics, mission scripts) can only run inside a game, so one player's
// game simulates it: the "world owner". Everything else is the server's job:
//  * campaigns: players join one by name (with an optional password); several run side by side;
//  * picks the world owner and hands the world over when the owner leaves;
//  * passes every player's state to the others and the owner's world to everyone, and keeps the
//    latest world state so a player who joins sees it at once;
//  * routes damage to whoever owns the victim;
//  * story coordination: mission starts, cutscene barrier, shared mission failure, mission checks;
//  * keeps the campaign save (with history), the journal and the players on disk.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "link.h"
#include "protocol.h"
#include "server_storage.h"
#include "socket.h"

namespace coop {

struct ServerConfig {
    StoryMode storyMode = StoryMode::Shared;
    std::string password;          // empty = open server
    int maxPlayers = 8;            // per campaign
    bool cutsceneBarrier = true;
    uint32_t barrierTimeoutMs = 120000;
    bool sharedFail = true;
    float regroupDistance = 150.0f;
};

class Server {
public:
    Server(IDatagramSocket& socket, const ServerConfig& config, IServerStorage& storage);
    ~Server();

    // Receives, processes and sends everything. Call often (every few milliseconds).
    void pump(uint32_t nowMs);

    size_t playerCount() const;
    size_t campaignCount() const { return campaigns_.size(); }
    // One line per campaign: name, players, world owner.
    std::vector<std::string> describe() const;

private:
    class ClientSocket;
    struct Client;
    struct Campaign;

    void dispatch(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs);
    void admit(Client& c, uint32_t nowMs);
    void leave(Client& c, uint32_t nowMs, const char* why);
    void handle(Client& c, Msg type, const std::vector<uint8_t>& payload, uint32_t nowMs);
    void electOwner(Campaign& camp, uint32_t nowMs, uint8_t preferred);
    void sendRoster(Campaign& camp);
    void sendWorldTo(Campaign& camp, Client& c);
    void mergeSnapshot(Campaign& camp, const EntitySnapshot& snap);
    void storyTick(Campaign& camp, uint32_t nowMs);
    void command(Client& c, const StoryCommandMsg& cmd);
    void notice(Client& c, const std::string& text);
    void hold(Client& c, uint32_t nowMs, const std::string& why);
    void release(Client& c, const std::string& why);
    void pumpDownload(Client& c);
    void finishUpload(Client& c, Campaign& camp, uint32_t nowMs);
    Client* member(Campaign& camp, uint8_t id);
    void forward(Campaign& camp, const Client& from, Msg type, const std::vector<uint8_t>& payload, bool reliable,
                 bool prefix);

    IDatagramSocket& socket_;
    ServerConfig config_;
    IServerStorage& storage_;
    std::map<Endpoint, std::unique_ptr<Client>> clients_;
    std::map<std::string, std::unique_ptr<Campaign>> campaigns_;
    uint32_t lastStoryMs_ = 0, lastPersistMs_ = 0;
};

}  // namespace coop
