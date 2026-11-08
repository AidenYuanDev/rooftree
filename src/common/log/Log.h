/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * RT_LOG_INFO("server.http", "{} answered {}", who, status): a line to the logger of that name, or of
 * the nearest name above it ("server.http" falls back to "server", then "root"), if that logger's level
 * lets it through. The format is checked when compiled; nothing is formatted for a line nobody wants.
 *
 * sLog->LoadFromConfig() reads LogsDir, Appender.* and Logger.* (examples/rooftree.yaml.dist), again on
 * a reload while other threads log: the new setup is built aside and then swapped in, the loggers and
 * appenders it replaces stop writing but stay in memory, since a thread may be using one right then.
 * Reloads are rare, what they leave behind is small.
 *
 * Lines are written on the thread that logs, unless sLog->Initialize(ioContext) asked for them to go
 * through that io_context, in order (a strand): then logging never waits for a disk.
 */

#ifndef ROOFTREE_LOG_H
#define ROOFTREE_LOG_H

#include "AsioHacksFwd.h"
#include "LogCommon.h"
#include "StringFormat.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Appender;
class Config;
class Logger;

class Log
{
public:
    static Log* instance();

    Log(Log const&) = delete;
    Log& operator=(Log const&) = delete;

    // Lines go through ioContext from now on, then LoadFromConfig
    void Initialize(Rooftree::Asio::IoContext* ioContext);
    // Back to writing on the thread that logs. Only once nothing else logs (the io_context is stopped)
    void SetSynchronous();
    void LoadFromConfig();
    // Every logger and appender off, as if nothing was configured
    void Close();

    // The logger a line of type at level goes to, null when it isn't wanted. Any thread
    Logger const* GetEnabledLogger(std::string_view type, LogLevel level) const;
    bool ShouldLog(std::string_view type, LogLevel level) const { return GetEnabledLogger(type, level) != nullptr; }

    template<typename... Args>
    void OutMessageTo(Logger const* logger, std::string_view type, LogLevel level, Rooftree::FormatString<Args...> format, Args&&... args)
    {
        Write(logger, type, level, Rooftree::StringVFormat(format.get(), Rooftree::MakeFormatArgs(args...)));
    }

    template<typename... Args>
    void OutMessage(std::string_view type, LogLevel level, Rooftree::FormatString<Args...> format, Args&&... args)
    {
        if (Logger const* logger = GetEnabledLogger(type, level))
            OutMessageTo(logger, type, level, format, std::forward<Args>(args)...);
    }

    // A logger's level (isLogger) or an appender's, by its name; false without one of that name
    bool SetLogLevel(std::string const& name, int32 level, bool isLogger = true);
    // One-line config values, "Appender.Console" "1,3,7"; also what LoadFromConfig does for each
    void CreateAppenderFromConfigLine(std::string const& key, std::string const& line);
    void CreateLoggerFromConfigLine(std::string const& key, std::string const& line);

    template<typename AppenderType>
    void RegisterAppender() { RegisterAppender(AppenderType::Type, &Make<AppenderType>); }

    // For the database appender, see AppenderDB.h
    void SetRealmId(uint32 realmId);

    // Ends in '/' unless empty
    std::string const& GetLogsDir() const { return _logsDir; }
    // _2026-11-07_19-31-48, the process's start, for file names
    std::string const& GetLogsTimestamp() const { return _logsTimestamp; }

private:
    using Factory = Appender* (*)(uint8 id, std::string name, LogLevel level, AppenderFlags flags, std::vector<std::string_view> const& args);

    template<typename AppenderType>
    static Appender* Make(uint8 id, std::string name, LogLevel level, AppenderFlags flags, std::vector<std::string_view> const& args)
    {
        return new AppenderType(id, std::move(name), level, flags, args);
    }

    // What is configured at one time. Never changed once published, never freed
    struct Setup
    {
        std::vector<Appender*> Appenders;
        std::unordered_map<std::string, Logger*> Loggers;
    };

    Log();
    ~Log();

    void RegisterAppender(uint8 type, Factory factory);
    void Write(Logger const* logger, std::string_view type, LogLevel level, std::string text);
    Logger const* FindLogger(std::string_view type) const;

    // Under _changing
    void AddAppender(Setup& setup, std::string const& key, std::string const& line);
    void AddLogger(Setup& setup, std::string const& key, std::string const& line);
    void Publish(std::unique_ptr<Setup> setup);
    void Replace(std::unique_ptr<Setup> setup);

    std::unordered_map<uint8, Factory> _factories;
    std::mutex _changing;
    std::vector<std::unique_ptr<Setup>> _setups;            // every one published, the last is current
    std::vector<std::unique_ptr<Appender>> _appenders;      // every one made
    std::vector<std::unique_ptr<Logger>> _loggers;
    std::atomic<Setup const*> _current;
    std::atomic<LogLevel> _lowest;                         // the lowest level any logger lets through
    std::atomic<uint32> _generation = 0;                    // for the lookup cache of each thread
    uint8 _nextAppenderId = 0;

    std::string _logsDir;
    std::string _logsTimestamp;
    Rooftree::Asio::Strand* _strand = nullptr;
};

#define sLog Log::instance()

#define RT_LOG_MESSAGE_BODY(type, level, ...)                                                           \
    do {                                                                                                \
        if (Logger const* rooftreeLogger = sLog->GetEnabledLogger((type), (level)))                     \
            sLog->OutMessageTo(rooftreeLogger, (type), (level), __VA_ARGS__);                           \
    } while (0)

#define RT_LOG_TRACE(type, ...) RT_LOG_MESSAGE_BODY(type, LOG_LEVEL_TRACE, __VA_ARGS__)
#define RT_LOG_DEBUG(type, ...) RT_LOG_MESSAGE_BODY(type, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define RT_LOG_INFO(type, ...) RT_LOG_MESSAGE_BODY(type, LOG_LEVEL_INFO, __VA_ARGS__)
#define RT_LOG_WARN(type, ...) RT_LOG_MESSAGE_BODY(type, LOG_LEVEL_WARN, __VA_ARGS__)
#define RT_LOG_ERROR(type, ...) RT_LOG_MESSAGE_BODY(type, LOG_LEVEL_ERROR, __VA_ARGS__)
#define RT_LOG_FATAL(type, ...) RT_LOG_MESSAGE_BODY(type, LOG_LEVEL_FATAL, __VA_ARGS__)

#endif // ROOFTREE_LOG_H
