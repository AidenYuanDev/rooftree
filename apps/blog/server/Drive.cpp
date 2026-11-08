/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "Drive.h"
#include "BlogDatabase.h"
#include "BlogSettings.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "StringConvert.h"
#include "Validation.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <set>

namespace Blog::Drive
{
namespace
{
void RemoveFile(std::string const& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error)
        RT_LOG_ERROR("blog.drive", "Could not remove {}: {}", path, error.message());
}

// Removes the files of stored names (with their small pictures), and the part files of "upload:<id>" ones
void RemoveStored(std::string const& directory, std::vector<std::string> const& names)
{
    for (std::string const& name : names)
    {
        if (name.starts_with("upload:"))
            RemoveFile(PartPath(directory, std::string_view(name).substr(7)));
        else
        {
            RemoveFile(FilePath(directory, name));
            RemoveFile(ThumbPath(directory, name));
        }
    }
}

std::vector<std::string> Strings(PreparedQueryResult result)
{
    std::vector<std::string> values;
    if (result)
    {
        do
            values.push_back(result->Fetch()[0].GetString());
        while (result->NextRow());
    }
    return values;
}
}

FolderRow FolderRow::FromFields(Field const* fields)
{
    FolderRow folder;
    folder.Id = fields[0].GetUInt32();
    folder.ParentId = fields[1].GetUInt32OrNull();
    folder.Name = fields[2].GetString();
    folder.CreatedAt = fields[3].GetDate();
    folder.TrashedAt = fields[4].GetDateOrNull();
    return folder;
}

FileRow FileRow::FromFields(Field const* fields)
{
    FileRow file;
    file.Id = fields[0].GetUInt32();
    file.FolderId = fields[1].GetUInt32OrNull();
    file.Name = fields[2].GetString();
    file.Size = fields[3].GetUInt64();
    file.Mime = fields[4].GetString();
    file.CreatedAt = fields[5].GetDate();
    file.UpdatedAt = fields[6].GetDate();
    file.StoredName = fields[7].GetString();
    file.TrashedAt = fields[8].GetDateOrNull();
    file.Thumb = fields[9].GetStringOrNull().value_or("");
    return file;
}

bool IsValidName(std::string_view name)
{
    if (name.empty() || name == "." || name == ".." || CountCharacters(name) > MaxNameLength || !IsCleanText(name, false))
        return false;
    // IsCleanText trims: no spaces around it either
    if (name.front() == ' ' || name.back() == ' ')
        return false;
    return name.find_first_of("/\\") == std::string_view::npos;
}

std::string MimeOf(std::string_view name)
{
    static std::map<std::string, std::string, std::less<>> const types =
    {
        { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "png", "image/png" }, { "gif", "image/gif" }, { "webp", "image/webp" },
        { "avif", "image/avif" }, { "bmp", "image/bmp" }, { "ico", "image/x-icon" }, { "svg", "image/svg+xml" },
        { "mp4", "video/mp4" }, { "m4v", "video/mp4" }, { "webm", "video/webm" }, { "mov", "video/quicktime" }, { "mkv", "video/x-matroska" },
        { "mp3", "audio/mpeg" }, { "m4a", "audio/mp4" }, { "aac", "audio/aac" }, { "wav", "audio/wav" }, { "flac", "audio/flac" },
        { "ogg", "audio/ogg" }, { "oga", "audio/ogg" }, { "opus", "audio/ogg" },
        { "pdf", "application/pdf" }, { "zip", "application/zip" }, { "gz", "application/gzip" }, { "tar", "application/x-tar" },
        { "7z", "application/x-7z-compressed" }, { "json", "application/json" }, { "xml", "application/xml" },
        { "txt", "text/plain" }, { "md", "text/markdown" }, { "log", "text/plain" }, { "csv", "text/csv" }, { "html", "text/html" },
        { "htm", "text/html" }, { "css", "text/css" }, { "js", "text/javascript" }, { "ts", "text/plain" }, { "yaml", "text/yaml" },
        { "yml", "text/yaml" }, { "toml", "text/plain" }, { "ini", "text/plain" }, { "conf", "text/plain" }, { "sh", "text/x-shellscript" },
        { "c", "text/x-c" }, { "h", "text/x-c" }, { "cpp", "text/x-c++" }, { "hpp", "text/x-c++" }, { "cc", "text/x-c++" },
        { "py", "text/x-python" }, { "rs", "text/plain" }, { "go", "text/plain" }, { "java", "text/plain" }, { "lua", "text/plain" },
        { "sql", "text/plain" },
    };

    std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return "application/octet-stream";

    std::string extension(name.substr(dot + 1));
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    auto itr = types.find(extension);
    return itr != types.end() ? itr->second : "application/octet-stream";
}

bool IsInlineType(std::string_view mime)
{
    if (mime == "image/svg+xml")
        return false;
    return mime.starts_with("image/") || mime.starts_with("video/") || mime.starts_with("audio/") || mime == "application/pdf";
}

std::string ContentDisposition(bool attachment, std::string_view name)
{
    std::string ascii;
    std::string encoded;
    for (unsigned char c : name)
    {
        ascii += (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? char(c) : '_';
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '~')
            encoded += char(c);
        else
            encoded += Rooftree::StringFormat("%{:02X}", c);
    }
    return Rooftree::StringFormat("{}; filename=\"{}\"; filename*=UTF-8''{}", attachment ? "attachment" : "inline", ascii, encoded);
}

std::string NumberedName(std::string_view name, uint32 n)
{
    std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return Rooftree::StringFormat("{} ({})", name, n);
    return Rooftree::StringFormat("{} ({}){}", name.substr(0, dot), n, name.substr(dot));
}

ByteRange ParseRange(std::string_view header, uint64 size)
{
    ByteRange range;
    if (!header.starts_with("bytes=") || header.find(',') != std::string_view::npos)
        return range;

    std::string_view spec = header.substr(6);
    std::size_t dash = spec.find('-');
    if (dash == std::string_view::npos)
        return range;

    std::string_view first = spec.substr(0, dash);
    std::string_view last = spec.substr(dash + 1);
    if (first.empty())
    {
        // The last n bytes
        Optional<uint64> suffix = Rooftree::StringTo<uint64>(last);
        if (!suffix)
            return range;
        if (*suffix == 0 || size == 0)
            return { .Type = ByteRange::Unsatisfiable };
        range.First = size - std::min(*suffix, size);
        range.Last = size - 1;
    }
    else
    {
        Optional<uint64> from = Rooftree::StringTo<uint64>(first);
        if (!from)
            return range;
        if (*from >= size)
            return { .Type = ByteRange::Unsatisfiable };
        Optional<uint64> to = last.empty() ? Optional<uint64>(size - 1) : Rooftree::StringTo<uint64>(last);
        if (!to || *to < *from)
            return range;
        range.First = *from;
        range.Last = std::min(*to, size - 1);
    }

    range.Type = ByteRange::Partial;
    return range;
}

uint64 QuotaOf(UserRow const& user, BlogSettings const& settings)
{
    if (user.DriveQuota)
        return *user.DriveQuota;
    return user.IsAdmin() ? settings.DriveAdminQuota : settings.DriveQuota;
}

uint32 TrashDaysOf(UserRow const& user, BlogSettings const& settings)
{
    return std::min(user.DriveTrashDays.value_or(settings.DriveTrashDays), MaxTrashDays);
}

std::string FilePath(std::string const& directory, std::string_view storedName)
{
    return directory + "/files/" + std::string(storedName);
}

std::string ThumbPath(std::string const& directory, std::string_view storedName)
{
    return directory + "/thumbs/" + std::string(storedName);
}

std::string_view ThumbType(std::string_view bytes)
{
    if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP")
        return "webp";
    if (bytes.starts_with("\xff\xd8\xff"))
        return "jpeg";
    if (bytes.starts_with("\x89PNG\r\n\x1a\n"))
        return "png";
    return {};
}

std::string PartPath(std::string const& directory, std::string_view uploadId)
{
    return directory + "/parts/" + std::string(uploadId);
}

std::pair<uint64, uint64> WorkerUsage(uint32 userId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_USAGE);
    stmt->setUInt32(0, userId);
    stmt->setUInt32(1, userId);
    stmt->setUInt32(2, userId);
    stmt->setUInt32(3, userId);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return { 0, 0 };
    return { result->Fetch()[0].GetUInt64(), result->Fetch()[1].GetUInt64() };
}

Optional<FolderRow> WorkerFolder(uint32 userId, uint32 folderId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_FOLDER);
    stmt->setUInt32(0, folderId);
    stmt->setUInt32(1, userId);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};
    FolderRow folder = FolderRow::FromFields(result->Fetch());
    // In the root, or under folders none of which is in the trash
    if (folder.TrashedAt || (folder.ParentId && !WorkerAllFolders(userId).contains(folderId)))
        return {};
    return folder;
}

Optional<FileRow> WorkerFile(uint32 userId, uint32 fileId, bool withTrash)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_FILE);
    stmt->setUInt32(0, fileId);
    stmt->setUInt32(1, userId);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};
    FileRow file = FileRow::FromFields(result->Fetch());
    if (!withTrash && (file.TrashedAt || (file.FolderId && !WorkerFolder(userId, *file.FolderId))))
        return {};
    return file;
}

std::map<uint32, FolderRow> WorkerAllFolders(uint32 userId, bool withTrash)
{
    std::map<uint32, FolderRow> folders;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_ALL_FOLDERS);
    stmt->setUInt32(0, userId);
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
        {
            FolderRow folder = FolderRow::FromFields(rows->Fetch());
            folders.emplace(folder.Id, std::move(folder));
        }
        while (rows->NextRow());
    }
    if (withTrash)
        return folders;

    // Without those in the trash, and those under them: a folder is kept when the way up to the root
    // meets none (a cycle in the rows is dropped)
    std::map<uint32, FolderRow> live;
    for (auto const& [id, folder] : folders)
    {
        bool kept = true;
        std::size_t steps = 0;
        for (FolderRow const* at = &folder; kept; ++steps)
        {
            if (at->TrashedAt || steps > folders.size())
                kept = false;
            else if (!at->ParentId)
                break;
            else if (auto parent = folders.find(*at->ParentId); parent != folders.end())
                at = &parent->second;
            else
                kept = false;
        }
        if (kept)
            live.emplace(id, folder);
    }
    return live;
}

FolderContents WorkerListFolder(uint32 userId, Optional<uint32> folderId)
{
    FolderContents contents;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_SUBFOLDERS);
    stmt->setUInt32(0, userId);
    stmt->setUInt32(1, folderId.value_or(0));
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
            contents.Folders.push_back(FolderRow::FromFields(rows->Fetch()));
        while (rows->NextRow());
    }

    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_FILES);
    stmt->setUInt32(0, userId);
    stmt->setUInt32(1, folderId.value_or(0));
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
            contents.Files.push_back(FileRow::FromFields(rows->Fetch()));
        while (rows->NextRow());
    }

    return contents;
}

bool WorkerNameTaken(uint32 userId, Optional<uint32> folderId, std::string const& name)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_NAME_TAKEN);
    stmt->setUInt32(0, userId);
    stmt->setUInt32(1, folderId.value_or(0));
    stmt->setString(2, name);
    stmt->setUInt32(3, userId);
    stmt->setUInt32(4, folderId.value_or(0));
    stmt->setString(5, name);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    return result && result->Fetch()[0].GetInt64() != 0;
}

std::vector<FolderRow> PathOf(std::map<uint32, FolderRow> const& folders, Optional<uint32> folderId, Optional<uint32> stopAt)
{
    std::vector<FolderRow const*> up;
    bool reached = !stopAt;
    // At most every folder once: a cycle in the rows can't loop
    for (Optional<uint32> id = folderId; id && up.size() <= folders.size();)
    {
        auto itr = folders.find(*id);
        if (itr == folders.end())
            break;
        up.push_back(&itr->second);
        if (stopAt && *id == *stopAt)
        {
            reached = true;
            break;
        }
        id = itr->second.ParentId;
    }

    std::vector<FolderRow> path;
    if (!reached)
        return path;
    for (auto itr = up.rbegin(); itr != up.rend(); ++itr)
        path.push_back(**itr);
    return path;
}

bool IsUnder(std::map<uint32, FolderRow> const& folders, Optional<uint32> folderId, uint32 root)
{
    return folderId && !PathOf(folders, folderId, root).empty();
}

Optional<std::vector<ZipEntry>> WorkerZipEntries(uint32 userId, std::map<uint32, FolderRow> const& folders,
    std::vector<uint32> const& fileIds, std::vector<uint32> const& folderIds, std::string const& directory)
{
    std::map<Optional<uint32>, std::vector<FolderRow const*>> children;
    for (auto const& [id, folder] : folders)
        children[folder.ParentId].push_back(&folder);
    std::map<Optional<uint32>, std::vector<FileRow>> files;
    std::map<uint32, FileRow const*> filesById;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_LIVE_FILES);
    stmt->setUInt32(0, userId);
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
        {
            FileRow file = FileRow::FromFields(rows->Fetch());
            if (!file.FolderId || folders.contains(*file.FolderId))
                files[file.FolderId].push_back(std::move(file));
        }
        while (rows->NextRow());
    }
    for (auto const& [folder, list] : files)
        for (FileRow const& file : list)
            filesById.emplace(file.Id, &file);

    // What is at the top of the zip; under a folder names are unique already
    std::vector<FolderRow const*> topFolders;
    std::vector<FileRow const*> topFiles;
    if (fileIds.empty() && folderIds.empty())
    {
        topFolders = children[Optional<uint32>()];
        for (FileRow const& file : files[Optional<uint32>()])
            topFiles.push_back(&file);
    }
    for (uint32 id : folderIds)
    {
        auto itr = folders.find(id);
        if (itr == folders.end())
            return {};
        topFolders.push_back(&itr->second);
    }
    for (uint32 id : fileIds)
    {
        auto itr = filesById.find(id);
        if (itr == filesById.end())
            return {};
        topFiles.push_back(itr->second);
    }

    // Chosen from several folders, names may meet: "a (2).txt"
    std::set<std::string> topNames;
    auto unique = [&](std::string const& name)
    {
        std::string taken = name;
        for (uint32 n = 2; topNames.contains(taken) && n < 1000; ++n)
            taken = NumberedName(name, n);
        topNames.insert(taken);
        return taken;
    };

    std::vector<ZipEntry> entries;
    auto addFile = [&](FileRow const& file, std::string name)
    {
        entries.push_back({ .Name = std::move(name), .Path = FilePath(directory, file.StoredName), .Size = file.Size, .Modified = file.UpdatedAt });
    };
    // Depth first, at most as deep as there are folders (a cycle in the rows can't loop)
    std::function<void(FolderRow const&, std::string const&, std::size_t)> addFolder = [&](FolderRow const& folder, std::string const& path, std::size_t depth)
    {
        if (depth > folders.size() || entries.size() > MaxZipEntries)
            return;
        entries.push_back({ .Name = path, .Modified = folder.CreatedAt });
        for (FolderRow const* child : children[folder.Id])
            addFolder(*child, path + child->Name + "/", depth + 1);
        for (FileRow const& file : files[folder.Id])
            addFile(file, path + file.Name);
    };
    for (FolderRow const* folder : topFolders)
        addFolder(*folder, unique(folder->Name) + "/", 0);
    for (FileRow const* file : topFiles)
        addFile(*file, unique(file->Name));

    if (entries.size() > MaxZipEntries)
        return {};
    return entries;
}

void WorkerDeleteFolder(uint32 userId, uint32 folderId, std::string const& directory)
{
    std::map<uint32, FolderRow> folders = WorkerAllFolders(userId, true);
    std::string ids;
    for (auto const& [id, folder] : folders)
        if (IsUnder(folders, id, folderId))
            ids += (ids.empty() ? "" : ",") + std::to_string(id);

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_STORED_NAMES_IN_FOLDERS);
    stmt->setUInt32(0, userId);
    stmt->setString(1, ids);
    stmt->setUInt32(2, userId);
    stmt->setString(3, ids);
    std::vector<std::string> stored = Strings(BlogDatabase.Query(stmt));

    stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_DRIVE_SHARES_IN_FOLDERS);
    stmt->setUInt32(0, userId);
    stmt->setString(1, ids);
    stmt->setString(2, ids);
    BlogDatabase.DirectExecute(stmt);
    for (BlogDatabaseStatements statement : { BLOG_DEL_DRIVE_FILES_IN_FOLDERS, BLOG_DEL_DRIVE_UPLOADS_IN_FOLDERS })
    {
        stmt = BlogDatabase.GetPreparedStatement(statement);
        stmt->setUInt32(0, userId);
        stmt->setString(1, ids);
        BlogDatabase.DirectExecute(stmt);
    }

    stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_DRIVE_FOLDER);
    stmt->setUInt32(0, folderId);
    stmt->setUInt32(1, userId);
    BlogDatabase.DirectExecute(stmt);

    // The rows first: a file that can't be removed is only lost space, a row without its file a broken download
    RemoveStored(directory, stored);
}

void WorkerDeleteFile(uint32 userId, FileRow const& file, std::string const& directory)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_DRIVE_FILE);
    stmt->setUInt32(0, file.Id);
    stmt->setUInt32(1, userId);
    BlogDatabase.DirectExecute(stmt);
    RemoveStored(directory, { file.StoredName });
}

std::vector<TrashItem> WorkerListTrash(uint32 userId, uint32 days)
{
    std::map<uint32, FolderRow> folders = WorkerAllFolders(userId, true);
    // Where it was: the names of the folders above it
    auto from = [&](Optional<uint32> parent)
    {
        std::vector<std::string> names;
        for (FolderRow const& step : PathOf(folders, parent))
            names.push_back(step.Name);
        return names;
    };
    auto purgeAt = [days](SystemTimePoint trashedAt) { return trashedAt + std::chrono::days(days); };
    std::vector<TrashItem> items;

    std::map<uint32, uint64> sizes;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_FOLDER_SIZES);
    stmt->setUInt32(0, userId);
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
            sizes[rows->Fetch()[0].GetUInt32()] = rows->Fetch()[1].GetUInt64();
        while (rows->NextRow());
    }
    for (auto const& [id, folder] : folders)
    {
        if (!folder.TrashedAt)
            continue;
        uint64 size = 0;
        for (auto const& [sizeFolder, bytes] : sizes)
            if (IsUnder(folders, sizeFolder, id))
                size += bytes;
        items.push_back({ .Kind = "folder", .Id = id, .Name = folder.Name, .Size = size, .TrashedAt = *folder.TrashedAt,
            .PurgeAt = purgeAt(*folder.TrashedAt), .From = from(folder.ParentId) });
    }

    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_TRASHED_FILES);
    stmt->setUInt32(0, userId);
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
        {
            FileRow file = FileRow::FromFields(rows->Fetch());
            items.push_back({ .Kind = "file", .Id = file.Id, .Name = file.Name, .Size = file.Size, .Mime = file.Mime, .TrashedAt = *file.TrashedAt,
                .PurgeAt = purgeAt(*file.TrashedAt), .From = from(file.FolderId) });
        }
        while (rows->NextRow());
    }

    std::ranges::stable_sort(items, std::greater<>(), &TrashItem::TrashedAt);
    return items;
}

void WorkerPurgeTrash(std::string const& directory, uint32 defaultDays, Optional<uint32> userId, bool all)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_EXPIRED_TRASH);
    for (uint8 i = 0; i < 8; i += 4)
    {
        stmt->setUInt8(i, all ? 1 : 0);
        stmt->setUInt32(i + 1, defaultDays);
        stmt->setUInt32(i + 2, userId.value_or(0));
        stmt->setUInt32(i + 3, userId.value_or(0));
    }
    PreparedQueryResult rows = BlogDatabase.Query(stmt);
    if (!rows)
        return;

    // Folders first: a file in one goes with it
    uint32 count = 0;
    do
    {
        Field* fields = rows->Fetch();
        uint32 owner = fields[1].GetUInt32();
        uint32 id = fields[2].GetUInt32();
        if (fields[0].GetString() == "folder")
            WorkerDeleteFolder(owner, id, directory);
        else if (Optional<FileRow> file = WorkerFile(owner, id, true))
            WorkerDeleteFile(owner, *file, directory);
        ++count;
    }
    while (rows->NextRow());
    RT_LOG_INFO("blog.drive", "Emptied {} things from the trash{}", count, userId ? " of account " + std::to_string(*userId) : std::string());
}

void WorkerDeleteAccountDrive(uint32 userId, std::string const& directory)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_USER_STORED_NAMES);
    stmt->setUInt32(0, userId);
    std::vector<std::string> stored = Strings(BlogDatabase.Query(stmt));
    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_USER_UPLOADS);
    stmt->setUInt32(0, userId);
    for (std::string& upload : Strings(BlogDatabase.Query(stmt)))
        stored.push_back("upload:" + upload);

    for (BlogDatabaseStatements statement : { BLOG_DEL_DRIVE_USER_UPLOADS, BLOG_DEL_DRIVE_USER_FILES, BLOG_DEL_DRIVE_USER_FOLDERS })
    {
        stmt = BlogDatabase.GetPreparedStatement(statement);
        stmt->setUInt32(0, userId);
        BlogDatabase.DirectExecute(stmt);
    }

    RemoveStored(directory, stored);
    if (!stored.empty())
        RT_LOG_INFO("blog.drive", "Removed the drive of account {}: {} files", userId, stored.size());
}

void WorkerRemoveStaleUploads(std::string const& directory)
{
    for (std::string const& id : Strings(BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_STALE_UPLOADS))))
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_DRIVE_UPLOAD);
        stmt->setString(0, id);
        BlogDatabase.DirectExecute(stmt);
        RemoveFile(PartPath(directory, id));
    }

    // Parts whose upload went with its folder or account: not written for a day, older than any upload
    // may be
    std::error_code error;
    for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory + "/parts", error))
    {
        std::error_code timeError;
        auto written = entry.last_write_time(timeError);
        if (!timeError && std::filesystem::file_time_type::clock::now() - written >= 24h)
            RemoveFile(entry.path().string());
    }
}
}
