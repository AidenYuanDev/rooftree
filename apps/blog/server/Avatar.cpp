/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Avatar.h"
#include "BlogDatabase.h"
#include "Field.h"
#include "HttpClient.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "StringFormat.h"
#include <filesystem>
#include <fstream>

namespace Blog
{
namespace
{
std::string WorkerGetAvatarName(uint32 userId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_AVATAR);
    stmt->setUInt32(0, userId);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    return result ? result->Fetch()[0].GetStringOrNull().value_or("") : "";
}

void WorkerSetAvatarName(uint32 userId, std::string_view name)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_AVATAR);
    if (name.empty())
        stmt->setNull(0);
    else
        stmt->setString(0, name);
    stmt->setUInt32(1, userId);
    BlogDatabase.DirectExecute(stmt);
}

// The provider's own way of asking for a size: GitHub ?s=256, Google's suffix =s96-c
std::string SizedAvatarUrl(std::string_view provider, std::string url)
{
    if (provider == "google")
    {
        std::size_t equals = url.rfind('=');
        if (equals != std::string::npos && url.find('/', equals) == std::string::npos)
            url.resize(equals);
        return url + "=s256-c";
    }
    return url + (url.find('?') == std::string::npos ? "?s=256" : "&s=256");
}
}

Optional<ImageType> DetectImage(std::string_view data)
{
    auto startsWith = [&](std::string_view magic, std::size_t offset = 0)
    {
        return data.size() >= offset + magic.size() && data.substr(offset, magic.size()) == magic;
    };

    if (startsWith("\xFF\xD8\xFF"))
        return ImageType{ "image/jpeg", "jpg" };
    if (startsWith("\x89PNG\r\n\x1A\n"))
        return ImageType{ "image/png", "png" };
    if (startsWith("GIF87a") || startsWith("GIF89a"))
        return ImageType{ "image/gif", "gif" };
    if (startsWith("RIFF") && startsWith("WEBP", 8))
        return ImageType{ "image/webp", "webp" };
    return {};
}

std::string_view AvatarExtension(std::string_view data)
{
    if (data.starts_with("\xFF\xD8\xFF"))
        return "jpg";
    if (data.starts_with("\x89PNG\r\n\x1A\n"))
        return "png";
    if (data.starts_with("RIFF") && data.size() >= 12 && data.substr(8, 4) == "WEBP")
        return "webp";
    return {};
}

std::string WorkerSaveAvatar(uint32 userId, std::string_view data, std::string const& directory)
{
    std::string name = Rooftree::StringFormat("avatar-{}.{}", Rooftree::Server::GenerateToken(12), AvatarExtension(data));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    {
        std::ofstream out(directory + "/" + name, std::ios::binary);
        out.write(data.data(), std::streamsize(data.size()));
        if (!out)
            return {};
    }

    std::string previous = WorkerGetAvatarName(userId);
    WorkerSetAvatarName(userId, name);
    RemoveAvatarFile(directory, previous);
    return name;
}

void WorkerRemoveAvatar(uint32 userId, std::string const& directory)
{
    std::string previous = WorkerGetAvatarName(userId);
    WorkerSetAvatarName(userId, {});
    RemoveAvatarFile(directory, previous);
}

void RemoveAvatarFile(std::string const& directory, std::string_view name)
{
    // Only our own names: nothing else in the directory is touched
    if (!name.starts_with("avatar-") || name.find('/') != std::string_view::npos || name.find("..") != std::string_view::npos)
        return;

    std::error_code error;
    std::filesystem::remove(directory + "/" + std::string(name), error);
}

Rooftree::Result<std::string> WorkerFetchProviderAvatar(std::string_view provider, std::string const& url)
{
    Rooftree::Server::HttpClientResponse response = Rooftree::Server::HttpFetch({ .Url = SizedAvatarUrl(provider, url), .Timeout = 10s });
    if (response.Status != 200)
        return std::unexpected(response.Status ? Rooftree::StringFormat("the picture's address answered {}", response.Status) : response.Error);
    if (response.Body.size() > MaxAvatarSize || AvatarExtension(response.Body).empty())
        return std::unexpected("not a JPEG, PNG or WebP picture of at most 512 KiB");
    return std::move(response.Body);
}
}
