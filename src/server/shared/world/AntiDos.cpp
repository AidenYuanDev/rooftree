/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "AntiDos.h"
#include "Config.h"
#include "ConnectionLimiter.h"
#include "Log.h"
#include "ServerMessage.h"

namespace Rooftree::Server
{
Optional<AntiDosSettings> AntiDosSettings::FromConfig(Config const& config, std::string const& path)
{
    return FromConfig(config, path, AntiDosSettings());
}

Optional<AntiDosSettings> AntiDosSettings::FromConfig(Config const& config, std::string const& path, AntiDosSettings defaults)
{
    AntiDosSettings settings = std::move(defaults);

    std::string_view defaultPolicy = settings.Policy == AntiDosPolicy::Log ? "Log" : settings.Policy == AntiDosPolicy::Kick ? "Kick" : "Reject";
    std::string policy = config.Get<std::string>(path + ".Policy", std::string(defaultPolicy));
    if (policy == "Log")
        settings.Policy = AntiDosPolicy::Log;
    else if (policy == "Reject")
        settings.Policy = AntiDosPolicy::Reject;
    else if (policy == "Kick")
        settings.Policy = AntiDosPolicy::Kick;
    else
    {
        RT_LOG_ERROR("server.antidos", "{}.Policy must be Log, Reject or Kick, not {}", path, policy);
        return {};
    }

    settings.KickDuration = Seconds(config.Get<uint32>(path + ".KickDuration",
        uint32(std::chrono::duration_cast<Seconds>(settings.KickDuration).count())));

    if (config.GetNode(path + ".Exempt"))
    {
        settings.Exempt.clear();
        for (std::string const& entry : config.Get<std::vector<std::string>>(path + ".Exempt", {}))
        {
            boost::system::error_code error;
            boost::asio::ip::address address = boost::asio::ip::make_address(entry, error);
            if (error)
            {
                RT_LOG_ERROR("server.antidos", "{}.Exempt: {} is not an address", path, entry);
                return {};
            }
            settings.Exempt.push_back(address);
        }
    }

    if (YAML::Node blockAfter = config.GetNode(path + ".BlockAfter"))
    {
        YAML::Node window = blockAfter.IsMap() ? blockAfter["Window"] : YAML::Node();
        YAML::Node rejections = blockAfter.IsMap() ? blockAfter["Rejections"] : YAML::Node();
        uint32 windowSeconds = 0, count = 0;
        if (!window || !rejections || !YAML::convert<uint32>::decode(window, windowSeconds)
            || !YAML::convert<uint32>::decode(rejections, count) || !windowSeconds)
        {
            RT_LOG_ERROR("server.antidos", "{}.BlockAfter needs Window (seconds, > 0) and Rejections (0: never)", path);
            return {};
        }

        settings.BlockAfterWindow = Seconds(windowSeconds);
        settings.BlockAfterRejections = count;
    }

    // Type names contain dots ("comment.create"), walk the mapping itself instead of looking them up
    YAML::Node limits = config.GetNode(path + ".Limits");
    if (!limits)
        return settings;

    if (!limits.IsMap())
    {
        RT_LOG_ERROR("server.antidos", "{}.Limits must be a mapping of message type -> {{ Window, MaxCount }}", path);
        return {};
    }

    for (auto const& entry : limits)
    {
        std::string name = entry.first.as<std::string>();
        YAML::Node const& limit = entry.second;
        YAML::Node window = limit.IsMap() ? limit["Window"] : YAML::Node();
        YAML::Node maxCount = limit.IsMap() ? limit["MaxCount"] : YAML::Node();
        uint32 windowSeconds = 0, count = 0;
        if (!window || !maxCount || !YAML::convert<uint32>::decode(window, windowSeconds)
            || !YAML::convert<uint32>::decode(maxCount, count) || !windowSeconds)
        {
            RT_LOG_ERROR("server.antidos", "{}.Limits.{} needs Window (seconds, > 0) and MaxCount", path, name);
            return {};
        }

        settings.Limits[name] = { .Window = Seconds(windowSeconds), .MaxCount = count };
    }

    return settings;
}

void AntiDosTable::Load(AntiDosSettings const& settings, MessageTypeRegistry const& types)
{
    _policy = settings.Policy;
    _kickDuration = settings.KickDuration;
    _blockAfterRejections = settings.BlockAfterRejections;
    _blockAfterWindow = settings.BlockAfterWindow;
    _exempt.clear();
    for (boost::asio::ip::address const& address : settings.Exempt)
        _exempt.push_back(Net::ConnectionLimiter::Normalize(address));
    _limits.assign(types.GetCount(), {});
    for (auto const& [name, limit] : settings.Limits)
    {
        MessageType const* type = types.Find(name);
        if (!type)
        {
            RT_LOG_WARN("server.antidos", "AntiDOS limit for unknown message type {}, ignored", name);
            continue;
        }

        _limits[type->Id] = limit;
    }
}

bool AntiDosTable::IsExempt(boost::asio::ip::address const& address) const
{
    return !_exempt.empty() && std::ranges::find(_exempt, Net::ConnectionLimiter::Normalize(address)) != _exempt.end();
}

bool AntiDosCounter::Evaluate(AntiDosTable const& table, MessageType const& type, TimePoint now)
{
    AntiDosLimit const* limit = table.GetLimit(type.Id);
    if (!limit)
        return true;

    std::deque<TimePoint>& history = _history[type.Id];
    while (!history.empty() && now - history.front() >= limit->Window)
        history.pop_front();

    if (history.size() >= limit->MaxCount)
        return false;

    history.push_back(now);
    return true;
}

bool AntiDosCounter::IsEmpty(TimePoint now, AntiDosTable const& table)
{
    std::erase_if(_history, [&](auto& entry)
    {
        AntiDosLimit const* limit = table.GetLimit(entry.first);
        std::deque<TimePoint>& history = entry.second;
        while (limit && !history.empty() && now - history.front() >= limit->Window)
            history.pop_front();
        return !limit || history.empty();
    });

    return _history.empty();
}

bool AntiDosCounter::CountViolation(MessageType const& type, TimePoint now)
{
    auto [itr, started] = _runs.try_emplace(type.Id, ViolationRun{ .Type = &type, .LastLogged = now, .LastViolation = now });
    if (!started)
    {
        itr->second.LastViolation = now;
        ++itr->second.Unlogged;
    }
    return started;
}

std::vector<AntiDosReport> AntiDosCounter::TakeReports(AntiDosTable const& table, TimePoint now, Milliseconds interval, bool sessionRemoved)
{
    std::vector<AntiDosReport> reports;
    std::erase_if(_runs, [&](auto& entry)
    {
        ViolationRun& run = entry.second;
        AntiDosLimit const* limit = table.GetLimit(entry.first);
        AntiDosReport report{ .Type = run.Type, .Count = run.Unlogged,
            .Over = std::chrono::duration_cast<Milliseconds>(run.LastViolation - run.LastLogged), .RunState = AntiDosReport::State::StillOver };

        // A limit removed by a reload ends its runs too
        bool ended = sessionRemoved || !limit || now - run.LastViolation >= limit->Window;
        if (ended)
        {
            report.RunState = sessionRemoved ? AntiDosReport::State::SessionRemoved : AntiDosReport::State::Ended;
            if (run.Unlogged || (run.Continued && !sessionRemoved))
                reports.push_back(report);
            return true;
        }

        if (run.Unlogged && now - run.LastLogged >= interval)
        {
            report.Over = std::chrono::duration_cast<Milliseconds>(now - run.LastLogged);
            reports.push_back(report);
            run.LastLogged = now;
            run.Unlogged = 0;
            run.Continued = true;
        }
        return false;
    });

    return reports;
}
}
