/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "AppenderFile.h"
#include "Log.h"
#include "LogMessage.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include <algorithm>
#include <cstdio>
#include <utility>

AppenderFile::AppenderFile(uint8 id, std::string name, LogLevel level, AppenderFlags flags, std::vector<std::string_view> const& args)
    : Appender(id, std::move(name), level, flags)
{
    if (args.size() <= 3 || args[3].empty())
        throw InvalidAppenderArgs(Rooftree::StringFormat("Log: file appender {} has no file name", GetName()));

    std::string fileName(args[3]);
    if (flags & APPENDER_FLAGS_USE_TIMESTAMP)
    {
        std::size_t dot = fileName.rfind('.');
        fileName.insert(dot == std::string::npos ? fileName.size() : dot, sLog->GetLogsTimestamp());
    }
    _path = sLog->GetLogsDir() + fileName;

    std::string mode = args.size() > 4 && !args[4].empty() ? std::string(args[4]) : "a";
    if (mode != "a" && mode != "w")
        throw InvalidAppenderArgs(Rooftree::StringFormat("Log: file appender {} has mode '{}', a or w", GetName(), mode));

    if (args.size() > 5 && !args[5].empty())
    {
        Optional<uint64> maxSize = Rooftree::StringTo<uint64>(args[5]);
        if (!maxSize)
            throw InvalidAppenderArgs(Rooftree::StringFormat("Log: file appender {} has an invalid size '{}'", GetName(), args[5]));
        _maxSize = *maxSize;
    }

    if (mode == "w" && (flags & APPENDER_FLAGS_MAKE_FILE_BACKUP))
        Backup();
    Open(mode.c_str());
}

AppenderFile::~AppenderFile()
{
    Close();
}

void AppenderFile::Write(LogMessage const& message, std::string const& prefix)
{
    std::string line = prefix + message.Text + '\n';
    if (_maxSize && _size + line.size() > _maxSize && _file)
    {
        Close();
        Backup();
        Open("w");
    }
    if (!_file)
        return;

    // One fwrite: lines of several threads never mix
    std::fwrite(line.data(), 1, line.size(), _file);
    std::fflush(_file);
    _size += line.size();
}

void AppenderFile::Close()
{
    if (_file)
        std::fclose(std::exchange(_file, nullptr));
}

void AppenderFile::Backup()
{
    LogMessage now{ .Level = LOG_LEVEL_INFO, .Type = {}, .Text = {}, .Time = time(nullptr) };
    std::string backup = _path + '.' + now.FormatTime();
    std::ranges::replace(backup, ':', '-');
    // Nothing to keep if there is no file yet
    std::rename(_path.c_str(), backup.c_str());
}

void AppenderFile::Open(char const* mode)
{
    _file = std::fopen(_path.c_str(), mode);
    if (!_file)
    {
        std::fprintf(stderr, "Log: file appender %s can't open %s\n", GetName().c_str(), _path.c_str());
        return;
    }
    std::fseek(_file, 0, SEEK_END);
    _size = uint64(std::ftell(_file));
}
