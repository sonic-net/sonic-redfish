///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "redfish_audit.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <string>

#include <gtest/gtest.h>

using namespace redfish_audit;

namespace
{

// A datagram listener standing in for the syslog daemon.
class Listener
{
  public:
    explicit Listener(const std::string& pathIn) : path(pathIn)
    {
        ::unlink(path.c_str());
        fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
                      path.c_str());
        EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
                  0);
    }
    ~Listener()
    {
        ::close(fd);
        ::unlink(path.c_str());
    }
    std::string receive()
    {
        std::array<char, 4096> buf{};
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        return n > 0 ? std::string(buf.data(), static_cast<size_t>(n))
                     : std::string();
    }

  private:
    std::string path;
    int fd = -1;
};

std::string socketPath(const char* name)
{
    return "/tmp/redfish_audit_test_" + std::to_string(::getpid()) + "_" +
           name;
}

const std::chrono::system_clock::time_point fixedTime{
    std::chrono::milliseconds(1794820447412LL)};

} // namespace

TEST(JsonEscape, QuotesBackslashAndControlBytes)
{
    EXPECT_EQ(jsonEscape("a\"b\\c"), "a\\\"b\\\\c");
    EXPECT_EQ(jsonEscape("line1\nline2"), "line1\\u000aline2");
    EXPECT_EQ(jsonEscape(std::string_view("a\0b", 3)), "a\\u0000b");
    EXPECT_EQ(jsonEscape("caf\xc3\xa9"), "caf??");
}

TEST(SafeToken, AcceptsIdentifiersOnly)
{
    EXPECT_TRUE(isSafeToken("PowerCycle"));
    EXPECT_TRUE(isSafeToken("Base.1.19.ActionParameterNotSupported"));
    EXPECT_TRUE(isSafeToken("user_name-1"));
    EXPECT_FALSE(isSafeToken(""));
    EXPECT_FALSE(isSafeToken("has space"));
    EXPECT_FALSE(isSafeToken("a\"}\n{\"ev\":\"request\""));
    EXPECT_FALSE(isSafeToken("10.0.0.1:443"));
    EXPECT_FALSE(isSafeToken(std::string(65, 'a')));
    EXPECT_EQ(safeToken("On"), "On");
    EXPECT_EQ(safeToken("On; rm -rf"), "<invalid>");
}

TEST(Auditable, StateChangingOrRefused)
{
    EXPECT_TRUE(isAuditable("POST", 204));
    EXPECT_TRUE(isAuditable("PATCH", 400));
    EXPECT_TRUE(isAuditable("PUT", 200));
    EXPECT_TRUE(isAuditable("DELETE", 404));
    EXPECT_FALSE(isAuditable("GET", 200));
    EXPECT_FALSE(isAuditable("GET", 404));
    EXPECT_FALSE(isAuditable("HEAD", 200));
    EXPECT_TRUE(isAuditable("GET", 401));
    EXPECT_TRUE(isAuditable("GET", 403));
}

TEST(Pseudonymizer, StablePerKeyAndKeyDependent)
{
    Pseudonymizer a(1, 2);
    Pseudonymizer b(3, 4);
    EXPECT_EQ(a.make('c', "192.0.2.10"), a.make('c', "192.0.2.10"));
    EXPECT_NE(a.make('c', "192.0.2.10"), a.make('c', "192.0.2.11"));
    EXPECT_NE(a.make('c', "192.0.2.10"), b.make('c', "192.0.2.10"));
    std::string p = a.make('s', "abc");
    EXPECT_EQ(p.size(), 10U);
    EXPECT_EQ(p.substr(0, 2), "s-");
    EXPECT_EQ(p.find("abc"), std::string::npos);
}

TEST(SipHash, ReferenceVector)
{
    // Key 00..0f, message 00..0e: the published SipHash-2-4 test vector.
    std::string msg;
    for (char i = 0; i < 15; ++i)
    {
        msg.push_back(i);
    }
    EXPECT_EQ(sipHash24(0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL, msg),
              0xa129ca6149be45e5ULL);
}

TEST(BuildRecord, FullRecord)
{
    Record r;
    r.user = "admin";
    r.role = "Administrator";
    r.auth = "session";
    r.sess = "s-000041c2";
    r.client = "c-00009be0";
    r.method = "POST";
    r.uri = "/redfish/v1/Systems/<str>/Actions/ComputerSystem.Reset";
    r.action = "ComputerSystem.Reset";
    r.detail = "{\"ResetType\":\"PowerCycle\"}";
    r.status = 204;
    r.ms = 38;
    EXPECT_EQ(
        buildRecord(r, 118, fixedTime),
        "{\"v\":1,\"ts\":\"2026-11-16T09:14:07.412Z\",\"seq\":118,"
        "\"src\":\"bmcweb\",\"ev\":\"request\","
        "\"user\":\"admin\",\"role\":\"Administrator\",\"auth\":\"session\","
        "\"sess\":\"s-000041c2\",\"client\":\"c-00009be0\","
        "\"method\":\"POST\","
        "\"uri\":\"/redfish/v1/Systems/<str>/Actions/ComputerSystem.Reset\","
        "\"action\":\"ComputerSystem.Reset\","
        "\"detail\":{\"ResetType\":\"PowerCycle\"},\"status\":204,\"ms\":38}");
}

TEST(BuildRecord, OmitsEmptyFieldsAndStaysOnOneLine)
{
    Record r;
    r.ev = "denied";
    r.method = "GET";
    r.user = "evil\"\n{\"ev\":\"request\"}";
    r.status = 401;
    std::string line = buildRecord(r, 1, fixedTime);
    EXPECT_EQ(line.find('\n'), std::string::npos);
    EXPECT_EQ(line.find("\"role\""), std::string::npos);
    EXPECT_EQ(line.find("\"ms\""), std::string::npos);
    EXPECT_NE(line.find("\"user\":\"evil\\\"\\u000a{\\\"ev\\\""),
              std::string::npos);
}

TEST(Sink, DeliversSyslogFramedRecord)
{
    std::string path = socketPath("deliver");
    Listener listener(path);
    Sink sink(path);
    Record r;
    r.action = "ComputerSystem.Reset";
    EXPECT_TRUE(sink.write(r));
    std::string line = listener.receive();
    EXPECT_EQ(line.rfind("<85>", 0), 0U);
    EXPECT_NE(line.find(" redfish-audit["), std::string::npos);
    EXPECT_NE(line.find("]: {\"v\":1,"), std::string::npos);
    EXPECT_NE(line.find("\"seq\":1,"), std::string::npos);
    EXPECT_NE(line.find("\"action\":\"ComputerSystem.Reset\""), std::string::npos);
    EXPECT_EQ(sink.dropped(), 0U);
}

TEST(Sink, ReadRecordsUseInfoSeverity)
{
    std::string path = socketPath("severity");
    Listener listener(path);
    Sink sink(path);
    Record r;
    r.ev = "read";
    r.severity = 6;
    EXPECT_TRUE(sink.write(r));
    EXPECT_EQ(listener.receive().rfind("<86>", 0), 0U);
}

TEST(Auditable, ReadMethods)
{
    EXPECT_TRUE(isRead("GET"));
    EXPECT_TRUE(isRead("HEAD"));
    EXPECT_FALSE(isRead("POST"));
    EXPECT_FALSE(isAuditable("GET", 200));
}

TEST(RateLimiter, LimitsPerSecondAndCountsTheRest)
{
    RateLimiter limiter(2);
    std::chrono::steady_clock::time_point t0{std::chrono::seconds(100)};
    EXPECT_TRUE(limiter.allow(t0));
    EXPECT_TRUE(limiter.allow(t0 + std::chrono::milliseconds(10)));
    EXPECT_FALSE(limiter.allow(t0 + std::chrono::milliseconds(20)));
    EXPECT_FALSE(limiter.allow(t0 + std::chrono::milliseconds(999)));
    // A new second starts a new window, and the count is handed over once.
    EXPECT_TRUE(limiter.allow(t0 + std::chrono::seconds(1)));
    EXPECT_EQ(limiter.takeSuppressed(), 2U);
    EXPECT_EQ(limiter.takeSuppressed(), 0U);
}

TEST(Sink, CountsDropsAndReportsGapOnRecovery)
{
    std::string path = socketPath("gap");
    Sink sink(path);
    Record r;
    // No listener yet: both records are lost, and the caller is not blocked.
    EXPECT_FALSE(sink.write(r));
    EXPECT_FALSE(sink.write(r));
    EXPECT_EQ(sink.dropped(), 2U);

    Listener listener(path);
    EXPECT_TRUE(sink.write(r));
    std::string gap = listener.receive();
    EXPECT_NE(gap.find("\"ev\":\"gap\""), std::string::npos);
    EXPECT_NE(gap.find("\"detail\":{\"lost\":2}"), std::string::npos);
    EXPECT_NE(gap.find("\"seq\":3,"), std::string::npos);
    std::string rec = listener.receive();
    EXPECT_NE(rec.find("\"ev\":\"request\""), std::string::npos);
    EXPECT_NE(rec.find("\"seq\":4,"), std::string::npos);
}

TEST(Sink, FullSocketBufferDropsInsteadOfBlocking)
{
    std::string path = socketPath("full");
    Listener listener(path); // never reads
    Sink sink(path);
    Record r;
    r.detail = "{\"pad\":\"" + std::string(2000, 'x') + "\"}";
    bool sawDrop = false;
    for (int i = 0; i < 5000 && !sawDrop; ++i)
    {
        sawDrop = !sink.write(r);
    }
    EXPECT_TRUE(sawDrop);
    EXPECT_GT(sink.dropped(), 0U);
}

TEST(EventRecord, CarriesMessageResourceSeverityAndSubscribers)
{
    Record r = buildEventRecord("Environmental.1.1.0.LeakDetectedCritical",
                                "leak_sensor_1", "Critical", 2);
    std::string line = buildRecord(r, 1, std::chrono::system_clock::now());
    EXPECT_NE(line.find("\"ev\":\"event\""), std::string::npos);
    EXPECT_NE(line.find("\"msg\":\"Environmental.1.1.0.LeakDetectedCritical\""),
              std::string::npos);
    EXPECT_NE(line.find("\"detail\":{\"resource\":\"leak_sensor_1\","
                        "\"Severity\":\"Critical\",\"subscribers\":2}"),
              std::string::npos);
}

TEST(EventRecord, SyslogSeverityFollowsEventSeverity)
{
    EXPECT_EQ(buildEventRecord("m", "r", "Critical", 0).severity, 2);
    EXPECT_EQ(buildEventRecord("m", "r", "Warning", 0).severity, 4);
    EXPECT_EQ(buildEventRecord("m", "r", "OK", 0).severity, 5);
}

TEST(EventRecord, UnsafeNamesAreNotCopied)
{
    Record r = buildEventRecord("m", "bad\"name\n", "Critical", 0);
    EXPECT_NE(r.detail.find("\"resource\":\"<invalid>\""), std::string::npos);
    EXPECT_EQ(r.detail.find('\n'), std::string::npos);
}

TEST(Sink, CriticalEventIsSentAtCritSeverity)
{
    std::string path = socketPath("event");
    Listener listener(path);
    Sink sink(path);
    EXPECT_TRUE(sink.write(buildEventRecord("m", "r", "Critical", 0)));
    // authpriv (10 << 3) | crit (2)
    EXPECT_EQ(listener.receive().rfind("<82>", 0), 0U);
}
