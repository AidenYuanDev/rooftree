/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "BlogTypes.h"
#include "Field.h"
#include <boost/json/array.hpp>
#include <chrono>
#include <fmt/chrono.h>

namespace Blog
{
SystemTimePoint PermanentBanTime()
{
    // Not 9999: SystemTimePoint counts nanoseconds in 64 bits and ends in 2262
    return std::chrono::sys_days(std::chrono::year(2200) / 1 / 1);
}

UserRow UserRow::FromFields(Field const* fields)
{
    UserRow user;
    user.Id = fields[0].GetUInt32();
    user.Username = fields[1].GetString();
    user.DisplayName = fields[2].GetString();
    user.Email = fields[3].GetString();
    user.EmailVerified = fields[4].GetInt64() != 0;
    user.PasswordHash = fields[5].GetStringOrNull().value_or("");
    user.HasPassword = !user.PasswordHash.empty();
    user.Role = fields[6].GetString();
    user.Locale = fields[7].GetString();
    user.TotpSecret = fields[8].GetBinaryOrNull();
    user.TotpLastStep = fields[9].GetInt64OrNull();
    user.BannedUntil = fields[10].GetDateOrNull();
    user.BanReason = fields[11].GetStringOrNull().value_or("");
    user.Avatar = fields[12].GetStringOrNull().value_or("");
    user.Denied = SplitList(fields[13].GetString());
    user.Deleted = fields[14].GetInt64() != 0;
    user.Granted = SplitList(fields[15].GetString());
    user.DriveQuota = fields[16].GetUInt64OrNull();
    user.DriveTrashDays = fields[17].GetUInt32OrNull();
    user.ReplyMails = fields[18].GetUInt8() != 0;
    user.EditsPosts = fields[19].GetInt64() != 0;
    return user;
}

std::string FormatTime(SystemTimePoint time)
{
    return fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::time_point_cast<std::chrono::seconds>(time));
}

boost::json::value FormatTime(Optional<SystemTimePoint> time)
{
    if (!time)
        return nullptr;

    return boost::json::value(FormatTime(*time));
}

std::vector<std::string> SplitList(std::string_view list)
{
    std::vector<std::string> values;
    while (!list.empty())
    {
        std::size_t comma = list.find(',');
        values.emplace_back(list.substr(0, comma));
        if (comma == std::string_view::npos)
            break;
        list.remove_prefix(comma + 1);
    }

    return values;
}

std::string JoinList(std::vector<std::string> const& values)
{
    std::string list;
    for (std::string const& value : values)
        list += (list.empty() ? "" : ",") + value;
    return list;
}
}
