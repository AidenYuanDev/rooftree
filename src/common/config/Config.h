/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Configuration backed by yaml-cpp.
 *
 * Every load produces an immutable Config snapshot. Readers take one with sConfigMgr->GetConfig() and
 * read as many values as they need from it: they all come from the same file, even if the config is
 * reloaded meanwhile. Modules that cache settings subscribe with OnLoaded() to read them again after
 * a reload.
 *
 * Paths are the nested mapping keys joined by dots: "Database.Login.Host" -> Database: { Login: { Host: ... } }
 * Keys may contain dots themselves: "Logger.server.http" -> Logger: { server.http: ... }
 *
 * Values and whole subtrees are decoded with YAML::convert<T>, so a struct only needs a specialization:
 *
 *   struct RealmInfo { std::string Name; uint16 Port; };
 *   template <> struct YAML::convert<RealmInfo> { static bool decode(Node const& node, RealmInfo& realm); };
 *
 *   // Realms: [ { Name: one, Port: 8085 }, { Name: two, Port: 8086 } ]
 *   auto realms = config.Get<std::vector<RealmInfo>>("Realms", {});
 *
 * Every value in the file can be overridden by an environment variable, read when the file is loaded:
 * "Database.Login.Host" -> ROOFTREE_DATABASE_LOGIN_HOST
 */

#ifndef ROOFTREE_CONFIG_H
#define ROOFTREE_CONFIG_H

#include "Define.h"
#include <yaml-cpp/yaml.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Config
{
public:
    // An empty config
    Config();
    ~Config();

    Config(Config const&) = delete;
    Config& operator=(Config const&) = delete;

    // Throws YAML::Exception if the content is not a valid config
    static std::shared_ptr<Config const> Parse(YAML::Node root, std::string filename);

    // The file this config was loaded from, empty if it was loaded from a string
    std::string const& GetFilename() const;

    bool Has(std::string_view path) const;
    bool IsMap(std::string_view path) const;

    // Keys of a mapping in file order, e.g. "Appender" -> "Console", "Server". "" is the root
    std::vector<std::string> GetKeys(std::string_view path) const;

    // A deep copy of a mapping or value, Undefined (IsDefined() is false) if missing.
    // yaml-cpp makes no promise that concurrent reads of one tree are safe (operator[] on a missing key
    // even inserts one), so the shared tree is never handed out, every reader gets its own copy
    YAML::Node GetNode(std::string_view path) const;

    // GetNode(path).as<T>(), nullopt if missing, empty or not convertible
    template <typename T>
    std::optional<T> Get(std::string_view path) const
    {
        YAML::Node node = GetNode(path);
        if (!node.IsDefined() || node.IsNull())
            return std::nullopt;

        try
        {
            return node.as<T>();
        }
        catch (YAML::Exception const&)
        {
            return std::nullopt;
        }
    }

    template <typename T>
    T Get(std::string_view path, T def) const
    {
        if (std::optional<T> value = Get<T>(path))
            return std::move(*value);
        return def;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

class ConfigMgr
{
public:
    using LoadedCallback = std::function<void(Config const&)>;

    // Unsubscribes when destroyed
    class Subscription
    {
    public:
        Subscription() = default;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        ~Subscription();

        void Reset();

    private:
        friend class ConfigMgr;
        explicit Subscription(uint32 id) : _id(id) { }

        uint32 _id = 0;
    };

    static ConfigMgr* instance();

    ConfigMgr(ConfigMgr const&) = delete;
    ConfigMgr& operator=(ConfigMgr const&) = delete;

    // On failure the current config stays and error says why
    bool LoadFile(std::string const& file, std::string& error);
    bool LoadString(std::string_view content, std::string& error);
    bool Reload(std::string& error);

    // The current snapshot, never null (an empty config before the first load). Safe from any thread
    std::shared_ptr<Config const> GetConfig() const;

    // Shortcut for a single value: GetConfig()->Get<T>(path, def)
    template <typename T>
    T Get(std::string_view path, T def) const { return GetConfig()->Get<T>(path, std::move(def)); }

    // Calls callback with the current config now (if one is loaded) and after every successful load,
    // on the thread that loads it. Callbacks run one at a time and must not load or subscribe themselves
    [[nodiscard]] Subscription OnLoaded(LoadedCallback callback);

private:
    ConfigMgr();
    ~ConfigMgr();

    void Publish(std::shared_ptr<Config const> config);
    void Unsubscribe(uint32 id);

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

#define sConfigMgr ConfigMgr::instance()

#endif // ROOFTREE_CONFIG_H
