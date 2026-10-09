///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "user_mgr.hpp"
#include "users.hpp"
#include "logger.hpp"

#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace sonic
{
namespace user
{

namespace
{
// Virtual Redfish identity exposed via D-Bus. It is NOT backed by a Unix
// account: the redfish container deliberately has no local users and no
// passwords. The object exists so the Redfish AccountService has an account
// to list; it plays no role in the access decision (see getUserInfo).
constexpr const char* redfishIdentityName = "bmcweb";

// Trim surrounding whitespace, so a configured list may be written with
// spaces after the separators, or set by a script that leaves a newline.
std::string trim(const std::string& value)
{
    constexpr const char* space = " \t\r\n";
    const auto first = value.find_first_not_of(space);
    if (first == std::string::npos)
    {
        return "";
    }
    const auto last = value.find_last_not_of(space);
    return value.substr(first, last - first + 1);
}

// Match a common name against one configured entry. An entry starting with
// "*." is a wildcard that matches any name ending in the remaining suffix:
// *.example.com matches one.example.com and two.one.example.com, but not
// example.com itself and not example.com.edu. Every other entry matches
// exactly, and the comparison is case sensitive. An entry that starts with
// "*" but is not a usable wildcard is skipped with a warning. This mirrors
// the handling of the SONiC REST API server.
bool commonNameMatches(const std::string& commonName, const std::string& entry)
{
    if (entry.rfind("*.", 0) == 0)
    {
        if (entry.size() < 3)
        {
            LOG_WARNING("Skipping invalid trusted common name '%s'",
                        entry.c_str());
            return false;
        }
        const std::string suffix = entry.substr(1); // keeps the leading dot
        return commonName.size() > suffix.size() &&
               commonName.compare(commonName.size() - suffix.size(),
                                  suffix.size(), suffix) == 0;
    }
    if (entry.rfind("*", 0) == 0)
    {
        LOG_WARNING("Skipping invalid trusted common name '%s'", entry.c_str());
        return false;
    }
    return commonName == entry;
}
}



UserMgr::UserMgr(sdbusplus::asio::object_server& server, const char* path,
                 sonic::dbus_bridge::ObjectMapperService* objectMapper,
                 sonic::dbus_bridge::RedisAdapter* redisAdapter) :
    server(server),
    path(path),
    objectMapper_(objectMapper),
    redisAdapter_(redisAdapter)
{
    // Register ObjectManager interface at /xyz/openbmc_project/user
    // This is required for BMCWeb's getManagedObjects() to work
    server.add_manager(path);

    // Register xyz.openbmc_project.User.Manager interface
    userMgrIface = server.add_interface(path, "xyz.openbmc_project.User.Manager");

    // Register AllPrivileges property (read-only)
    userMgrIface->register_property("AllPrivileges", privMgr);

    // Register AllGroups property (read-only)
    userMgrIface->register_property("AllGroups", allGroups);

    // Register GetUserInfo method
    userMgrIface->register_method(
        "GetUserInfo",
        [this](const std::string& userName) {
            return getUserInfo(userName);
        });

    // Register DeleteUser method (delete via object path, not this interface)
    // BMCWeb uses the Delete method on individual user objects

    userMgrIface->initialize();

    initUserObjects();

    // Read the trusted common names up front. A failure here is not fatal:
    // the read is retried on the first request that needs it, and requests
    // are refused until it succeeds.
    loadTrustedCnames();
}



void UserMgr::initUserObjects(void)
{
    // Create the D-Bus object for the virtual Redfish identity
    // unconditionally. There is no Unix account behind it (nothing in
    // /etc/passwd is consulted), so this cannot fail: authentication is mTLS
    // (the client certificate is the credential) and enabled is always true.

    std::string userName = redfishIdentityName;
    std::string userPriv = "priv-admin";
    std::vector<std::string> userGroups = {"redfish"};

    // Create D-Bus object path for the identity
    sdbusplus::message::object_path tempObjPath(usersObjPath);
    tempObjPath /= userName;
    std::string objPath(tempObjPath);

    usersList.emplace(userName, std::make_unique<Users>(
                                    server, objPath, userGroups,
                                    userPriv, true /* enabled */, *this));

    LOG_INFO("Created D-Bus object for Redfish identity '%s' at %s",
             userName.c_str(), objPath.c_str());
}

bool UserMgr::loadTrustedCnames()
{
    if (!redisAdapter_)
    {
        return false;
    }

    const std::optional<std::string> read = redisAdapter_->getRedfishClientCnames();
    if (!read)
    {
        return false; // CONFIG_DB unreadable; retried on the next request
    }

    cachedCnames_ = *read;
    haveCachedCnames_ = true;
    if (trim(cachedCnames_).empty())
    {
        LOG_WARNING("No trusted client common names configured; every client "
                    "certificate is refused until client_crt_cname is set");
    }
    else
    {
        LOG_INFO("Trusted client common names: %s", cachedCnames_.c_str());
    }
    return true;
}

bool UserMgr::isCommonNameTrusted(const std::string& commonName)
{
    if (!redisAdapter_)
    {
        // Refuse rather than accept: without CONFIG_DB the trusted list cannot
        // be known, and that is the same position as a failed read below.
        LOG_ERROR("GetUserInfo: no CONFIG_DB access; refusing '%s'",
                  commonName.c_str());
        return false;
    }

    // The list is read once and then held for the lifetime of the process,
    // as the SONiC REST API server does with its own trusted common names.
    // Changing it therefore takes effect when this service restarts.
    if (!haveCachedCnames_ && !loadTrustedCnames())
    {
        // The policy is not known and cannot be read, so refuse rather than
        // drop the check. A client certificate issued by the staged CA is
        // still required to get this far.
        LOG_ERROR("GetUserInfo: trusted common names are unknown and CONFIG_DB "
                  "cannot be read; refusing '%s'", commonName.c_str());
        return false;
    }

    const std::string& configured = cachedCnames_;
    if (trim(configured).empty())
    {
        // No trusted list configured: refuse, as the SONiC REST API server
        // does. Being issued by the staged CA is not enough on its own.
        LOG_ERROR("GetUserInfo: no trusted common names configured; refusing "
                  "'%s'", commonName.c_str());
        return false;
    }

    std::istringstream stream(configured);
    std::string entry;
    while (std::getline(stream, entry, ','))
    {
        const std::string trimmed = trim(entry);
        if (!trimmed.empty() && commonNameMatches(commonName, trimmed))
        {
            LOG_DEBUG("GetUserInfo: common name '%s' matches trusted entry "
                      "'%s'",
                      commonName.c_str(), trimmed.c_str());
            return true;
        }
    }

    LOG_ERROR("GetUserInfo: common name '%s' matches none of the trusted "
              "common names",
              commonName.c_str());
    return false;
}

UserInfoMap UserMgr::getUserInfo(const std::string& userName)
{
    // Authorization is not role-based on the SONiC BMC: there is no local user
    // database and no Unix accounts in this container. Access is decided by
    // authentication, which is mTLS: bmcweb creates a session, and so asks
    // here, only for a client certificate issued by the staged CA. A client
    // without one still completes the TLS handshake, but has no session and is
    // answered 401 before this is reached. On top of that, the common name
    // carried by the certificate (which bmcweb passes here as the identity)
    // must match the trusted list in CONFIG_DB; an empty list matches nothing.
    // Anything accepted is answered with priv-admin.
    if (userName.empty())
    {
        LOG_ERROR("GetUserInfo: user name is empty");
        throw std::invalid_argument("User name is empty");
    }

    // An untrusted common name is answered with a role that carries no
    // privileges, rather than an error. bmcweb maps an unexpected D-Bus error
    // to 500, which would report a client whose certificate is not trusted as
    // a server fault; an empty role fails its privilege check and is answered
    // with 403 instead.
    const bool trusted = isCommonNameTrusted(userName);

    UserInfoMap userInfo;

    userInfo.emplace("UserPrivilege",
                     trusted ? std::string("priv-admin") : std::string(""));
    userInfo.emplace("UserGroups", std::vector<std::string>{"redfish"});
    userInfo.emplace("UserEnabled", true);
    userInfo.emplace("UserLockedForFailedAttempt", false);
    userInfo.emplace("UserPasswordExpired", false);
    userInfo.emplace("RemoteUser", false);

    return userInfo;
}

} // namespace user
} // namespace sonic
