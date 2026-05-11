///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "object_mapper.hpp"
#include "config.h"
#include "logger.hpp"

#include <algorithm>

namespace sonic::dbus_bridge
{

namespace
{

constexpr const char* OBJMAPPER_PATH = "/xyz/openbmc_project/object_mapper";
constexpr const char* OBJMAPPER_IFACE = "xyz.openbmc_project.ObjectMapper";

} // namespace

ObjectMapperService::ObjectMapperService(sdbusplus::asio::object_server& server)
    : server_(server)
{
}

bool ObjectMapperService::initialize()
{
    mapperIface_ = server_.add_interface(OBJMAPPER_PATH, OBJMAPPER_IFACE);

    mapperIface_->register_method(
        "GetObject",
        [this](const std::string& path,
               const std::vector<std::string>& interfaces) {
            return getObject(path, interfaces);
        });

    mapperIface_->register_method(
        "GetSubTree",
        [this](const std::string& subtree, int32_t depth,
               const std::vector<std::string>& interfaces) {
            return getSubTree(subtree, depth, interfaces);
        });

    mapperIface_->register_method(
        "GetSubTreePaths",
        [this](const std::string& subtree, int32_t depth,
               const std::vector<std::string>& interfaces) {
            return getSubTreePaths(subtree, depth, interfaces);
        });

    mapperIface_->register_method(
        "GetAssociatedSubTreePaths",
        [this](const sdbusplus::message::object_path& associatedPath,
               const sdbusplus::message::object_path& subtree, int32_t depth,
               const std::vector<std::string>& interfaces) {
            return getAssociatedSubTreePaths(associatedPath.str, subtree.str,
                                             depth, interfaces);
        });

    mapperIface_->initialize();

    LOG_INFO("Registered minimal ObjectMapper at %s", OBJMAPPER_PATH);
    return true;
}

void ObjectMapperService::registerObject(
    const std::string& path, const std::vector<std::string>& interfaces,
    const std::string& serviceName)
{
    objects_[path] = {interfaces,
                      serviceName.empty() ? INVENTORY_MANAGER_BUSNAME
                                          : serviceName};
}

void ObjectMapperService::unregisterObject(const std::string& path)
{
    objects_.erase(path);
}

bool ObjectMapperService::pathIsUnder(const std::string& root,
                                      const std::string& path)
{
    if (root.empty() || root == "/")
    {
        return true;
    }

    // Normalize root by removing trailing slash
    std::string normalizedRoot = root;
    while (!normalizedRoot.empty() && normalizedRoot.back() == '/')
    {
        normalizedRoot.pop_back();
    }

    if (normalizedRoot.empty())
    {
        return true;
    }

    if (path.size() < normalizedRoot.size())
    {
        return false;
    }
    if (path.compare(0, normalizedRoot.size(), normalizedRoot) != 0)
    {
        return false;
    }
    if (path.size() == normalizedRoot.size())
    {
        return true;
    }
    return path[normalizedRoot.size()] == '/';
}

ObjectMapperService::GetObjectResult ObjectMapperService::getObject(
    const std::string& path, const std::vector<std::string>& interfaces)
{
    GetObjectResult result;

    auto it = objects_.find(path);
    if (it == objects_.end())
    {
        return result;
    }

    const auto& objInfo = it->second;

    // Filter interfaces if filter list is provided
    std::vector<std::string> matchedIfaces;
    if (interfaces.empty())
    {
        matchedIfaces = objInfo.interfaces;
    }
    else
    {
        for (const auto& iface : objInfo.interfaces)
        {
            if (std::find(interfaces.begin(), interfaces.end(), iface) !=
                interfaces.end())
            {
                matchedIfaces.push_back(iface);
            }
        }
    }

    if (!matchedIfaces.empty())
    {
        result[objInfo.serviceName] = matchedIfaces;
    }

    return result;
}

ObjectMapperService::GetSubTreeResult ObjectMapperService::getSubTree(
    const std::string& subtree, int32_t /*depth*/,
    const std::vector<std::string>& interfaces)
{
    GetSubTreeResult result;

    for (const auto& [path, objInfo] : objects_)
    {
        if (!pathIsUnder(subtree, path))
        {
            continue;
        }

        // Filter interfaces if filter list is provided
        std::vector<std::string> matchedIfaces;
        if (interfaces.empty())
        {
            matchedIfaces = objInfo.interfaces;
        }
        else
        {
            for (const auto& iface : objInfo.interfaces)
            {
                if (std::find(interfaces.begin(), interfaces.end(), iface) !=
                    interfaces.end())
                {
                    matchedIfaces.push_back(iface);
                }
            }
        }

        if (!matchedIfaces.empty())
        {
            result[path][objInfo.serviceName] = matchedIfaces;
        }
    }

    return result;
}

ObjectMapperService::GetSubTreePathsResult ObjectMapperService::getSubTreePaths(
    const std::string& subtree, int32_t /*depth*/,
    const std::vector<std::string>& interfaces)
{
    GetSubTreePathsResult result;

    for (const auto& [path, objInfo] : objects_)
    {
        if (!pathIsUnder(subtree, path))
        {
            continue;
        }

        // Check if interfaces match (if filter provided)
        bool matches = interfaces.empty();
        if (!matches)
        {
            for (const auto& iface : objInfo.interfaces)
            {
                if (std::find(interfaces.begin(), interfaces.end(), iface) !=
                    interfaces.end())
                {
                    matches = true;
                    break;
                }
            }
        }

        if (matches)
        {
            result.push_back(path);
        }
    }

    return result;
}

ObjectMapperService::GetSubTreePathsResult
    ObjectMapperService::getAssociatedSubTreePaths(
        const std::string& /*associatedPath*/, const std::string& /*subtree*/,
        int32_t /*depth*/, const std::vector<std::string>& /*interfaces*/)
{
    // We don't currently create any association objects, so simply
    // return an empty array of paths. This is sufficient for bmcweb's
    // chassis connectivity helpers, which treat empty results as
    // "no associations" without raising errors.
    return {};
}

} // namespace sonic::dbus_bridge

