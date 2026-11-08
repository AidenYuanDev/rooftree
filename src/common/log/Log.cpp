/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Log.h"
#include "AppenderConsole.h"
#include "AppenderFile.h"
#include "Config.h"
#include "Errors.h"
#include "LogConfig.h"
#include "LogMessage.h"
#include "Logger.h"
#include "Strand.h"
#include "StringConvert.h"
#include "Util.h"
#include <cstdio>

namespace
{
constexpr std::string_view RootLogger = "root";

std::string StartTimestamp()
{
    time_t now = time(nullptr);
    tm local;
    localtime_r(&now, &local);
    return Rooftree::StringFormat("_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
        local.tm_hour, local.tm_min, local.tm_sec);
}

Optional<LogLevel> ParseLevel(std::string_view text)
{
    Optional<uint8> level = Rooftree::StringTo<uint8>(text);
    if (!level || *level > LOG_LEVEL_FATAL)
        return {};
    return LogLevel(*level);
}

// "Appender.Console" -> "Console"
std::string NameOf(std::string const& key)
{
    std::size_t dot = key.find('.');
    return dot == std::string::npos ? key : key.substr(dot + 1);
}
}

Log* Log::instance()
{
    static Log log;
    return &log;
}

Log::Log() : _lowest(LogLevel(LOG_LEVEL_FATAL + 1)), _logsTimestamp(StartTimestamp())
{
    RegisterAppender<AppenderConsole>();
    RegisterAppender<AppenderFile>();
    Publish(std::make_unique<Setup>());
}

Log::~Log()
{
    delete _strand;
}

void Log::RegisterAppender(uint8 type, Factory factory)
{
    std::scoped_lock lock(_changing);
    bool added = _factories.emplace(type, factory).second;
    ASSERT(added, "Appender type %u registered twice", uint32(type));
}

void Log::Initialize(Rooftree::Asio::IoContext* ioContext)
{
    if (ioContext && !_strand)
        _strand = new Rooftree::Asio::Strand(*ioContext);
    LoadFromConfig();
}

void Log::SetSynchronous()
{
    delete std::exchange(_strand, nullptr);
}

void Log::LoadFromConfig()
{
    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();

    std::scoped_lock lock(_changing);
    _logsDir = config->Get<std::string>("LogsDir", "");
    if (!_logsDir.empty() && _logsDir.back() != '/')
        _logsDir += '/';

    auto setup = std::make_unique<Setup>();
    for (std::string const& name : config->GetKeys("Appender"))
        AddAppender(*setup, "Appender." + name, LogConfig::GetAppenderConfigLine(*config, "Appender." + name));
    for (std::string const& name : config->GetKeys("Logger"))
        AddLogger(*setup, "Logger." + name, LogConfig::GetLoggerConfigLine(*config, "Logger." + name));

    // Something always reaches the console
    if (!setup->Loggers.contains(std::string(RootLogger)))
    {
        std::fprintf(stderr, "Log: no Logger.root, logging errors to the console (and Logger.server at info)\n");
        setup = std::make_unique<Setup>();
        AddAppender(*setup, "Appender.Console", "1,2,0");
        AddLogger(*setup, "Logger.root", "5,Console");
        AddLogger(*setup, "Logger.server", "3,Console");
    }

    Replace(std::move(setup));
}

void Log::Close()
{
    std::scoped_lock lock(_changing);
    auto setup = std::make_unique<Setup>();
    Replace(std::move(setup));
}

void Log::CreateAppenderFromConfigLine(std::string const& key, std::string const& line)
{
    std::scoped_lock lock(_changing);
    auto setup = std::make_unique<Setup>(*_current.load());
    AddAppender(*setup, key, line);
    Replace(std::move(setup));
}

void Log::CreateLoggerFromConfigLine(std::string const& key, std::string const& line)
{
    std::scoped_lock lock(_changing);
    auto setup = std::make_unique<Setup>(*_current.load());
    AddLogger(*setup, key, line);
    Publish(std::move(setup));
}

// Type,Level,Flags,its own values...
void Log::AddAppender(Setup& setup, std::string const& key, std::string const& line)
{
    std::string name = NameOf(key);
    std::vector<std::string_view> values = Rooftree::Tokenize(line, ',', true);
    Optional<uint8> type = values.size() >= 2 ? Rooftree::StringTo<uint8>(values[0]) : Optional<uint8>();
    Optional<LogLevel> level = values.size() >= 2 ? ParseLevel(values[1]) : Optional<LogLevel>();
    Optional<uint8> flags = values.size() >= 3 ? Rooftree::StringTo<uint8>(values[2]) : Optional<uint8>(0);
    auto factory = type ? _factories.find(*type) : _factories.end();
    if (factory == _factories.end() || !level || !flags)
    {
        std::fprintf(stderr, "Log: %s: '%s' isn't Type,Level,Flags,... with a known type and a level from 0 to 6\n", key.c_str(), line.c_str());
        return;
    }

    try
    {
        Appender* appender = factory->second(_nextAppenderId++, std::move(name), *level, AppenderFlags(*flags), values);
        _appenders.emplace_back(appender);
        std::erase_if(setup.Appenders, [&](Appender* other) { return other->GetName() == appender->GetName(); });
        setup.Appenders.push_back(appender);
    }
    catch (InvalidAppenderArgs const& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
    }
}

// Level,Appender Appender...
void Log::AddLogger(Setup& setup, std::string const& key, std::string const& line)
{
    std::string name = NameOf(key);
    std::vector<std::string_view> values = Rooftree::Tokenize(line, ',', true);
    Optional<LogLevel> level = values.size() == 2 ? ParseLevel(values[0]) : Optional<LogLevel>();
    if (!level)
    {
        std::fprintf(stderr, "Log: %s: '%s' isn't Level,Appenders with a level from 0 to 6\n", key.c_str(), line.c_str());
        return;
    }
    if (setup.Loggers.contains(name))
    {
        std::fprintf(stderr, "Log: %s is there twice\n", key.c_str());
        return;
    }

    std::vector<Appender*> appenders;
    for (std::string_view appenderName : Rooftree::Tokenize(values[1], ' ', false))
    {
        auto appender = std::ranges::find(setup.Appenders, appenderName, &Appender::GetName);
        if (appender != setup.Appenders.end())
            appenders.push_back(*appender);
        else
            std::fprintf(stderr, "Log: %s: there is no appender %.*s\n", key.c_str(), int(appenderName.size()), appenderName.data());
    }

    setup.Loggers[name] = _loggers.emplace_back(std::make_unique<Logger>(name, *level, std::move(appenders))).get();
}

void Log::Publish(std::unique_ptr<Setup> setup)
{
    // The loggers' levels as they are now, SetLogLevel's included
    LogLevel lowest = LogLevel(LOG_LEVEL_FATAL + 1);
    for (auto const& [name, logger] : setup->Loggers)
        if (logger->GetLevel() != LOG_LEVEL_DISABLED)
            lowest = std::min(lowest, logger->GetLevel());
    _lowest = lowest;

    // A reader takes the generation before the setup (FindLogger): it never keeps what it found in an
    // old setup under the new generation
    _current.store(setup.get(), std::memory_order_release);
    _setups.push_back(std::move(setup));
    _generation.fetch_add(1, std::memory_order_release);
}

// The new one first: a thread still writing through the old one meanwhile loses no line
void Log::Replace(std::unique_ptr<Setup> setup)
{
    Setup const& old = *_current.load();
    Publish(std::move(setup));
    Setup const& next = *_current.load();
    for (Appender* appender : old.Appenders)
        if (std::ranges::find(next.Appenders, appender) == next.Appenders.end())
            appender->Retire();
}

bool Log::SetLogLevel(std::string const& name, int32 level, bool isLogger)
{
    if (level < LOG_LEVEL_DISABLED || level > LOG_LEVEL_FATAL)
        return false;

    std::scoped_lock lock(_changing);
    Setup const& setup = *_current.load();
    if (!isLogger)
    {
        auto appender = std::ranges::find(setup.Appenders, name, &Appender::GetName);
        if (appender == setup.Appenders.end())
            return false;
        (*appender)->SetLevel(LogLevel(level));
        return true;
    }

    auto logger = setup.Loggers.find(name);
    if (logger == setup.Loggers.end())
        return false;
    logger->second->SetLevel(LogLevel(level));
    if (level != LOG_LEVEL_DISABLED && LogLevel(level) < _lowest)
        _lowest = LogLevel(level);
    return true;
}

void Log::SetRealmId(uint32 realmId)
{
    std::scoped_lock lock(_changing);
    for (Appender* appender : _current.load()->Appenders)
        appender->SetRealmId(realmId);
}

Logger const* Log::GetEnabledLogger(std::string_view type, LogLevel level) const
{
    // Most lines aren't wanted: decided here, without a lookup
    if (level < _lowest.load(std::memory_order_relaxed))
        return nullptr;

    Logger const* logger = FindLogger(type);
    return logger && logger->IsEnabled(level) ? logger : nullptr;
}

Logger const* Log::FindLogger(std::string_view type) const
{
    // What each name falls back to, per thread, until the next change: no lock, no walking up the dots
    struct Hash : std::hash<std::string_view> { using is_transparent = void; };
    struct Cache
    {
        uint32 Generation = 0;
        std::unordered_map<std::string, Logger const*, Hash, std::equal_to<>> Loggers;
    };
    thread_local Cache cache;

    uint32 generation = _generation.load(std::memory_order_acquire);
    if (cache.Generation != generation)
    {
        cache.Loggers.clear();
        cache.Generation = generation;
    }
    if (auto cached = cache.Loggers.find(type); cached != cache.Loggers.end())
        return cached->second;

    Setup const& setup = *_current.load(std::memory_order_acquire);
    Logger const* logger = nullptr;
    for (std::string_view name = type;;)
    {
        if (auto found = setup.Loggers.find(std::string(name)); found != setup.Loggers.end())
        {
            logger = found->second;
            break;
        }
        if (name == RootLogger)
            break;
        std::size_t dot = name.rfind('.');
        name = dot == std::string_view::npos ? RootLogger : name.substr(0, dot);
    }
    cache.Loggers.emplace(type, logger);
    return logger;
}

void Log::Write(Logger const* logger, std::string_view type, LogLevel level, std::string text)
{
    LogMessage message{ .Level = level, .Type = std::string(type), .Text = std::move(text), .Time = time(nullptr) };
    if (_strand)
        Rooftree::Asio::post(*_strand, [logger, message = std::move(message)] { logger->Write(message); });
    else
        logger->Write(message);
}
