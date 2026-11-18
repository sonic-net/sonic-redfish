///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

// Audit record building blocks for the Redfish service.
//
// Standard library and POSIX only, so the same header serves bmcweb and the
// unit tests. Nothing here knows about HTTP or D-Bus types.

#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <string_view>

namespace redfish_audit
{

inline constexpr int schemaVersion = 1;
inline constexpr std::string_view invalidValue = "<invalid>";

// Escape a string for use inside a JSON string literal. Control bytes are
// written as \u00XX and non-ASCII bytes as '?', so a hostile value can
// neither end the string nor start a new log line.
inline std::string jsonEscape(std::string_view in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in)
    {
        auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (u < 0x20 || u == 0x7f)
        {
            std::array<char, 8> buf{};
            std::snprintf(buf.data(), buf.size(), "\\u%04x", u);
            out += buf.data();
        }
        else if (u >= 0x80)
        {
            out += '?';
        }
        else
        {
            out += c;
        }
    }
    return out;
}

// True for a short identifier-like value: letters, digits, '.', '_', '-'.
// Caller-supplied values must pass this before they are logged.
inline bool isSafeToken(std::string_view s, size_t maxLen = 64)
{
    if (s.empty() || s.size() > maxLen)
    {
        return false;
    }
    for (char c : s)
    {
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9');
        if (!alnum && c != '.' && c != '_' && c != '-')
        {
            return false;
        }
    }
    return true;
}

inline std::string safeToken(std::string_view s, size_t maxLen = 64)
{
    return isSafeToken(s, maxLen) ? std::string(s) : std::string(invalidValue);
}

inline bool isStateChanging(std::string_view method)
{
    return method == "POST" || method == "PATCH" || method == "PUT" ||
           method == "DELETE";
}

inline bool isRead(std::string_view method)
{
    return method == "GET" || method == "HEAD";
}

// Lets at most perSecond events through in each one-second window and counts
// the rest, so the next event let through can say how many were left out.
class RateLimiter
{
  public:
    explicit RateLimiter(unsigned perSecondIn) : perSecond(perSecondIn) {}

    bool allow(std::chrono::steady_clock::time_point now)
    {
        if (now - windowStart >= std::chrono::seconds(1))
        {
            windowStart = now;
            inWindow = 0;
        }
        if (inWindow >= perSecond)
        {
            ++suppressed;
            return false;
        }
        ++inWindow;
        return true;
    }

    // The number left out since the last call; resets the count.
    uint64_t takeSuppressed()
    {
        uint64_t count = suppressed;
        suppressed = 0;
        return count;
    }

  private:
    unsigned perSecond;
    std::chrono::steady_clock::time_point windowStart;
    unsigned inWindow = 0;
    uint64_t suppressed = 0;
};

// A request is recorded when it can change state, or when access was refused.
inline bool isAuditable(std::string_view method, unsigned status)
{
    return isStateChanging(method) || status == 401 || status == 403;
}

// SipHash-2-4: a keyed hash, used so that pseudonyms cannot be reversed or
// recomputed without the in-memory key.
inline uint64_t sipHash24(uint64_t k0, uint64_t k1, std::string_view data)
{
    auto rotl = [](uint64_t x, int b) { return (x << b) | (x >> (64 - b)); };
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;
    auto round = [&]() {
        v0 += v1;
        v1 = rotl(v1, 13);
        v1 ^= v0;
        v0 = rotl(v0, 32);
        v2 += v3;
        v3 = rotl(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = rotl(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = rotl(v1, 17);
        v1 ^= v2;
        v2 = rotl(v2, 32);
    };
    const size_t len = data.size();
    size_t i = 0;
    for (; i + 8 <= len; i += 8)
    {
        uint64_t m = 0;
        for (size_t j = 0; j < 8; ++j)
        {
            m |= static_cast<uint64_t>(static_cast<unsigned char>(data[i + j]))
                 << (8 * j);
        }
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    uint64_t b = static_cast<uint64_t>(len) << 56;
    for (size_t j = 0; i + j < len; ++j)
    {
        b |= static_cast<uint64_t>(static_cast<unsigned char>(data[i + j]))
             << (8 * j);
    }
    v3 ^= b;
    round();
    round();
    v0 ^= b;
    v2 ^= 0xff;
    round();
    round();
    round();
    round();
    return v0 ^ v1 ^ v2 ^ v3;
}

// Stable stand-ins for values that must not be logged (client address,
// session id). The same input gives the same output until the key changes.
class Pseudonymizer
{
  public:
    Pseudonymizer(uint64_t k0In, uint64_t k1In) : k0(k0In), k1(k1In) {}

    static Pseudonymizer random()
    {
        std::random_device rd;
        auto word = [&rd]() {
            return (static_cast<uint64_t>(rd()) << 32) | rd();
        };
        return {word(), word()};
    }

    std::string make(char prefix, std::string_view value) const
    {
        std::array<char, 16> buf{};
        std::snprintf(buf.data(), buf.size(), "%c-%08x", prefix,
                      static_cast<unsigned>(sipHash24(k0, k1, value) &
                                            0xffffffffU));
        return buf.data();
    }

  private:
    uint64_t k0;
    uint64_t k1;
};

struct Record
{
    std::string_view src = "bmcweb";
    std::string_view ev = "request";
    std::string user;
    std::string role;
    std::string auth;
    std::string sess;
    std::string client;
    std::string method;
    std::string uri;
    std::string action;
    // Pre-built JSON object text, e.g. {"ResetType":"On"}. Empty = omitted.
    std::string detail;
    std::string msg;
    unsigned status = 0;
    long long ms = -1;
    // syslog severity: 5 (notice) by default, 6 (info) for read requests
    int severity = 5;
};

inline std::string
    formatTimestamp(std::chrono::system_clock::time_point now)
{
    using namespace std::chrono;
    auto msSinceEpoch =
        duration_cast<milliseconds>(now.time_since_epoch()).count();
    auto secs = static_cast<std::time_t>(msSinceEpoch / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    std::array<char, 40> buf{};
    std::snprintf(buf.data(), buf.size(),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(msSinceEpoch % 1000));
    return buf.data();
}

// One record as a single line of JSON. Empty fields are left out.
inline std::string buildRecord(const Record& r, uint64_t seq,
                               std::chrono::system_clock::time_point now)
{
    std::string out = "{\"v\":" + std::to_string(schemaVersion);
    auto str = [&out](std::string_view key, std::string_view value) {
        if (value.empty())
        {
            return;
        }
        out += ",\"";
        out += key;
        out += "\":\"";
        out += jsonEscape(value);
        out += '"';
    };
    str("ts", formatTimestamp(now));
    out += ",\"seq\":" + std::to_string(seq);
    str("src", r.src);
    str("ev", r.ev);
    str("user", r.user);
    str("role", r.role);
    str("auth", r.auth);
    str("sess", r.sess);
    str("client", r.client);
    str("method", r.method);
    str("uri", r.uri);
    str("action", r.action);
    if (!r.detail.empty())
    {
        out += ",\"detail\":" + r.detail;
    }
    if (r.status != 0)
    {
        out += ",\"status\":" + std::to_string(r.status);
    }
    str("msg", r.msg);
    if (r.ms >= 0)
    {
        out += ",\"ms\":" + std::to_string(r.ms);
    }
    out += '}';
    return out;
}

// Sends records to the local syslog socket without ever blocking the caller.
// A record that cannot be sent is counted; the next successful send is
// preceded by a "gap" record carrying the count. Sequence numbers advance
// for dropped records too, so a reader can see where the loss happened.
class Sink
{
  public:
    explicit Sink(std::string pathIn = "/dev/log",
                  std::string identIn = "redfish-audit") :
        path(std::move(pathIn)), ident(std::move(identIn))
    {}

    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    ~Sink()
    {
        closeSocket();
    }

    bool write(const Record& r)
    {
        auto now = std::chrono::system_clock::now();
        if (pendingDrops > 0)
        {
            Record gap;
            gap.src = r.src;
            gap.ev = "gap";
            gap.detail = "{\"lost\":" + std::to_string(pendingDrops) + "}";
            if (!sendLine(buildRecord(gap, ++seq, now), gap.severity, now))
            {
                // The unsent gap notice gives its number to the lost record.
                ++pendingDrops;
                ++totalDrops;
                return false;
            }
            pendingDrops = 0;
        }
        if (!sendLine(buildRecord(r, ++seq, now), r.severity, now))
        {
            ++pendingDrops;
            ++totalDrops;
            return false;
        }
        return true;
    }

    uint64_t dropped() const
    {
        return totalDrops;
    }

    uint64_t sequence() const
    {
        return seq;
    }

  private:
    // authpriv: kept apart from ordinary daemon output
    static constexpr int facility = 10 << 3;

    bool openSocket()
    {
        if (fd >= 0)
        {
            return true;
        }
        sockaddr_un addr{};
        if (path.size() >= sizeof(addr.sun_path))
        {
            return false;
        }
        int s = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (s < 0)
        {
            return false;
        }
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::connect(s, reinterpret_cast<const sockaddr*>(&addr),
                      sizeof(addr)) != 0)
        {
            ::close(s);
            return false;
        }
        fd = s;
        return true;
    }

    void closeSocket()
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
    }

    bool sendLine(const std::string& json, int severity,
                  std::chrono::system_clock::time_point now)
    {
        if (!openSocket())
        {
            return false;
        }
        auto secs = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&secs, &tm);
        std::array<char, 32> stamp{};
        std::strftime(stamp.data(), stamp.size(), "%b %e %H:%M:%S", &tm);
        std::string line = "<" + std::to_string(facility | severity) + ">" +
                           stamp.data() + " " + ident + "[" +
                           std::to_string(::getpid()) + "]: " + json;
        ssize_t n = ::send(fd, line.data(), line.size(),
                           MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(line.size()))
        {
            return true;
        }
        if (errno != EAGAIN && errno != ENOBUFS)
        {
            // The listener went away; reconnect on the next record.
            closeSocket();
        }
        return false;
    }

    std::string path;
    std::string ident;
    int fd = -1;
    uint64_t seq = 0;
    uint64_t pendingDrops = 0;
    uint64_t totalDrops = 0;
};

} // namespace redfish_audit
