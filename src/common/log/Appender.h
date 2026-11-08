/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Where lines go: the console, a file, a database. An appender has a level of its own on top of the
 * logger's, and flags for what goes before each line (time, level, the logger's name).
 *
 * A type of its own is registered with sLog->RegisterAppender<T>(): T has a static AppenderType Type and
 * a constructor (id, name, level, flags, args), args being the config line's values, its own from the
 * fourth on. It throws InvalidAppenderArgs for args it can't use; the config then has no such appender.
 */

#ifndef ROOFTREE_APPENDER_H
#define ROOFTREE_APPENDER_H

#include "LogCommon.h"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>

struct LogMessage;

class Appender
{
public:
    Appender(uint8 id, std::string name, LogLevel level, AppenderFlags flags = APPENDER_FLAGS_NONE)
        : _id(id), _name(std::move(name)), _level(level), _flags(flags) { }
    Appender(Appender const&) = delete;
    Appender& operator=(Appender const&) = delete;
    virtual ~Appender() = default;

    uint8 GetId() const { return _id; }
    std::string const& GetName() const { return _name; }
    AppenderFlags GetFlags() const { return _flags; }
    LogLevel GetLevel() const { return _level; }
    void SetLevel(LogLevel level) { _level = level; }
    virtual AppenderType GetType() const = 0;
    // The database appender's: what it writes as realm, and only once it has one
    virtual void SetRealmId(uint32 /*realmId*/) { }

    // Any thread: one line at a time, whole
    void Append(LogMessage const& message);
    // Replaced by a reload: writes nothing from now on and lets go of its file. It isn't destroyed, a
    // thread may still be writing through a logger of before
    void Retire();

private:
    // Under the appender's lock. prefix: what the flags put before the text
    virtual void Write(LogMessage const& message, std::string const& prefix) = 0;
    virtual void Close() { }

    uint8 _id;
    std::string _name;
    std::atomic<LogLevel> _level;
    AppenderFlags _flags;
    std::mutex _writing;
    bool _retired = false;
};

class InvalidAppenderArgs : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

#endif // ROOFTREE_APPENDER_H
