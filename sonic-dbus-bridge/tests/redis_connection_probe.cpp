#include "redis_connection.hpp"
#include <filesystem>
#include <string>

using sonic::dbus_bridge::connectRedisEndpoint;

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        return 64;
    }
    swss::SonicDBConfig::initialize(argv[1]);
    const std::string mode = argv[2];
    const std::string host = argv[3];
    const int port = std::stoi(argv[4]);
    const auto descriptorCount = [] {
        return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                             std::filesystem::directory_iterator());
    };
    const auto before = descriptorCount();
    for (int iteration = 0; iteration < (mode == "repeat" ? 100 : 1); ++iteration)
    {
        auto* context = connectRedisEndpoint("STATE_DB", host, port);
        if (!context)
        {
            return 2;
        }
        auto* selected = static_cast<redisReply*>(redisCommand(context, "SELECT 6"));
        const bool selectedOk = selected && selected->type != REDIS_REPLY_ERROR;
        if (selected) freeReplyObject(selected);
        if (!selectedOk) { redisFree(context); return 3; }

        auto* who = static_cast<redisReply*>(redisCommand(context, "ACL WHOAMI"));
        const std::string expected = mode == "writer" || mode == "repeat" ? "bridge-writer" : "default";
        const bool roleOk = who && who->type == REDIS_REPLY_STRING && who->str == expected;
        if (who) freeReplyObject(who);
        if (!roleOk) { redisFree(context); return 4; }

        auto* result = static_cast<redisReply*>(redisCommand(context, "SET bridge-probe works"));
        const bool writeOk = result && (mode == "reader" ?
            result->type == REDIS_REPLY_ERROR && std::string(result->str).starts_with("NOPERM") :
            result->type == REDIS_REPLY_STATUS);
        if (result) freeReplyObject(result);
        redisFree(context);
        if (!writeOk) return 5;
    }
    return before == descriptorCount() ? 0 : 6;
}
