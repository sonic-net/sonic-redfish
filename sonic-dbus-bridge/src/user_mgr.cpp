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

#include <pwd.h>
#include <shadow.h>

#include <array>
#include <chrono>
#include <limits>
#include <string>

namespace sonic
{
namespace user
{

namespace
{
// Only one user is exposed via D-Bus for Redfish authentication/authorization.
// TODO: If needed, replace this hardcoded name with a dynamic scan of /etc/passwd +
// /etc/group, picking up every local user that belongs to one of the
// privilege groups in privMgr (priv-admin / priv-operator / priv-user).
constexpr const char* adminUserName = "bmcweb";

long currentDate()
{
    const auto date = std::chrono::duration_cast<std::chrono::days>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

    if (date > std::numeric_limits<long>::max())
    {
        return std::numeric_limits<long>::max();
    }

    if (date < std::numeric_limits<long>::min())
    {
        return std::numeric_limits<long>::min();
    }

    return date;
}

}



bool UserMgr::isUserExist(const std::string& userName)
{
    if (userName.empty())
    {
        LOG_ERROR("User name is empty");
        throw std::invalid_argument("User name is empty");
    }
    if (usersList.find(userName) == usersList.end())
    {
        return false;
    }
    return true;
}



UserMgr::UserMgr(sdbusplus::asio::object_server& server, const char* path,
                 sonic::dbus_bridge::ObjectMapperService* objectMapper) :
    server(server),
    path(path),
    objectMapper_(objectMapper)
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
}



bool UserMgr::isUserEnabled(const std::string& userName)
{
    std::array<char, 4096> buffer{};
    struct spwd spwd;
    struct spwd* resultPtr = nullptr;
    int status = getspnam_r(userName.c_str(), &spwd, buffer.data(),
                            buffer.max_size(), &resultPtr);
    if (!status && (&spwd == resultPtr))
    {
        // according to chage/usermod code -1 means that account does not expire
        // https://github.com/shadow-maint/shadow/blob/7a796897e52293efe9e210ab8da32b7aefe65591/src/chage.c
        if (resultPtr->sp_expire < 0)
        {
            return true;
        }

        // check account expiration date against current date
        if (resultPtr->sp_expire > currentDate())
        {
            return true;
        }

        return false;
    }
    return false; // assume user is disabled for any error.
}



void UserMgr::initUserObjects(void)
{
    // Only create D-Bus object for the admin user
    // Authentication is done via PAM for all users, but only admin
    // is exposed for Redfish AccountService and authorization

    std::string userName = adminUserName;

    // Check if admin user exists in the system
    std::array<char, 4096> buffer{};
    struct passwd pwd;
    struct passwd* resultPtr = nullptr;

    int status = getpwnam_r(userName.c_str(), &pwd, buffer.data(),
                            buffer.max_size(), &resultPtr);

    if (status != 0 || resultPtr == nullptr)
    {
        LOG_ERROR("Redfish user '%s' not found in system", userName.c_str());
        // Don't throw - service can still start, but GetUserInfo will fail
        return;
    }

    // Admin user always has priv-admin privilege
    std::string userPriv = "priv-admin";

    // Admin user is in redfish group (for additional permissions if needed)
    std::vector<std::string> userGroups = {"redfish"};

    // Create D-Bus object path for admin user
    sdbusplus::message::object_path tempObjPath(usersObjPath);
    tempObjPath /= userName;
    std::string objPath(tempObjPath);

    // Create the Users object for admin
    usersList.emplace(userName, std::make_unique<Users>(
                                    server, objPath, userGroups,
                                    userPriv, isUserEnabled(userName), *this));

    LOG_INFO("Created D-Bus object for Redfish user '%s' at %s",
             userName.c_str(), objPath.c_str());
}

UserInfoMap UserMgr::getUserInfo(const std::string& userName)
{
    UserInfoMap userInfo;

    // Check if user exists in our list (local user)
    if (!isUserExist(userName))
    {
        LOG_ERROR("GetUserInfo: User %s not found", userName.c_str());
        throw std::runtime_error("User not found");
    }

    const auto& user = usersList[userName];

    userInfo.emplace("UserPrivilege", user->getUserPrivilege());
    userInfo.emplace("UserGroups", user->getUserGroups());
    userInfo.emplace("UserEnabled", user->getUserEnabled());
    userInfo.emplace("UserLockedForFailedAttempt", user->getUserLockedForFailedAttempt());
    userInfo.emplace("UserPasswordExpired", user->getUserPasswordExpired());
    userInfo.emplace("RemoteUser", false);  // Always false for local users

    return userInfo;
}

} // namespace user
} // namespace sonic
