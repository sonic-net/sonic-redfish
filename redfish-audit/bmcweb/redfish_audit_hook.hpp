///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

// Audit hook for bmcweb: one record per state-changing or refused request.
//
// The router calls attach() for every request. attach() wraps the completion
// handler, so the record is written when the response is final, whichever
// route handled it.
//
// Read requests (GET, HEAD) are recorded only while the bridge's LogLevel
// D-Bus property is DEBUG; otherwise they cost one flag test. The bridge takes
// that level from CONFIG_DB (LOGGER|sonic-dbus-bridge). bmcweb's own log
// level follows the same property.
//
// Events the BMC raises itself (leak, host power) are recorded by
// recordEvent(), at every log level.
//
// Only values named here are ever logged. Request bodies, credentials,
// tokens, certificates and network addresses are not; a route that is not
// listed below gets the standard fields and nothing from its body.

#pragma once

#include "async_resp.hpp"
#include "dbus_singleton.hpp"
#include "http_request.hpp"
#include "http_response.hpp"
#include "logging.hpp"
#include "redfish_audit.hpp"
#include "sessions.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace redfish_audit
{

inline Sink& sink()
{
    static Sink instance;
    return instance;
}

inline const Pseudonymizer& pseudonymizer()
{
    static const Pseudonymizer instance = Pseudonymizer::random();
    return instance;
}

struct RequestContext
{
    // Route pattern that matched, e.g. /redfish/v1/Systems/<str>/. Empty
    // until the router finds one.
    std::string rule;
    std::chrono::steady_clock::time_point start;
};

namespace detail
{

// Where sonic-dbus-bridge serves the log level
inline constexpr const char* logService = "com.sonic.RackManager";
inline constexpr const char* logPath = "/com/sonic/Redfish/Logging";
inline constexpr const char* logInterface = "com.sonic.Redfish.Logging";

inline bool& readsRecorded()
{
    static bool enabled = false;
    return enabled;
}

// At most this many read records per second. Reads share the syslog socket
// with the records of changes and refusals, and must not crowd those out.
inline constexpr unsigned maxReadsPerSecond = 50;

inline RateLimiter& readLimiter()
{
    static RateLimiter instance(maxReadsPerSecond);
    return instance;
}

// bmcweb's own log level for a bridge level name. bmcweb logs every request
// at its info level, so only DEBUG raises it above the built-in level; ERROR
// and CRIT lower it. Any other name, or none, means the built-in level.
inline crow::LogLevel bmcwebLogLevel(std::string_view level,
                                     crow::LogLevel builtIn)
{
    if (level == "DEBUG")
    {
        return crow::LogLevel::Debug;
    }
    if (level == "ERROR")
    {
        return crow::LogLevel::Error;
    }
    if (level == "CRIT")
    {
        return crow::LogLevel::Critical;
    }
    return builtIn;
}

inline void applyLogLevel(std::string_view level)
{
    // Read before the first change, so it is the level bmcweb was built with.
    static const crow::LogLevel builtIn = crow::getBmcwebCurrentLoggingLevel();
    readsRecorded() = (level == "DEBUG");
    crow::getBmcwebCurrentLoggingLevel() = bmcwebLogLevel(level, builtIn);
}

// Back to the defaults until the bridge answers, so a missing bridge means
// the default.
inline void fetchLogLevel()
{
    applyLogLevel({});
    if (crow::connections::systemBus == nullptr)
    {
        return;
    }
    sdbusplus::asio::getProperty<std::string>(
        *crow::connections::systemBus, logService, logPath, logInterface,
        "LogLevel",
        [](const boost::system::error_code& ec, const std::string& level) {
            if (!ec)
            {
                applyLogLevel(level);
            }
        });
}

inline constexpr size_t maxBodyBytes = 64 * 1024;
inline constexpr size_t maxListItems = 16;

struct Field
{
    std::string_view name;
    // When not empty, the value must be one of these.
    std::span<const std::string_view> allowed;
};

struct Annotation
{
    std::string_view method;
    std::string_view ruleSuffix;
    std::span<const Field> fields;
    // Copy the fields only when the request succeeded. For a body that also
    // holds a password: a refused request may carry it in the wrong field.
    bool successOnly = false;
};

inline constexpr std::array<std::string_view, 13> resetTypes{
    "On",           "ForceOff",        "GracefulShutdown", "GracefulRestart",
    "ForceRestart", "Nmi",             "ForceOn",          "PushPowerButton",
    "PowerCycle",   "Suspend",         "Pause",            "Resume",
    "FullPowerCycle"};

inline constexpr std::array<Field, 1> resetFields{
    Field{"ResetType", resetTypes}};
inline constexpr std::array<Field, 2> resetToDefaultsFields{
    Field{"ResetType", {}}, Field{"ResetToDefaultsType", {}}};
inline constexpr std::array<Field, 2> accountFields{Field{"UserName", {}},
                                                    Field{"RoleId", {}}};
inline constexpr std::array<Field, 3> subscriptionFields{
    Field{"Protocol", {}}, Field{"RegistryPrefixes", {}},
    Field{"ResourceTypes", {}}};
inline constexpr std::array<Field, 1> testEventFields{
    Field{"MessageId", {}}};

inline constexpr std::array<Annotation, 7> annotations{
    Annotation{"POST", "/Actions/ComputerSystem.Reset", resetFields},
    Annotation{"POST", "/Actions/Manager.Reset", resetFields},
    Annotation{"POST", "/Actions/Chassis.Reset", resetFields},
    Annotation{"POST", "/Actions/Manager.ResetToDefaults",
               resetToDefaultsFields},
    Annotation{"POST", "/redfish/v1/AccountService/Accounts", accountFields,
               true},
    Annotation{"POST", "/redfish/v1/EventService/Subscriptions",
               subscriptionFields},
    Annotation{"POST", "/Actions/EventService.SubmitTestEvent",
               testEventFields},
};

inline std::string_view stripTrailingSlash(std::string_view s)
{
    while (s.size() > 1 && s.back() == '/')
    {
        s.remove_suffix(1);
    }
    return s;
}

inline std::string_view lastSegment(std::string_view path)
{
    path = stripTrailingSlash(path);
    size_t pos = path.rfind('/');
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

inline std::string checkedValue(const nlohmann::json& value,
                                std::span<const std::string_view> allowed)
{
    const std::string* str = value.get_ptr<const std::string*>();
    if (str == nullptr || !isSafeToken(*str))
    {
        return std::string(invalidValue);
    }
    if (!allowed.empty())
    {
        for (std::string_view candidate : allowed)
        {
            if (candidate == *str)
            {
                return *str;
            }
        }
        return std::string(invalidValue);
    }
    return *str;
}

inline nlohmann::json checkedField(const nlohmann::json& value,
                                   std::span<const std::string_view> allowed)
{
    if (!value.is_array())
    {
        return checkedValue(value, allowed);
    }
    nlohmann::json out = nlohmann::json::array();
    for (const nlohmann::json& item : value)
    {
        if (out.size() >= maxListItems)
        {
            break;
        }
        out.push_back(checkedValue(item, allowed));
    }
    return out;
}

// The request body as a JSON object, or a discarded value when it is
// absent, too large, or not an object.
inline nlohmann::json parseBody(const crow::Request& req)
{
    const std::string& body = req.body();
    if (body.empty() || body.size() > maxBodyBytes)
    {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_object())
    {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
    return parsed;
}

// Alert names and severities from a SubmitAlert payload:
//   {"redfish_alert_data": {"LeakDetected": {"Severity": "Critical", ...}}}
// Measured values and positions are left out.
inline void describeAlerts(const nlohmann::json& body, nlohmann::json& out)
{
    nlohmann::json alerts = nlohmann::json::array();
    for (const auto& [envelope, content] : body.items())
    {
        if (!envelope.starts_with("redfish") || !content.is_object())
        {
            continue;
        }
        for (const auto& [name, alert] : content.items())
        {
            if (alerts.size() >= maxListItems)
            {
                break;
            }
            nlohmann::json entry;
            entry["name"] = safeToken(name);
            if (alert.is_object())
            {
                auto severity = alert.find("Severity");
                if (severity != alert.end())
                {
                    entry["Severity"] = checkedValue(*severity, {});
                }
            }
            alerts.push_back(std::move(entry));
        }
    }
    out["alerts"] = std::move(alerts);
}

inline std::string buildDetail(const crow::Request& req,
                               const crow::Response& res,
                               std::string_view method, std::string_view rule)
{
    nlohmann::json out = nlohmann::json::object();
    std::string_view path = req.url().path();

    if (rule.ends_with("/AccountService/Accounts/<str>"))
    {
        out["account"] = safeToken(lastSegment(path));
    }
    else if (rule.ends_with("/EventService/Subscriptions/<str>"))
    {
        out["id"] = safeToken(lastSegment(path));
    }
    else if (rule.ends_with("/EventService/Subscriptions") &&
             method == "POST" && res.resultInt() == 201)
    {
        out["id"] = safeToken(lastSegment(res.getHeaderValue("Location")));
    }

    if (method == "PATCH")
    {
        nlohmann::json body = parseBody(req);
        if (!body.is_discarded())
        {
            nlohmann::json changed = nlohmann::json::array();
            for (const auto& [key, value] : body.items())
            {
                if (changed.size() >= maxListItems)
                {
                    break;
                }
                changed.push_back(safeToken(key));
            }
            out["changed"] = std::move(changed);
        }
    }
    else if (rule.ends_with("/Actions/SONiC.SubmitAlert"))
    {
        nlohmann::json body = parseBody(req);
        if (!body.is_discarded())
        {
            describeAlerts(body, out);
        }
        out["bytes"] = req.body().size();
    }
    else
    {
        for (const Annotation& annotation : annotations)
        {
            if (annotation.method != method ||
                !rule.ends_with(annotation.ruleSuffix))
            {
                continue;
            }
            if (annotation.successOnly &&
                (res.resultInt() < 200 || res.resultInt() >= 300))
            {
                break;
            }
            nlohmann::json body = parseBody(req);
            if (body.is_discarded())
            {
                break;
            }
            for (const Field& field : annotation.fields)
            {
                auto it = body.find(field.name);
                if (it != body.end())
                {
                    out[field.name] = checkedField(*it, field.allowed);
                }
            }
            break;
        }
    }

    if (out.empty())
    {
        return {};
    }
    return out.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
}

// MessageId of the first error in a Redfish error response, if any.
inline std::string firstMessageId(const crow::Response& res)
{
    const nlohmann::json& body = res.jsonValue;
    if (!body.is_object())
    {
        return {};
    }
    auto error = body.find("error");
    if (error == body.end() || !error->is_object())
    {
        return {};
    }
    auto info = error->find("@Message.ExtendedInfo");
    if (info == error->end() || !info->is_array() || info->empty())
    {
        return {};
    }
    const nlohmann::json& first = info->front();
    if (!first.is_object())
    {
        return {};
    }
    auto id = first.find("MessageId");
    if (id == first.end())
    {
        return {};
    }
    const std::string* str = id->get_ptr<const std::string*>();
    return (str != nullptr && isSafeToken(*str)) ? *str : std::string();
}

inline std::string_view authName(persistent_data::SessionType type)
{
    switch (type)
    {
        case persistent_data::SessionType::Basic:
            return "basic";
        case persistent_data::SessionType::Session:
            return "session";
        case persistent_data::SessionType::Cookie:
            return "cookie";
        case persistent_data::SessionType::MutualTLS:
            return "mtls";
        case persistent_data::SessionType::None:
        default:
            return "";
    }
}

// bmcweb keeps the privilege name; records carry the Redfish role name.
inline std::string roleName(std::string_view privilege)
{
    if (privilege == "priv-admin")
    {
        return "Administrator";
    }
    if (privilege == "priv-operator")
    {
        return "Operator";
    }
    if (privilege == "priv-user")
    {
        return "ReadOnly";
    }
    return std::string(privilege);
}

inline void fillClient(const boost::asio::ip::address& address, Record& record)
{
    if (!address.is_unspecified())
    {
        record.client = pseudonymizer().make('c', address.to_string());
    }
}

inline void fillCaller(const crow::Request& req, Record& record)
{
    fillClient(req.ipAddress, record);
    if (req.session == nullptr)
    {
        return;
    }
    const persistent_data::UserSession& session = *req.session;
    record.user = session.username;
    record.role = roleName(session.userRole);
    record.auth = authName(session.sessionType);
    if (session.sessionType == persistent_data::SessionType::Session ||
        session.sessionType == persistent_data::SessionType::Cookie)
    {
        record.sess = pseudonymizer().make('s', session.uniqueId);
    }
}

// SubmitTelemetry arrives continuously, so accepted calls are counted and
// reported once per interval instead of one record each.
struct TelemetryCounters
{
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    std::chrono::steady_clock::time_point since =
        std::chrono::steady_clock::now();
};

inline TelemetryCounters& telemetryCounters()
{
    static TelemetryCounters instance;
    return instance;
}

inline constexpr std::chrono::seconds telemetryInterval{60};

inline void flushTelemetrySummary(std::chrono::steady_clock::time_point now)
{
    TelemetryCounters& counters = telemetryCounters();
    if (now - counters.since < telemetryInterval)
    {
        return;
    }
    if (counters.accepted != 0 || counters.rejected != 0)
    {
        nlohmann::json summary;
        summary["accepted"] = counters.accepted;
        summary["rejected"] = counters.rejected;
        summary["seconds"] = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - counters.since)
                                 .count();
        Record record;
        record.ev = "telemetry_summary";
        record.action = "SONiC.SubmitTelemetry";
        record.detail = summary.dump();
        sink().write(record);
    }
    counters = TelemetryCounters{};
    counters.since = now;
}

inline void recordCompleted(const crow::Request& req,
                            const crow::Response& res,
                            const RequestContext& context)
{
    auto now = std::chrono::steady_clock::now();
    std::string_view method = req.methodString();
    unsigned status = res.resultInt();
    std::string_view rule = stripTrailingSlash(context.rule);

    if (rule.ends_with("/Actions/SONiC.SubmitTelemetry") && method == "POST")
    {
        TelemetryCounters& counters = telemetryCounters();
        if (status < 300)
        {
            ++counters.accepted;
            flushTelemetrySummary(now);
            return;
        }
        ++counters.rejected;
    }
    flushTelemetrySummary(now);

    bool auditable = isAuditable(method, status);
    if (!auditable && !(isRead(method) && readsRecorded()))
    {
        return;
    }
    if (!auditable && !readLimiter().allow(now))
    {
        return;
    }

    Record record;
    // Reads left out by the limiter since the last read record. Added to
    // detail at the end, after the route's own detail is built.
    uint64_t suppressed = 0;
    if (!auditable)
    {
        record.ev = "read";
        record.severity = 6;
        suppressed = readLimiter().takeSuppressed();
    }
    record.method = std::string(method);
    record.uri = rule.empty() ? std::string("<unmatched>")
                              : std::string(rule);
    record.status = status;
    record.msg = firstMessageId(res);
    record.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - context.start)
                    .count();
    fillCaller(req, record);

    size_t actionPos = rule.rfind("/Actions/");
    if (actionPos != std::string_view::npos)
    {
        record.action = std::string(rule.substr(actionPos + 9));
    }

    bool sessionCreate =
        method == "POST" &&
        (rule == "/redfish/v1/SessionService/Sessions" ||
         rule == "/redfish/v1/SessionService/Sessions/Members");
    if (sessionCreate)
    {
        // The body holds a password: take the account name only, and only
        // once the login has succeeded. A failed attempt may carry a
        // password typed into the user field.
        if (status == 201)
        {
            record.ev = "login";
            nlohmann::json body = parseBody(req);
            if (!body.is_discarded())
            {
                auto user = body.find("UserName");
                if (user != body.end())
                {
                    record.user = checkedValue(*user, {});
                }
            }
            record.auth = "session";
        }
        else
        {
            record.ev = "login_failed";
        }
    }
    else if (status == 401 || status == 403)
    {
        record.ev = "denied";
    }
    else
    {
        if (method == "DELETE" && status < 300 &&
            rule == "/redfish/v1/SessionService/Sessions/<str>")
        {
            record.ev = "logout";
        }
        else
        {
            record.detail = buildDetail(req, res, method, rule);
        }
    }
    record.detail = withSuppressed(std::move(record.detail), suppressed);

    sink().write(record);
}

// At most this many records per second for requests refused before routing,
// so a flood of unauthenticated requests cannot flood the log.
inline constexpr unsigned maxUnauthenticatedPerSecond = 20;

inline RateLimiter& unauthenticatedLimiter()
{
    static RateLimiter instance(maxUnauthenticatedPerSecond);
    return instance;
}

} // namespace detail

// Called by the router for every request, before route lookup.
inline std::shared_ptr<RequestContext> attach(
    const std::shared_ptr<crow::Request>& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    auto context = std::make_shared<RequestContext>();
    context->start = std::chrono::steady_clock::now();

    std::function<void(crow::Response&)> next =
        asyncResp->res.releaseCompleteRequestHandler();
    asyncResp->res.setCompleteRequestHandler(
        [req, context, next{std::move(next)}](crow::Response& res) mutable {
            detail::recordCompleted(*req, res, *context);
            // The connection replaces this handler while it runs; keep the
            // next one alive on the stack and touch no captures afterwards.
            std::function<void(crow::Response&)> local = std::move(next);
            if (local)
            {
                local(res);
            }
        });
    return context;
}

// Called when a request is refused for lack of valid credentials, which
// happens before routing. The path is not logged: nothing has validated it.
// This form needs no Request: HTTP/1 can refuse a request on its headers
// alone, before one is built.
inline void recordUnauthenticated(std::string_view method, unsigned status,
                                  const boost::asio::ip::address& address)
{
    RateLimiter& limiter = detail::unauthenticatedLimiter();
    if (!limiter.allow(std::chrono::steady_clock::now()))
    {
        return;
    }

    Record record;
    record.ev = "denied";
    record.method = std::string(method);
    record.uri = "<unauthenticated>";
    record.status = status;
    detail::fillClient(address, record);
    record.detail = withSuppressed({}, limiter.takeSuppressed());
    sink().write(record);
}

inline void recordUnauthenticated(const crow::Request& req,
                                  crow::Response& res)
{
    recordUnauthenticated(req.methodString(), res.resultInt(), req.ipAddress);
}

// Called when the BMC raises a Redfish event of its own. subscribers is the
// number of event subscriptions at that moment.
inline void recordEvent(std::string_view messageId, std::string_view resource,
                        std::string_view severity, size_t subscribers)
{
    sink().write(buildEventRecord(messageId, resource, severity, subscribers));
}

inline void watchLogLevel();

// Called once at service start, after the system bus is connected.
inline void recordStart()
{
    Record record;
    record.ev = "start";
    sink().write(record);
    watchLogLevel();
}

// Follow the bridge's LogLevel property: its current value, later changes,
// and bridge restarts (which put the level back to its default).
inline void watchLogLevel()
{
    if (crow::connections::systemBus == nullptr)
    {
        return;
    }
    static sdbusplus::bus::match_t changed(
        *crow::connections::systemBus,
        // The sender is part of the match: only the owner of the bridge's bus
        // name is believed.
        "type='signal',sender='com.sonic.RackManager',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',path='/com/sonic/Redfish/Logging',"
        "arg0='com.sonic.Redfish.Logging'",
        [](sdbusplus::message_t& msg) {
            std::string interface;
            std::map<std::string, std::variant<std::string>> values;
            try
            {
                msg.read(interface, values);
            }
            catch (const sdbusplus::exception_t&)
            {
                return;
            }
            auto level = values.find("LogLevel");
            if (level != values.end())
            {
                detail::applyLogLevel(std::get<std::string>(level->second));
            }
        });
    static sdbusplus::bus::match_t ownerChanged(
        *crow::connections::systemBus,
        "type='signal',sender='org.freedesktop.DBus',"
        "member='NameOwnerChanged',arg0='com.sonic.RackManager'",
        [](sdbusplus::message_t&) { detail::fetchLogLevel(); });
    detail::fetchLogLevel();
}

} // namespace redfish_audit
