/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Config.h"
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

namespace
{
std::string JoinPath(std::string const& prefix, std::string const& key)
{
    return prefix.empty() ? key : prefix + '.' + key;
}

// ROOFTREE_ + path in upper case, dots become underscores: "Database.Login.Host" -> ROOFTREE_DATABASE_LOGIN_HOST
std::optional<std::string> GetEnvOverride(std::string_view path)
{
    std::string name;
    for (char c : path)
        name += (c == '.') ? '_' : char(std::toupper(static_cast<unsigned char>(c)));

    if (char const* value = std::getenv(("ROOFTREE_" + name).c_str()))
        return value;
    return std::nullopt;
}

std::string ErrorMessage(YAML::Exception const& e)
{
    if (e.mark.is_null())
        return e.msg;
    return e.msg + " at line " + std::to_string(e.mark.line + 1) + ", column " + std::to_string(e.mark.column + 1);
}
}

struct Config::Impl
{
    std::string Filename;

    // The whole file. Only read while holding TreeLock, see GetNode()
    YAML::Node Root;
    mutable std::mutex TreeLock;

    // Every mapping and value by its path ("" = the root), pointing into Root
    std::map<std::string, YAML::Node, std::less<>> Nodes;

    // The keys of every mapping by its path, in file order
    std::map<std::string, std::vector<std::string>, std::less<>> Keys;

    void Index(YAML::Node const& map, std::string const& prefix)
    {
        std::vector<std::string>& keys = Keys[prefix];
        for (auto const& entry : map)
        {
            if (!entry.first.IsScalar())
                throw YAML::Exception(entry.first.Mark(), "keys must be plain values");

            std::string path = JoinPath(prefix, entry.first.Scalar());
            // YAML forbids duplicate keys, this also catches two spellings of one path: server.http: and server: { http: }
            if (Nodes.contains(path))
                throw YAML::Exception(entry.first.Mark(), "duplicate key '" + path + "'");

            Nodes.emplace(path, entry.second);
            keys.push_back(entry.first.Scalar());
            if (entry.second.IsMap())
                Index(entry.second, path);
        }
    }
};

Config::Config() : _impl(std::make_unique<Impl>())
{
    _impl->Root = YAML::Node(YAML::NodeType::Map);
    _impl->Nodes.emplace("", _impl->Root);
    _impl->Keys[""];
}

Config::~Config() = default;

std::shared_ptr<Config const> Config::Parse(YAML::Node root, std::string filename)
{
    // An empty document is an empty config, anything else must be a mapping ("key = value" parses as a plain scalar)
    if (root.IsNull())
        root = YAML::Node(YAML::NodeType::Map);
    else if (!root.IsMap())
        throw YAML::Exception(root.Mark(), "the root of the config must be a mapping");

    auto config = std::make_shared<Config>();
    Impl& impl = *config->_impl;
    impl.Filename = std::move(filename);
    impl.Root = root;
    impl.Nodes.clear();
    impl.Keys.clear();
    impl.Nodes.emplace("", root);
    impl.Index(root, "");

    // Not shared yet, so the tree can still be changed. Assigning to a handle changes the node it refers to
    for (auto& [path, node] : impl.Nodes)
        if (node.IsScalar() || node.IsNull())
            if (std::optional<std::string> env = GetEnvOverride(path))
                node = *env;

    return config;
}

std::string const& Config::GetFilename() const
{
    return _impl->Filename;
}

bool Config::Has(std::string_view path) const
{
    return _impl->Nodes.contains(path);
}

bool Config::IsMap(std::string_view path) const
{
    return _impl->Keys.contains(path);
}

std::vector<std::string> Config::GetKeys(std::string_view path) const
{
    auto itr = _impl->Keys.find(path);
    return itr != _impl->Keys.end() ? itr->second : std::vector<std::string>();
}

YAML::Node Config::GetNode(std::string_view path) const
{
    auto itr = _impl->Nodes.find(path);
    if (itr == _impl->Nodes.end())
        return YAML::Node(YAML::NodeType::Undefined);

    std::lock_guard lock(_impl->TreeLock);
    return YAML::Clone(itr->second);
}

struct ConfigMgr::Impl
{
    mutable std::mutex ConfigLock;
    std::shared_ptr<Config const> Current = std::make_shared<Config>();
    bool Loaded = false;

    // Serializes loads with their callbacks, and changes to the subscribers
    std::mutex LoadLock;
    std::map<uint32, LoadedCallback> Subscribers;
    uint32 NextSubscriberId = 1;
};

ConfigMgr::ConfigMgr() : _impl(std::make_unique<Impl>()) { }
ConfigMgr::~ConfigMgr() = default;

ConfigMgr* ConfigMgr::instance()
{
    static ConfigMgr instance;
    return &instance;
}

bool ConfigMgr::LoadFile(std::string const& file, std::string& error)
{
    try
    {
        Publish(Config::Parse(YAML::LoadFile(file), file));
        return true;
    }
    catch (YAML::Exception const& e)
    {
        error = file + ": " + ErrorMessage(e);
        return false;
    }
}

bool ConfigMgr::LoadString(std::string_view content, std::string& error)
{
    try
    {
        Publish(Config::Parse(YAML::Load(std::string(content)), ""));
        return true;
    }
    catch (YAML::Exception const& e)
    {
        error = ErrorMessage(e);
        return false;
    }
}

bool ConfigMgr::Reload(std::string& error)
{
    std::string file = GetConfig()->GetFilename();
    if (file.empty())
    {
        error = "no file loaded";
        return false;
    }
    return LoadFile(file, error);
}

std::shared_ptr<Config const> ConfigMgr::GetConfig() const
{
    std::lock_guard lock(_impl->ConfigLock);
    return _impl->Current;
}

void ConfigMgr::Publish(std::shared_ptr<Config const> config)
{
    std::lock_guard loadLock(_impl->LoadLock);
    {
        std::lock_guard lock(_impl->ConfigLock);
        _impl->Current = config;
        _impl->Loaded = true;
    }

    for (auto const& [id, callback] : _impl->Subscribers)
        callback(*config);
}

ConfigMgr::Subscription ConfigMgr::OnLoaded(LoadedCallback callback)
{
    std::lock_guard loadLock(_impl->LoadLock);
    uint32 id = _impl->NextSubscriberId++;
    LoadedCallback& stored = _impl->Subscribers[id] = std::move(callback);

    std::shared_ptr<Config const> current;
    {
        std::lock_guard lock(_impl->ConfigLock);
        if (_impl->Loaded)
            current = _impl->Current;
    }
    if (current)
        stored(*current);

    return Subscription(id);
}

void ConfigMgr::Unsubscribe(uint32 id)
{
    std::lock_guard loadLock(_impl->LoadLock);
    _impl->Subscribers.erase(id);
}

ConfigMgr::Subscription::Subscription(Subscription&& other) noexcept : _id(std::exchange(other._id, 0)) { }

ConfigMgr::Subscription& ConfigMgr::Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        _id = std::exchange(other._id, 0);
    }
    return *this;
}

ConfigMgr::Subscription::~Subscription()
{
    Reset();
}

void ConfigMgr::Subscription::Reset()
{
    if (_id)
        sConfigMgr->Unsubscribe(std::exchange(_id, 0));
}
