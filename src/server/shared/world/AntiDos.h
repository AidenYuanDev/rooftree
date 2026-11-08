/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Flood protection by message type: each type may have a limit "at most MaxCount within Window",
 * counted per session. The limits come from the config, and the window is a sliding one, so a client
 * can't send 2 * MaxCount around the boundary of two fixed windows.
 *
 * What happens to a message over the limit is the policy:
 *   Log    - log it and process the message anyway (find the right limits before enforcing them)
 *   Reject - log it and reject the message (an HTTP request gets 429)
 *   Kick   - reject it, close the session and refuse its key for KickDuration
 * The session's OnAntiDosViolation() runs for every violation, for an app that wants to ban.
 *
 * Messages from the Exempt addresses are never counted, for the host's own clients (a
 * prerender reading every post through the API).
 *
 * A session refused BlockAfter.Rejections times within BlockAfter.Window has its
 * address blocked for KickDuration: the network threads then drop its connections and
 * requests before they cost anything. Rejecting is cheap, but not free: a client that ignores 429s and
 * sends thousands a second would keep the server busy answering them.
 *
 * A run of violations is logged when it starts, then at most once per interval with how many
 * came since, and once more when the session keeps within the limit for a whole window. A client that
 * floods would otherwise write a line per message, as fast as it sends them, and push everything else
 * out of the log.
 */

#ifndef ROOFTREE_ANTI_DOS_H
#define ROOFTREE_ANTI_DOS_H

#include "Define.h"
#include "Duration.h"
#include "Optional.h"
#include <boost/asio/ip/address.hpp>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

class Config;

namespace Rooftree::Server
{
class MessageTypeRegistry;
struct MessageType;

enum class AntiDosPolicy
{
    Log,
    Reject,
    Kick
};

struct AntiDosLimit
{
    Milliseconds Window = 1s;
    uint32 MaxCount = 0;    // 0: no limit
};

struct AntiDosSettings
{
    AntiDosPolicy Policy = AntiDosPolicy::Reject;
    Milliseconds KickDuration = 1min;
    std::unordered_map<std::string, AntiDosLimit> Limits;  // by message type name
    std::vector<boost::asio::ip::address> Exempt;           // clients never counted, never blocked
    uint32 BlockAfterRejections = 0;                        // 0: never block an address
    Milliseconds BlockAfterWindow = 10s;

    // <path>.Policy (Log, Reject, Kick), <path>.KickDuration (seconds), <path>.Limits, a mapping of
    // type name -> { Window: seconds, MaxCount: n }, <path>.Exempt, a list of addresses, and
    // <path>.BlockAfter, { Window: seconds, Rejections: n }. A missing
    // key keeps its value in defaults, a type the mapping doesn't name keeps its default limit (MaxCount
    // 0 lifts one). Empty if something is malformed
    static Optional<AntiDosSettings> FromConfig(Config const& config, std::string const& path, AntiDosSettings defaults);
    static Optional<AntiDosSettings> FromConfig(Config const& config, std::string const& path);
};

// The limits indexed by message type id, built on the main thread from the settings
class AntiDosTable
{
public:
    void Load(AntiDosSettings const& settings, MessageTypeRegistry const& types);

    AntiDosLimit const* GetLimit(uint32 typeId) const
    {
        return typeId < _limits.size() && _limits[typeId].MaxCount ? &_limits[typeId] : nullptr;
    }

    // IPv4 clients of a dual stack listener (::ffff:a.b.c.d) as IPv4
    bool IsExempt(boost::asio::ip::address const& address) const;

    uint32 GetBlockAfterRejections() const { return _blockAfterRejections; }
    Milliseconds GetBlockAfterWindow() const { return _blockAfterWindow; }

    AntiDosPolicy GetPolicy() const { return _policy; }
    Milliseconds GetKickDuration() const { return _kickDuration; }

private:
    std::vector<AntiDosLimit> _limits;
    std::vector<boost::asio::ip::address> _exempt;
    uint32 _blockAfterRejections = 0;
    Milliseconds _blockAfterWindow = 10s;
    AntiDosPolicy _policy = AntiDosPolicy::Reject;
    Milliseconds _kickDuration = 1min;
};

// What a run of violations of one type did since its last line
struct AntiDosReport
{
    enum class State
    {
        StillOver,      // the interval passed, the run goes on
        Ended,          // within the limit for a whole window again
        SessionRemoved
    };

    MessageType const* Type;
    uint64 Count;           // violations since the last line
    Milliseconds Over;      // from the last line to the last of them
    State RunState;
};

// Per session: when the messages of each limited type were let through, within its window, and the
// violations not logged yet
class AntiDosCounter
{
public:
    // Counts the message if it is within its type's limit, false if over it (not counted then)
    bool Evaluate(AntiDosTable const& table, MessageType const& type, TimePoint now);

    // Nothing counted within any window anymore
    bool IsEmpty(TimePoint now, AntiDosTable const& table);

    // A message over its limit. True if it starts a run of violations of its type, to be logged right
    // away; the rest of the run is counted for TakeReports()
    bool CountViolation(MessageType const& type, TimePoint now);

    bool HasViolations() const { return !_runs.empty(); }

    // The runs due a line: every interval while one goes on, once more when it ended (unless the first
    // line was all there was to say). sessionRemoved: every run with violations not logged yet, all forgotten
    std::vector<AntiDosReport> TakeReports(AntiDosTable const& table, TimePoint now, Milliseconds interval, bool sessionRemoved = false);

private:
    struct ViolationRun
    {
        MessageType const* Type;
        TimePoint LastLogged;
        TimePoint LastViolation;
        uint64 Unlogged = 0;
        bool Continued = false;     // a StillOver line was written
    };

    std::unordered_map<uint32, std::deque<TimePoint>> _history;
    std::unordered_map<uint32, ViolationRun> _runs;     // by type id
};
}

#endif // ROOFTREE_ANTI_DOS_H
