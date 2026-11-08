/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Lines in a file under LogsDir. The config line's values from the fourth on: the file's name, the mode
 * (a appends, the default; w overwrites, after renaming the old file when the flags ask for a backup)
 * and the largest size in bytes: a line that would go over it starts the file over, the old one renamed
 * with the time. Flag 8 puts the server's start time in the name: Server_2026-11-07_19-31-48.log.
 */

#ifndef ROOFTREE_APPENDER_FILE_H
#define ROOFTREE_APPENDER_FILE_H

#include "Appender.h"
#include <cstdio>
#include <string_view>
#include <vector>

class AppenderFile : public Appender
{
public:
    static constexpr AppenderType Type = APPENDER_FILE;

    AppenderFile(uint8 id, std::string name, LogLevel level, AppenderFlags flags, std::vector<std::string_view> const& args);
    ~AppenderFile() override;

    AppenderType GetType() const override { return Type; }

private:
    void Write(LogMessage const& message, std::string const& prefix) override;
    void Close() override;

    // Renames the file there, if any, to its name with the time
    void Backup();
    void Open(char const* mode);

    std::string _path;
    uint64 _maxSize = 0;        // 0: no limit
    uint64 _size = 0;
    FILE* _file = nullptr;
};

#endif // ROOFTREE_APPENDER_FILE_H
