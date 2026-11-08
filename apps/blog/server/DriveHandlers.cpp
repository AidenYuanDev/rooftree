/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The drive's API (Drive.h). /api/drive/... is the account's own drive: only accounts with one
 * (UserRow::IsGranted("drive")), and every query names the account, so an id of someone else's file
 * or folder is a 404 like one that doesn't exist.
 *
 * /api/shares/<token>/... is what a link shows, to any logged in account that isn't banned (the
 * owner's drive must still be on): a file, or a folder with what is under it, read only. A link with a
 * password asks for it once per account (drive_share_access.unlocked); wrong ones are limited per
 * account and link. The owner sees who opened a link, how often, and how often they downloaded.
 */

#include "Accounts.h"
#include "ApiCall.h"
#include "ApiOperations.h"
#include "Argon2Hash.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Drive.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "StringConvert.h"
#include "Util.h"
#include "Validation.h"
#include <filesystem>
#include <fstream>

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
using Drive::FileRow;
using Drive::FolderRow;

constexpr std::size_t ShareTokenLength = 32;    // GenerateToken(24)
constexpr std::size_t MaxSharePasswordLength = 128;
// Wrong passwords of one account for one link within the window, then it waits
constexpr std::size_t UnlockFailures = 5;
constexpr Seconds UnlockFailureWindow = 10min;
// Light: it guards one link, not an account
constexpr HashSettings SharePasswordHash = { .Iterations = 1, .MemoryCost = 8 * 1024 };

// Main thread: the wrong passwords per "<user>:<share>"
std::map<std::string, std::vector<SystemTimePoint>> UnlockFailureTimes;

// The trashes of every account are emptied of what expired this often, on a drive's listing
constexpr Seconds TrashSweepInterval = 1h;
SystemTimePoint LastTrashSweep;

Optional<uint32> GetPathId(BlogSession::Request const& request, std::string_view name = "id")
{
    Optional<uint32> id = Rooftree::StringTo<uint32>(request->GetPathParameter(name));
    return id && *id ? id : Optional<uint32>();
}

// ?folder=: none or 0 is the root. False when it isn't an id
bool GetFolderParameter(BlogSession::Request const& request, Optional<uint32>& folder)
{
    Optional<std::string> value = request->GetQueryParameter("folder");
    if (!value || value->empty() || *value == "0")
        return true;
    Optional<uint32> id = Rooftree::StringTo<uint32>(*value);
    if (!id)
        return false;
    folder = id;
    return true;
}

// A folder of a request: none or 0 is the root
Optional<uint32> FolderOf(flatbuffers::Optional<uint32> folder)
{
    return folder && *folder ? Optional<uint32>(*folder) : Optional<uint32>();
}

flatbuffers::Offset<Reader::DriveUsage> UsageTable(flatbuffers::FlatBufferBuilder& builder, UserRow const& user, uint64 used)
{
    BlogSettings const& settings = sWorld->GetSettings();
    return Reader::CreateDriveUsage(builder, used, Drive::QuotaOf(user, settings), settings.DriveMaxFileSize, Drive::ChunkSize,
        Drive::TrashDaysOf(user, settings));
}

flatbuffers::Offset<Reader::DriveFolder> FolderTable(flatbuffers::FlatBufferBuilder& builder, FolderRow const& folder)
{
    return Reader::CreateDriveFolder(builder, folder.Id, folder.ParentId ? flatbuffers::Optional<uint32>(*folder.ParentId) : flatbuffers::nullopt,
        builder.CreateString(folder.Name), TimeString(builder, folder.CreatedAt));
}

flatbuffers::Offset<Reader::DriveFile> FileTable(flatbuffers::FlatBufferBuilder& builder, FileRow const& file)
{
    return Reader::CreateDriveFile(builder, file.Id, file.FolderId ? flatbuffers::Optional<uint32>(*file.FolderId) : flatbuffers::nullopt,
        builder.CreateString(file.Name), file.Size, builder.CreateString(file.Mime), TimeString(builder, file.CreatedAt),
        TimeString(builder, file.UpdatedAt), !file.Thumb.empty());
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Reader::PathStep>>> PathTable(flatbuffers::FlatBufferBuilder& builder,
    std::vector<FolderRow> const& path)
{
    std::vector<flatbuffers::Offset<Reader::PathStep>> steps;
    for (FolderRow const& step : path)
        steps.push_back(Reader::CreatePathStep(builder, step.Id, builder.CreateString(step.Name)));
    return builder.CreateVector(steps);
}

// A folder's folders and files
std::pair<flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Reader::DriveFolder>>>,
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Reader::DriveFile>>>> ContentsTable(flatbuffers::FlatBufferBuilder& builder,
    Drive::FolderContents const& contents)
{
    std::vector<flatbuffers::Offset<Reader::DriveFolder>> folders;
    for (FolderRow const& folder : contents.Folders)
        folders.push_back(FolderTable(builder, folder));
    std::vector<flatbuffers::Offset<Reader::DriveFile>> files;
    for (FileRow const& file : contents.Files)
        files.push_back(FileTable(builder, file));
    return { builder.CreateVector(folders), builder.CreateVector(files) };
}

void RespondFolder(ApiRequest const& request, FolderRow const& folder, http::status status = http::status::ok)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(FolderTable(builder, folder));
    request->Respond(builder, status);
}

void RespondFile(ApiRequest const& request, FileRow const& file, http::status status = http::status::ok)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(FileTable(builder, file));
    request->Respond(builder, status);
}

void RespondDeleted(ApiRequest const& request, bool trashed)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateDriveDeleted(builder, true, trashed));
    request->Respond(builder);
}

void SetFolder(BlogDatabasePreparedStatement* stmt, uint8 index, Optional<uint32> folder)
{
    if (folder)
        stmt->setUInt32(index, *folder);
    else
        stmt->setNull(index);
}

// Worker: a file or folder (BLOG_UPD_DRIVE_*_TRASH) into the trash
void WorkerMoveToTrash(BlogDatabaseStatements statement, uint32 userId, uint32 id)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(statement);
    stmt->setUInt32(0, id);
    stmt->setUInt32(1, userId);
    BlogDatabase.DirectExecute(stmt);
}

// A file to send: opened at its first byte, how much of it
struct Download
{
    FileRow File;
    Drive::ByteRange Range;
    std::shared_ptr<std::ifstream> Stream;
};

// Worker: the file opened at the range's start; no stream when the file is gone from the disk
void WorkerOpen(Download& download, std::string const& directory)
{
    if (download.Range.Type == Drive::ByteRange::Unsatisfiable)
        return;

    auto stream = std::make_shared<std::ifstream>(Drive::FilePath(directory, download.File.StoredName), std::ios::binary);
    if (!*stream)
        return;
    if (download.Range.Type == Drive::ByteRange::Partial)
        stream->seekg(std::streamoff(download.Range.First));
    if (*stream)
        download.Stream = std::move(stream);
}

// Main thread: the file, whole or the range of it; shown by the browser (pictures, audio, video, PDF)
// unless asked to download, everything else downloaded
void RespondDownload(BlogSession::Request const& request, Download const& download, bool attachment)
{
    FileRow const& file = download.File;
    http::response<http::string_body>& response = request->GetResponse();
    response.set(http::field::accept_ranges, "bytes");
    response.set(http::field::cache_control, "private, no-cache");
    response.set("X-Content-Type-Options", "nosniff");
    if (download.Range.Type == Drive::ByteRange::Unsatisfiable)
    {
        response.set(http::field::content_range, "bytes */" + std::to_string(file.Size));
        return request->RespondError(http::status::range_not_satisfiable, "range_not_satisfiable");
    }

    if (!download.Stream)
    {
        RT_LOG_ERROR("blog.drive", "The file {} of drive file {} is missing", file.StoredName, file.Id);
        return RespondInternalError(request);
    }

    bool shown = !attachment && Drive::IsInlineType(file.Mime);
    response.set(http::field::content_type, shown ? file.Mime : "application/octet-stream");
    response.set(http::field::content_disposition, Drive::ContentDisposition(!shown, file.Name));
    // Nothing of it runs as our page; the browser's PDF viewer doesn't work sandboxed, and needs the
    // plugin our pages' policy forbids
    if (file.Mime != "application/pdf")
        response.set(http::field::content_security_policy, "sandbox; frame-ancestors 'self'");
    else
        response.set(http::field::content_security_policy, "default-src 'none'; object-src 'self'; style-src 'unsafe-inline'; frame-ancestors 'self'");
    // The drive's page shows it in a frame
    response.set(http::field::x_frame_options, "SAMEORIGIN");

    if (download.Range.Type == Drive::ByteRange::Partial)
    {
        response.set(http::field::content_range, Rooftree::StringFormat("bytes {}-{}/{}", download.Range.First, download.Range.Last, file.Size));
        return request->RespondStream(http::status::partial_content, download.Stream, download.Range.Last - download.Range.First + 1);
    }
    request->RespondStream(http::status::ok, download.Stream, file.Size);
}

// ?files=1,2&folders=3: ids, at most MaxZipEntries of them. False when one isn't an id
bool GetIdList(BlogSession::Request const& request, std::string_view name, std::vector<uint32>& ids)
{
    std::string value = request->GetQueryParameter(name).value_or("");
    for (std::string_view part : Rooftree::Tokenize(value, ',', false))
    {
        Optional<uint32> id = Rooftree::StringTo<uint32>(part);
        if (!id || !*id || ids.size() >= Drive::MaxZipEntries)
            return false;
        ids.push_back(*id);
    }
    return true;
}

// A zip to send, made on a worker (it only holds names and sizes; the files are read as it goes out)
struct Zip
{
    std::shared_ptr<std::istream> Stream;
    uint64 Length = 0;
    std::string Name;
};

Optional<Zip> MakeZip(Optional<std::vector<ZipEntry>> entries, std::string name)
{
    if (!entries)
        return {};
    auto [stream, length] = MakeZipStream(std::move(*entries));
    return Zip{ .Stream = std::move(stream), .Length = length, .Name = std::move(name) + ".zip" };
}

void RespondZip(BlogSession::Request const& request, Zip const& zip)
{
    http::response<http::string_body>& response = request->GetResponse();
    response.set(http::field::content_type, "application/zip");
    response.set(http::field::content_disposition, Drive::ContentDisposition(true, zip.Name));
    response.set(http::field::cache_control, "private, no-cache");
    response.set("X-Content-Type-Options", "nosniff");
    request->RespondStream(http::status::ok, zip.Stream, zip.Length);
}

// Worker: a file's small picture, empty when it has none (or it is gone from the disk)
std::string WorkerReadThumb(FileRow const& file, std::string const& directory)
{
    if (file.Thumb.empty())
        return {};
    std::ifstream in(Drive::ThumbPath(directory, file.StoredName), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes.size() <= Drive::MaxThumbSize ? bytes : std::string();
}

// The small picture, kept by the browser a day (a file's never changes but with it)
void RespondThumb(BlogSession::Request const& request, std::string_view type, std::string bytes)
{
    if (bytes.empty())
        return RespondNotFound(request);
    http::response<http::string_body>& response = request->GetResponse();
    response.result(http::status::ok);
    response.set(http::field::content_type, "image/" + std::string(type));
    response.set(http::field::cache_control, "private, max-age=86400");
    response.set("X-Content-Type-Options", "nosniff");
    response.set(http::field::content_security_policy, "sandbox; frame-ancestors 'self'");
    response.body() = std::move(bytes);
    request->Respond();
}

// A link as an account opens it
struct ShareRow
{
    uint32 Id = 0;
    uint32 OwnerId = 0;
    Optional<uint32> FileId;
    Optional<uint32> FolderId;
    std::string PasswordHash;
    Optional<SystemTimePoint> ExpiresAt;
    bool Expired = false;
    std::string OwnerName;
    bool OwnerHasDrive = false;
    bool Unlocked = false;
};

bool IsShareToken(std::string_view token)
{
    return token.size() == ShareTokenLength && std::ranges::all_of(token, [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

// Worker: none when there is no such link, or its owner's drive is off or gone
Optional<ShareRow> WorkerFindShare(std::string const& token, uint32 viewerId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_SHARE_BY_TOKEN);
    stmt->setUInt32(0, viewerId);
    stmt->setString(1, token);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    Field* fields = result->Fetch();
    ShareRow share;
    share.Id = fields[0].GetUInt32();
    share.OwnerId = fields[1].GetUInt32();
    share.FileId = fields[2].GetUInt32OrNull();
    share.FolderId = fields[3].GetUInt32OrNull();
    share.PasswordHash = fields[4].GetStringOrNull().value_or("");
    share.ExpiresAt = fields[5].GetDateOrNull();
    share.Expired = fields[6].GetInt64() != 0;
    share.OwnerName = fields[7].GetString();
    std::vector<std::string> granted = SplitList(fields[9].GetString());
    share.OwnerHasDrive = fields[10].GetInt64() == 0
        && (fields[8].GetString() == RoleAdmin || std::ranges::find(granted, GrantDrive) != granted.end());
    share.Unlocked = fields[11].GetInt64() != 0;
    if (!share.OwnerHasDrive)
        return {};
    return share;
}

void WorkerCount(BlogDatabaseStatements statement, uint32 shareId, uint32 userId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(statement);
    stmt->setUInt32(0, shareId);
    stmt->setUInt32(1, userId);
    BlogDatabase.DirectExecute(stmt);
}

// A row of BLOG_DRIVE_SHARE_COLUMNS, as its owner sees it
struct ShareItem
{
    uint32 Id;
    std::string Token, Kind;
    uint32 TargetId;
    std::string Name;
    bool HasPassword;
    Optional<SystemTimePoint> ExpiresAt;
    SystemTimePoint CreatedAt;
    int64 Visitors;
    uint64 Views, Downloads;
    bool Trashed;

    // folders: the owner's, but those in the trash
    static ShareItem FromFields(Field const* fields, std::map<uint32, FolderRow> const& folders)
    {
        Optional<uint32> fileFolder = fields[11].GetUInt32OrNull();
        // What it shows is in the trash: the link answers 404 until it is restored
        bool trashed = fields[2].IsNull() ? !folders.contains(fields[3].GetUInt32())
            : fields[12].GetInt64() != 0 || (fileFolder && !folders.contains(*fileFolder));
        return { .Id = fields[0].GetUInt32(), .Token = fields[1].GetString(), .Kind = fields[2].IsNull() ? "folder" : "file",
            .TargetId = fields[2].IsNull() ? fields[3].GetUInt32() : fields[2].GetUInt32(), .Name = fields[4].GetStringOrNull().value_or(""),
            .HasPassword = fields[5].GetInt64() != 0, .ExpiresAt = fields[6].GetDateOrNull(), .CreatedAt = fields[7].GetDate(),
            .Visitors = fields[8].GetInt64(), .Views = fields[9].GetUInt64(), .Downloads = fields[10].GetUInt64(), .Trashed = trashed };
    }

    flatbuffers::Offset<Reader::DriveShare> Table(flatbuffers::FlatBufferBuilder& builder) const
    {
        return Reader::CreateDriveShare(builder, Id, builder.CreateString(Token), builder.CreateString(Kind), TargetId, builder.CreateString(Name),
            HasPassword, TimeString(builder, ExpiresAt), ExpiresAt && *ExpiresAt <= SystemTimePoint::clock::now(), TimeString(builder, CreatedAt),
            Visitors, Views, Downloads, Trashed);
    }
};

// What a link answers before anything else: 404 none (also revoked, or what it showed was deleted),
// 410 ran out. True when it may be read; with a password not given yet the caller answers "locked"
bool CheckShare(BlogSession::Request const& request, Optional<ShareRow> const& share)
{
    if (!share)
    {
        RespondNotFound(request);
        return false;
    }
    if (share->Expired)
    {
        request->RespondError(http::status::gone, "share_expired");
        return false;
    }
    return true;
}

bool CheckShare(ApiRequest const& request, Optional<ShareRow> const& share)
{
    if (!share)
    {
        RespondNotFound(request);
        return false;
    }
    if (share->Expired)
    {
        request->RespondError(http::status::gone, "share_expired");
        return false;
    }
    return true;
}

bool IsLocked(ShareRow const& share, uint32 viewerId)
{
    return !share.PasswordHash.empty() && !share.Unlocked && share.OwnerId != viewerId;
}
}

namespace
{
// The drive viewer works on: its own (user none or its id), 403 no_drive without one; another account's
// for the admin only. what: the operation, logged when the admin changes another's drive. Nothing when
// refuse() was called
Rooftree::Task<Optional<UserRow>> FindDrive(Session& session, UserRow viewer, Optional<uint32> user, std::string what,
    std::function<void(http::status status, std::string_view code)> refuse)
{
    if (!user || *user == viewer.Id)
    {
        if (!viewer.IsGranted(GrantDrive))
        {
            refuse(http::status::forbidden, "no_drive");
            co_return {};
        }
        co_return viewer;
    }

    if (!viewer.IsAdmin())
    {
        refuse(http::status::forbidden, "forbidden");
        co_return {};
    }

    // The admin, on that account's drive (on or off, its files are there)
    Optional<UserRow> owner = co_await session.OnWorker([id = *user] { return FindUserById(id); });
    if (!owner || owner->Deleted)
    {
        refuse(http::status::not_found, "not_found");
        co_return {};
    }
    if (!what.empty())
        RT_LOG_INFO("blog.admin", "{} on the drive of {} ({}): {}", viewer.Username, owner->Username, owner->Id, what);
    co_return owner;
}

// An operation's drive, its request's user (DriveRequest and the like have one); changes says whether
// it changes something, for the log
Rooftree::Task<Optional<UserRow>> ResolveDrive(Session& session, ApiRequest request, flatbuffers::Optional<uint32> user, bool changes)
{
    co_return co_await FindDrive(session, request->GetAuth().User, user ? Optional<uint32>(*user) : Optional<uint32>(),
        changes ? std::string(GetOpcodeName(request->GetOpcode())) : std::string(), [request](http::status status, std::string_view code)
    {
        request->RespondError(status, code);
    });
}

Rooftree::Task<void> PurgeTrash(Session& session, uint32 defaultDays, std::string directory)
{
    co_await session.OnWorker([=] { Drive::WorkerPurgeTrash(directory, defaultDays); });
}
}

Rooftree::Task<Optional<UserRow>> BlogSession::RequireDrive(Request request)
{
    Optional<AuthInfo> login = co_await RequireUserAsync(request);
    if (!login)
        co_return {};
    AuthInfo const& auth = *login;

    // ?user=<id>: the admin, on that account's drive
    Optional<uint32> user;
    if (Optional<std::string> text = request->GetQueryParameter("user"); text && !text->empty())
    {
        user = Rooftree::StringTo<uint32>(*text);
        if (!user)
        {
            if (auth.User.IsAdmin())
                RespondNotFound(request);
            else
                request->RespondError(http::status::forbidden, "forbidden");
            co_return {};
        }
    }

    std::string what = request->GetMethod() != http::verb::get
        ? Rooftree::StringFormat("{} {}", std::string_view(http::to_string(request->GetMethod())), request->GetTarget()) : std::string();
    co_return co_await FindDrive(*this, auth.User, user, what, [request](http::status status, std::string_view code)
    {
        request->RespondError(status, code);
    });
}

// Its folders and files, the way there from the root, and how full the drive is
Rooftree::Task<void> ListDrive(Session& session, ApiRequest request)
{
    Reader::DriveListRequest const& in = request->Get<Reader::DriveListRequest>();
    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), false);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto folder = FolderOf(in.folder());

    BlogSettings const& settings = sWorld->GetSettings();
    if (SystemTimePoint now = SystemTimePoint::clock::now(); now - LastTrashSweep >= TrashSweepInterval)
    {
        LastTrashSweep = now;
        session.Spawn(PurgeTrash(session, settings.DriveTrashDays, settings.DriveDirectory));
    }

    struct Result
    {
        bool Found = false;
        Drive::FolderContents Contents;
        Optional<FolderRow> Folder;
        std::vector<FolderRow> Path;
        uint64 Used = 0;
    };

    auto user = owner;
    Result result = co_await session.OnWorker([userId = owner.Id, folder]
    {
        Result result;
        std::map<uint32, FolderRow> folders = Drive::WorkerAllFolders(userId);
        auto itr = folder ? folders.find(*folder) : folders.end();
        if (folder && itr == folders.end())
            return result;

        result.Found = true;
        result.Contents = Drive::WorkerListFolder(userId, folder);
        if (itr != folders.end())
            result.Folder = itr->second;
        result.Path = Drive::PathOf(folders, folder);
        result.Used = Drive::WorkerUsage(userId).first;
        return result;
    });
    if (!result.Found)
        co_return RespondNotFound(request);

    flatbuffers::FlatBufferBuilder builder;
    auto [folders, files] = ContentsTable(builder, result.Contents);
    auto shown = result.Folder ? FolderTable(builder, *result.Folder) : 0;
    auto path = PathTable(builder, result.Path);
    builder.Finish(Reader::CreateDriveList(builder, folders, files, shown, path, UsageTable(builder, user, result.Used)));
    request->Respond(builder);
}

// Every folder, for choosing where to move something
Rooftree::Task<void> ListDriveFolders(Session& session, ApiRequest request)
{
    Optional<UserRow> drive = co_await ResolveDrive(session, request, request->Get<Reader::DriveRequest>().user(), false);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    std::map<uint32, FolderRow> all = co_await session.OnWorker([userId = owner.Id] { return Drive::WorkerAllFolders(userId); });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::DriveFolder>> folders;
    for (auto const& [id, folder] : all)
        folders.push_back(FolderTable(builder, folder));
    builder.Finish(Reader::CreateDriveFolders(builder, builder.CreateVector(folders)));
    request->Respond(builder);
}

// {parentId, name}
Rooftree::Task<void> CreateDriveFolder(Session& session, ApiRequest request)
{
    Reader::DriveFolderInput const& in = request->Get<Reader::DriveFolderInput>();
    TableFields fields(request->GetErrors());
    std::string name = fields.Text("name", in.name(), 1, Drive::MaxNameLength);
    if (!name.empty() && !Drive::IsValidName(name))
        fields.AddError("name", "invalid");
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto parent = FolderOf(in.parentId());

    struct Result
    {
        std::string_view Error;
        Optional<FolderRow> Folder;
    };

    Result result = co_await session.OnWorker([userId = owner.Id, parent, name]
    {
        Result result;
        if (parent && !Drive::WorkerFolder(userId, *parent))
        {
            result.Error = "not_found";
            return result;
        }
        if (Drive::WorkerNameTaken(userId, parent, name))
        {
            result.Error = "name_taken";
            return result;
        }

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_DRIVE_FOLDER);
        stmt->setUInt32(0, userId);
        SetFolder(stmt, 1, parent);
        stmt->setString(2, name);
        BlogDatabase.DirectExecute(stmt);

        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_FOLDER_BY_NAME);
        stmt->setUInt32(0, userId);
        stmt->setUInt32(1, parent.value_or(0));
        stmt->setString(2, name);
        if (PreparedQueryResult row = BlogDatabase.Query(stmt))
            result.Folder = FolderRow::FromFields(row->Fetch());
        else
            result.Error = "internal_error";
        return result;
    });
    if (result.Error == "not_found")
        co_return RespondNotFound(request);
    if (result.Error == "name_taken")
        co_return request->RespondError(http::status::conflict, "name_taken");
    if (!result.Folder)
        co_return RespondInternalError(request);
    RespondFolder(request, *result.Folder, http::status::created);
}

// {name?, parentId?}: renamed, moved, or both. Never into itself or a folder under it
Rooftree::Task<void> UpdateDriveFolder(Session& session, ApiRequest request)
{
    Reader::DriveFolderInput const& in = request->Get<Reader::DriveFolderInput>();
    if (!in.id() || !*in.id())
        co_return RespondNotFound(request);

    TableFields fields(request->GetErrors());
    Optional<std::string> name;
    if (in.name())
    {
        name = fields.Text("name", in.name(), 1, Drive::MaxNameLength);
        if (!name->empty() && !Drive::IsValidName(*name))
            fields.AddError("name", "invalid");
    }
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    bool moved = in.parentId().has_value();
    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = *in.id();
    auto parent = FolderOf(in.parentId());

    struct Result
    {
        std::string_view Error;
        Optional<FolderRow> Folder;
    };

    Result result = co_await session.OnWorker([userId = owner.Id, id, name, parent, moved]
    {
        Result result;
        std::map<uint32, FolderRow> folders = Drive::WorkerAllFolders(userId);
        auto itr = folders.find(id);
        if (itr == folders.end() || (moved && parent && !folders.contains(*parent)))
        {
            result.Error = "not_found";
            return result;
        }

        FolderRow folder = itr->second;
        Optional<uint32> newParent = moved ? parent : folder.ParentId;
        std::string newName = name.value_or(folder.Name);
        if (moved && Drive::IsUnder(folders, newParent, id))
        {
            result.Error = "invalid_parent";
            return result;
        }
        if ((newParent != folder.ParentId || newName != folder.Name) && Drive::WorkerNameTaken(userId, newParent, newName))
        {
            result.Error = "name_taken";
            return result;
        }

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_DRIVE_FOLDER);
        SetFolder(stmt, 0, newParent);
        stmt->setString(1, newName);
        stmt->setUInt32(2, id);
        stmt->setUInt32(3, userId);
        BlogDatabase.DirectExecute(stmt);
        result.Folder = Drive::WorkerFolder(userId, id);
        return result;
    });
    if (result.Error == "invalid_parent")
        co_return RespondValidation(request, { { "parentId", "invalid" } });
    if (result.Error == "name_taken")
        co_return request->RespondError(http::status::conflict, "name_taken");
    if (!result.Folder)
        co_return RespondNotFound(request);
    RespondFolder(request, *result.Folder);
}

// With everything under it: into the trash, or for good when the account keeps none.
// {ok, trashed}
Rooftree::Task<void> DeleteDriveFolder(Session& session, ApiRequest request)
{
    Reader::DriveIdRequest const& in = request->Get<Reader::DriveIdRequest>();
    if (!in.id() || !*in.id())
        co_return RespondNotFound(request);

    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = *in.id();

    bool trash = Drive::TrashDaysOf(owner, sWorld->GetSettings()) > 0;
    bool found = co_await session.OnWorker([userId = owner.Id, id, trash, directory = sWorld->GetSettings().DriveDirectory]
    {
        if (!Drive::WorkerFolder(userId, id))
            return false;
        if (trash)
            WorkerMoveToTrash(BLOG_UPD_DRIVE_FOLDER_TRASH, userId, id);
        else
            Drive::WorkerDeleteFolder(userId, id, directory);
        return true;
    });
    if (!found)
        co_return RespondNotFound(request);
    RespondDeleted(request, trash);
}

// {name?, folderId?}
Rooftree::Task<void> UpdateDriveFile(Session& session, ApiRequest request)
{
    Reader::DriveFileInput const& in = request->Get<Reader::DriveFileInput>();
    if (!in.id() || !*in.id())
        co_return RespondNotFound(request);

    TableFields fields(request->GetErrors());
    Optional<std::string> name;
    if (in.name())
    {
        name = fields.Text("name", in.name(), 1, Drive::MaxNameLength);
        if (!name->empty() && !Drive::IsValidName(*name))
            fields.AddError("name", "invalid");
    }
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    bool moved = in.folderId().has_value();
    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = *in.id();
    auto folder = FolderOf(in.folderId());

    struct Result
    {
        std::string_view Error;
        Optional<FileRow> File;
    };

    Result result = co_await session.OnWorker([userId = owner.Id, id, name, folder, moved]
    {
        Result result;
        Optional<FileRow> file = Drive::WorkerFile(userId, id);
        if (!file || (moved && folder && !Drive::WorkerFolder(userId, *folder)))
            return result;

        Optional<uint32> newFolder = moved ? folder : file->FolderId;
        std::string newName = name.value_or(file->Name);
        if ((newFolder != file->FolderId || newName != file->Name) && Drive::WorkerNameTaken(userId, newFolder, newName))
        {
            result.Error = "name_taken";
            return result;
        }

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_DRIVE_FILE);
        SetFolder(stmt, 0, newFolder);
        stmt->setString(1, newName);
        stmt->setString(2, Drive::MimeOf(newName));
        stmt->setUInt32(3, id);
        stmt->setUInt32(4, userId);
        BlogDatabase.DirectExecute(stmt);
        result.File = Drive::WorkerFile(userId, id);
        return result;
    });
    if (result.Error == "name_taken")
        co_return request->RespondError(http::status::conflict, "name_taken");
    if (!result.File)
        co_return RespondNotFound(request);
    RespondFile(request, *result.File);
}

// Into the trash, or for good when the account keeps none. {ok, trashed}
Rooftree::Task<void> DeleteDriveFile(Session& session, ApiRequest request)
{
    Reader::DriveIdRequest const& in = request->Get<Reader::DriveIdRequest>();
    if (!in.id() || !*in.id())
        co_return RespondNotFound(request);

    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = *in.id();

    bool trash = Drive::TrashDaysOf(owner, sWorld->GetSettings()) > 0;
    bool found = co_await session.OnWorker([userId = owner.Id, id, trash, directory = sWorld->GetSettings().DriveDirectory]
    {
        Optional<FileRow> file = Drive::WorkerFile(userId, id);
        if (!file)
            return false;
        if (trash)
            WorkerMoveToTrash(BLOG_UPD_DRIVE_FILE_TRASH, userId, id);
        else
            Drive::WorkerDeleteFile(userId, *file, directory);
        return true;
    });
    if (!found)
        co_return RespondNotFound(request);
    RespondDeleted(request, trash);
}

// What the trash holds, the expired gone first: {trash: [...], days}
Rooftree::Task<void> ListDriveTrash(Session& session, ApiRequest request)
{
    Optional<UserRow> drive = co_await ResolveDrive(session, request, request->Get<Reader::DriveRequest>().user(), false);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    BlogSettings const& settings = sWorld->GetSettings();
    uint32 days = Drive::TrashDaysOf(owner, settings);
    std::vector<Drive::TrashItem> trash = co_await session.OnWorker([userId = owner.Id, days, defaultDays = settings.DriveTrashDays, directory = settings.DriveDirectory]
    {
        Drive::WorkerPurgeTrash(directory, defaultDays, userId);
        return Drive::WorkerListTrash(userId, days);
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::DriveTrashItem>> items;
    for (Drive::TrashItem const& item : trash)
        items.push_back(Reader::CreateDriveTrashItem(builder, builder.CreateString(item.Kind), item.Id, builder.CreateString(item.Name),
            item.Size, item.Mime ? builder.CreateString(*item.Mime) : 0, TimeString(builder, item.TrashedAt),
            TimeString(builder, item.PurgeAt), StringList(builder, item.From)));
    builder.Finish(Reader::CreateDriveTrash(builder, builder.CreateVector(items), days));
    request->Respond(builder);
}

// /api/drive/trash/{kind}/{id}/restore: back in its folder, or the root when that is gone or in the
// trash itself, as "name (2)" when its name was taken meanwhile. The file or folder, with {kind}
Rooftree::Task<void> RestoreFromTrash(Session& session, ApiRequest request)
{
    Reader::DriveTrashRequest const& in = request->Get<Reader::DriveTrashRequest>();
    std::string kind = ReadText(in.kind());
    if (!in.id() || !*in.id() || (kind != "file" && kind != "folder"))
        co_return RespondNotFound(request);

    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = *in.id();

    // The one restored, a file or a folder
    using Restored = std::variant<std::monostate, FileRow, FolderRow>;
    Restored restored = co_await session.OnWorker([userId = owner.Id, id, kind]() -> Restored
    {
        std::map<uint32, FolderRow> all = Drive::WorkerAllFolders(userId, true);
        std::map<uint32, FolderRow> live = Drive::WorkerAllFolders(userId);
        Optional<uint32> parent;
        std::string name;
        if (kind == "file")
        {
            Optional<FileRow> file = Drive::WorkerFile(userId, id, true);
            if (!file || !file->TrashedAt)
                return {};
            parent = file->FolderId;
            name = file->Name;
        }
        else
        {
            auto itr = all.find(id);
            if (itr == all.end() || !itr->second.TrashedAt)
                return {};
            parent = itr->second.ParentId;
            name = itr->second.Name;
        }
        if (parent && !live.contains(*parent))
            parent.reset();

        std::string unique = name;
        for (uint32 n = 2; Drive::WorkerNameTaken(userId, parent, unique) && n < 1000; ++n)
            unique = Drive::NumberedName(name, n);
        if (CountCharacters(unique) > Drive::MaxNameLength)
            return {};

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(kind == "file" ? BLOG_UPD_DRIVE_FILE_RESTORE : BLOG_UPD_DRIVE_FOLDER_RESTORE);
        SetFolder(stmt, 0, parent);
        stmt->setString(1, unique);
        uint8 index = 2;
        if (kind == "file")
            stmt->setString(index++, Drive::MimeOf(unique));
        stmt->setUInt32(index++, id);
        stmt->setUInt32(index, userId);
        BlogDatabase.DirectExecute(stmt);

        if (kind == "file")
        {
            if (Optional<FileRow> file = Drive::WorkerFile(userId, id))
                return *file;
        }
        else if (Optional<FolderRow> folder = Drive::WorkerFolder(userId, id))
            return *folder;
        return {};
    });
    flatbuffers::FlatBufferBuilder builder;
    if (FileRow const* file = std::get_if<FileRow>(&restored))
        builder.Finish(Reader::CreateDriveRestored(builder, builder.CreateString("file"), file->Id, flatbuffers::nullopt,
            file->FolderId.value_or(0), builder.CreateString(file->Name), file->Size, builder.CreateString(file->Mime),
            TimeString(builder, file->CreatedAt), TimeString(builder, file->UpdatedAt), !file->Thumb.empty()));
    else if (FolderRow const* folder = std::get_if<FolderRow>(&restored))
        builder.Finish(Reader::CreateDriveRestored(builder, builder.CreateString("folder"), folder->Id, folder->ParentId.value_or(0),
            flatbuffers::nullopt, builder.CreateString(folder->Name), flatbuffers::nullopt, 0, TimeString(builder, folder->CreatedAt)));
    else
        co_return RespondNotFound(request);
    request->Respond(builder);
}

// /api/drive/trash/{kind}/{id}: out of the trash for good
Rooftree::Task<void> PurgeFromTrash(Session& session, ApiRequest request)
{
    Reader::DriveTrashRequest const& in = request->Get<Reader::DriveTrashRequest>();
    std::string kind = ReadText(in.kind());
    if (!in.id() || !*in.id() || (kind != "file" && kind != "folder"))
        co_return RespondNotFound(request);

    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = *in.id();

    bool found = co_await session.OnWorker([userId = owner.Id, id, kind, directory = sWorld->GetSettings().DriveDirectory]
    {
        if (kind == "file")
        {
            Optional<FileRow> file = Drive::WorkerFile(userId, id, true);
            if (!file || !file->TrashedAt)
                return false;
            Drive::WorkerDeleteFile(userId, *file, directory);
            return true;
        }
        std::map<uint32, FolderRow> all = Drive::WorkerAllFolders(userId, true);
        auto itr = all.find(id);
        if (itr == all.end() || !itr->second.TrashedAt)
            return false;
        Drive::WorkerDeleteFolder(userId, id, directory);
        return true;
    });
    if (!found)
        co_return RespondNotFound(request);
    RespondOk(request);
}

// Everything in the trash, for good
Rooftree::Task<void> EmptyDriveTrash(Session& session, ApiRequest request)
{
    Optional<UserRow> drive = co_await ResolveDrive(session, request, request->Get<Reader::DriveRequest>().user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    BlogSettings const& settings = sWorld->GetSettings();
    co_await session.OnWorker([userId = owner.Id, defaultDays = settings.DriveTrashDays, directory = settings.DriveDirectory]
    {
        Drive::WorkerPurgeTrash(directory, defaultDays, userId, true);
    });
    RespondOk(request);
}

// ?download=1: as a download, else shown when the browser can (pictures, audio, video, PDF). Range
Rooftree::Task<void> BlogSession::HandleDriveDownload(Request request)
{
    Optional<UserRow> drive = co_await RequireDrive(request);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    Optional<uint32> id = GetPathId(request);
    if (!id)
        co_return RespondNotFound(request);

    bool attachment = request->GetQueryParameter("download").value_or("") == "1";
    Optional<Download> download = co_await OnWorker([userId = owner.Id, id = *id, range = std::string(request->GetHeader(http::field::range)),
        directory = sWorld->GetSettings().DriveDirectory]() -> Optional<Download>
    {
        Optional<FileRow> file = Drive::WorkerFile(userId, id);
        if (!file)
            return {};

        Download download{ .File = std::move(*file) };
        download.Range = Drive::ParseRange(range, download.File.Size);
        WorkerOpen(download, directory);
        return download;
    });
    if (!download)
        co_return RespondNotFound(request);
    RespondDownload(request, *download, attachment);
}

// The body: a picture or a video's small picture (WebP, JPEG or PNG, at most MaxThumbSize), made by
// the browser that uploaded it, or that opened it later
Rooftree::Task<void> BlogSession::HandleDriveSetThumb(Request request)
{
    Optional<UserRow> drive = co_await RequireDrive(request);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    Optional<uint32> id = GetPathId(request);
    if (!id)
        co_return RespondNotFound(request);
    std::string_view type = Drive::ThumbType(request->GetBody());
    if (request->GetBody().size() > Drive::MaxThumbSize)
        co_return RespondValidation(request, { { "body", "too_long" } });
    if (type.empty())
        co_return RespondValidation(request, { { "body", "invalid" } });

    std::string_view error = co_await OnWorker([request, userId = owner.Id, id = *id, type, directory = sWorld->GetSettings().DriveDirectory]() -> std::string_view
    {
        Optional<FileRow> file = Drive::WorkerFile(userId, id);
        if (!file)
            return "not_found";
        if (!file->Mime.starts_with("image/") && !file->Mime.starts_with("video/"))
            return "invalid";

        std::error_code error;
        std::filesystem::create_directories(directory + "/thumbs", error);
        std::ofstream out(Drive::ThumbPath(directory, file->StoredName), std::ios::binary | std::ios::trunc);
        out.write(request->GetBody().data(), std::streamsize(request->GetBody().size()));
        out.flush();
        if (!out)
            return "internal_error";

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_DRIVE_FILE_THUMB);
        stmt->setString(0, std::string(type));
        stmt->setUInt32(1, id);
        stmt->setUInt32(2, userId);
        BlogDatabase.DirectExecute(stmt);
        return {};
    });
    if (error == "not_found")
        co_return RespondNotFound(request);
    if (error == "invalid")
        co_return request->RespondError(http::status::bad_request, "not_a_picture");
    if (!error.empty())
        co_return RespondInternalError(request);
    request->RespondJson(http::status::ok, { { "ok", true } });
}

Rooftree::Task<void> BlogSession::HandleDriveThumb(Request request)
{
    Optional<UserRow> drive = co_await RequireDrive(request);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    Optional<uint32> id = GetPathId(request);
    if (!id)
        co_return RespondNotFound(request);

    std::pair<std::string, std::string> thumb = co_await OnWorker([userId = owner.Id, id = *id, directory = sWorld->GetSettings().DriveDirectory]() -> std::pair<std::string, std::string>
    {
        Optional<FileRow> file = Drive::WorkerFile(userId, id);
        if (!file)
            return {};
        return { file->Thumb, WorkerReadThumb(*file, directory) };
    });
    RespondThumb(request, thumb.first, std::move(thumb.second));
}

// A zip of the drive: ?folder= with all under it, or ?files=&folders= chosen (named ?name=.zip), or
// the whole drive
Rooftree::Task<void> BlogSession::HandleDriveZip(Request request)
{
    Optional<UserRow> drive = co_await RequireDrive(request);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    Optional<uint32> folder;
    std::vector<uint32> fileIds;
    std::vector<uint32> folderIds;
    if (!GetFolderParameter(request, folder) || !GetIdList(request, "files", fileIds) || !GetIdList(request, "folders", folderIds))
        co_return RespondNotFound(request);
    if (folder)
        folderIds.push_back(*folder);
    std::string name = request->GetQueryParameter("name").value_or("");
    if (!Drive::IsValidName(name))
        name.clear();

    Optional<Zip> zip = co_await OnWorker([userId = owner.Id, fileIds, folderIds, name, directory = sWorld->GetSettings().DriveDirectory]
    {
        std::map<uint32, FolderRow> folders = Drive::WorkerAllFolders(userId);
        std::string zipName = name;
        if (zipName.empty())
            zipName = folderIds.size() == 1 && fileIds.empty() && folders.contains(folderIds[0]) ? folders.at(folderIds[0]).Name : "drive";
        return MakeZip(Drive::WorkerZipEntries(userId, folders, fileIds, folderIds, directory), zipName);
    });
    if (!zip)
        co_return RespondNotFound(request);
    RespondZip(request, *zip);
}

// {folderId, name, size}: an upload to send in pieces of chunkSize, when the drive and the disk have
// room for it. Unfinished uploads count until they are done
Rooftree::Task<void> CreateDriveUpload(Session& session, ApiRequest request)
{
    Reader::DriveUploadInput const& in = request->Get<Reader::DriveUploadInput>();
    BlogSettings const& settings = sWorld->GetSettings();
    TableFields fields(request->GetErrors());
    std::string name = fields.Text("name", in.name(), 1, Drive::MaxNameLength);
    if (!name.empty() && !Drive::IsValidName(name))
        fields.AddError("name", "invalid");
    uint64 size = 0;
    if (!in.size() || *in.size() < 0)
        fields.AddError("size", "invalid");
    else if (uint64(*in.size()) > settings.DriveMaxFileSize)
        fields.AddError("size", "too_long");
    else
        size = uint64(*in.size());
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto folder = FolderOf(in.folderId());

    struct Result
    {
        std::string_view Error;
        std::string Id;
    };

    Result result = co_await session.OnWorker([userId = owner.Id, quota = Drive::QuotaOf(owner, settings), minFree = settings.DriveMinFreeSpace,
        directory = settings.DriveDirectory, folder, name, size]
    {
        Result result;
        if (folder && !Drive::WorkerFolder(userId, *folder))
        {
            result.Error = "not_found";
            return result;
        }

        Drive::WorkerRemoveStaleUploads(directory);
        if (Drive::WorkerUsage(userId).first + size > quota)
        {
            result.Error = "quota_exceeded";
            return result;
        }

        std::error_code error;
        std::filesystem::create_directories(directory + "/parts", error);
        std::filesystem::create_directories(directory + "/files", error);
        std::filesystem::space_info space = std::filesystem::space(directory, error);
        if (error || space.available < size + minFree)
        {
            if (error)
                RT_LOG_ERROR("blog.drive", "Blog.Drive.Directory {} can't be used: {}", directory, error.message());
            result.Error = "disk_full";
            return result;
        }

        result.Id = Rooftree::Server::GenerateToken(24);
        if (!std::ofstream(Drive::PartPath(directory, result.Id), std::ios::binary))
        {
            RT_LOG_ERROR("blog.drive", "Could not write into Blog.Drive.Directory {}", directory);
            result.Error = "internal_error";
            return result;
        }

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_DRIVE_UPLOAD);
        stmt->setString(0, result.Id);
        stmt->setUInt32(1, userId);
        SetFolder(stmt, 2, folder);
        stmt->setString(3, name);
        stmt->setUInt64(4, size);
        BlogDatabase.DirectExecute(stmt);
        return result;
    });
    if (result.Error == "not_found")
        co_return RespondNotFound(request);
    if (result.Error == "quota_exceeded" || result.Error == "disk_full")
        co_return request->RespondError(http::status::insufficient_storage, result.Error);
    if (!result.Error.empty())
        co_return RespondInternalError(request);

    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateDriveUploadDirect(builder, result.Id.c_str(), 0, Drive::ChunkSize));
    request->Respond(builder, http::status::created);
}

// ?offset=<bytes received so far>, the body the next piece. Another offset: 409 with the server's, to
// go on from there
Rooftree::Task<void> BlogSession::HandleDriveUploadChunk(Request request)
{
    Optional<UserRow> drive = co_await RequireDrive(request);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    std::string id(request->GetPathParameter("id"));
    Optional<uint64> offset = Rooftree::StringTo<uint64>(request->GetQueryParameter("offset").value_or(""));
    if (!offset)
        co_return RespondValidation(request, { { "offset", "invalid" } });
    if (request->GetBody().empty() || request->GetBody().size() > Drive::ChunkSize)
        co_return RespondValidation(request, { { "body", request->GetBody().empty() ? "required" : "too_long" } });

    struct Result
    {
        std::string_view Error;
        uint64 Received = 0;
    };

    // The body is only read: the worker reads it from the request, no copy of the piece
    Result result = co_await OnWorker([request, userId = owner.Id, id, offset = *offset, directory = sWorld->GetSettings().DriveDirectory]
    {
        Result result;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_UPLOAD);
        stmt->setString(0, id);
        stmt->setUInt32(1, userId);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
        {
            result.Error = "not_found";
            return result;
        }

        uint64 size = row->Fetch()[2].GetUInt64();
        result.Received = row->Fetch()[3].GetUInt64();
        std::string const& piece = request->GetBody();
        if (offset != result.Received)
        {
            result.Error = "offset_mismatch";
            return result;
        }
        if (result.Received + piece.size() > size)
        {
            result.Error = "too_long";
            return result;
        }

        // The part has what the row says: a piece written before a crash, and not counted, goes
        std::string path = Drive::PartPath(directory, id);
        std::error_code error;
        if (std::filesystem::file_size(path, error) != result.Received || error)
        {
            std::filesystem::resize_file(path, result.Received, error);
            if (error)
            {
                result.Error = "internal_error";
                return result;
            }
        }

        {
            std::ofstream out(path, std::ios::binary | std::ios::app);
            out.write(piece.data(), std::streamsize(piece.size()));
            out.flush();
            if (!out)
            {
                RT_LOG_ERROR("blog.drive", "Could not write the upload {} into Blog.Drive.Directory", id);
                result.Error = "internal_error";
                return result;
            }
        }

        result.Received += piece.size();
        stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_DRIVE_UPLOAD_RECEIVED);
        stmt->setUInt64(0, result.Received);
        stmt->setString(1, id);
        stmt->setUInt32(2, userId);
        BlogDatabase.DirectExecute(stmt);
        return result;
    });
    if (result.Error == "not_found")
        co_return RespondNotFound(request);
    if (result.Error == "offset_mismatch")
        co_return request->RespondError(http::status::conflict, "offset_mismatch", {}, { { "received", std::to_string(result.Received) } });
    if (result.Error == "too_long")
        co_return RespondValidation(request, { { "body", "too_long" } });
    if (!result.Error.empty())
        co_return RespondInternalError(request);
    request->RespondJson(http::status::ok, { { "received", result.Received } });
}

// Every piece there: the file, under a name of its own in its folder
Rooftree::Task<void> CompleteDriveUpload(Session& session, ApiRequest request)
{
    Reader::DriveUploadRequest const& in = request->Get<Reader::DriveUploadRequest>();
    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = ReadText(in.id());

    struct Result
    {
        std::string_view Error;
        Optional<FileRow> File;
    };

    auto user = owner.Username;
    Result result = co_await session.OnWorker([userId = owner.Id, id, directory = sWorld->GetSettings().DriveDirectory]
    {
        Result result;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_UPLOAD);
        stmt->setString(0, id);
        stmt->setUInt32(1, userId);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
        {
            result.Error = "not_found";
            return result;
        }

        Field* fields = row->Fetch();
        Optional<uint32> folder = fields[0].GetUInt32OrNull();
        std::string name = fields[1].GetString();
        uint64 size = fields[2].GetUInt64();
        if (fields[3].GetUInt64() != size)
        {
            result.Error = "incomplete";
            return result;
        }

        std::string unique = name;
        for (uint32 n = 2; Drive::WorkerNameTaken(userId, folder, unique) && n < 1000; ++n)
            unique = Drive::NumberedName(name, n);
        if (CountCharacters(unique) > Drive::MaxNameLength)
        {
            result.Error = "name_taken";
            return result;
        }

        std::string storedName = Rooftree::Server::GenerateToken(24);
        std::error_code error;
        std::filesystem::rename(Drive::PartPath(directory, id), Drive::FilePath(directory, storedName), error);
        if (error)
        {
            RT_LOG_ERROR("blog.drive", "Could not move the upload {} into place: {}", id, error.message());
            result.Error = "internal_error";
            return result;
        }

        stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_DRIVE_FILE);
        stmt->setUInt32(0, userId);
        SetFolder(stmt, 1, folder);
        stmt->setString(2, unique);
        stmt->setString(3, storedName);
        stmt->setUInt64(4, size);
        stmt->setString(5, Drive::MimeOf(unique));
        BlogDatabase.DirectExecute(stmt);

        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_DRIVE_UPLOAD);
        stmt->setString(0, id);
        BlogDatabase.DirectExecute(stmt);

        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_FILE_BY_STORED_NAME);
        stmt->setString(0, storedName);
        if (PreparedQueryResult file = BlogDatabase.Query(stmt))
            result.File = FileRow::FromFields(file->Fetch());
        else
        {
            // The name was taken right then by another upload
            std::filesystem::remove(Drive::FilePath(directory, storedName), error);
            result.Error = "name_taken";
        }
        return result;
    });
    if (result.Error == "not_found")
        co_return RespondNotFound(request);
    if (result.Error == "incomplete")
        co_return request->RespondError(http::status::conflict, "incomplete");
    if (result.Error == "name_taken")
        co_return request->RespondError(http::status::conflict, "name_taken");
    if (!result.File)
        co_return RespondInternalError(request);
    RT_LOG_INFO("blog.drive", "{} uploaded a file of {} bytes", user, result.File->Size);
    RespondFile(request, *result.File, http::status::created);
}

Rooftree::Task<void> CancelDriveUpload(Session& session, ApiRequest request)
{
    Reader::DriveUploadRequest const& in = request->Get<Reader::DriveUploadRequest>();
    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = ReadText(in.id());

    bool found = co_await session.OnWorker([userId = owner.Id, id, directory = sWorld->GetSettings().DriveDirectory]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_UPLOAD);
        stmt->setString(0, id);
        stmt->setUInt32(1, userId);
        if (!BlogDatabase.Query(stmt))
            return false;

        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_DRIVE_UPLOAD);
        stmt->setString(0, id);
        BlogDatabase.DirectExecute(stmt);
        std::error_code error;
        std::filesystem::remove(Drive::PartPath(directory, id), error);
        return true;
    });
    if (!found)
        co_return RespondNotFound(request);
    RespondOk(request);
}

Rooftree::Task<void> ListDriveShares(Session& session, ApiRequest request)
{
    Optional<UserRow> drive = co_await ResolveDrive(session, request, request->Get<Reader::DriveRequest>().user(), false);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    std::vector<ShareItem> shares = co_await session.OnWorker([userId = owner.Id]
    {
        std::vector<ShareItem> shares;
        std::map<uint32, FolderRow> folders = Drive::WorkerAllFolders(userId);
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_SHARES);
        stmt->setUInt32(0, userId);
        stmt->setUInt32(1, 0);
        stmt->setUInt32(2, 0);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
                shares.push_back(ShareItem::FromFields(rows->Fetch(), folders));
            while (rows->NextRow());
        }
        return shares;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::DriveShare>> items;
    for (ShareItem const& share : shares)
        items.push_back(share.Table(builder));
    builder.Finish(Reader::CreateDriveShares(builder, builder.CreateVector(items)));
    request->Respond(builder);
}

// {fileId | folderId, expires: 1d, 7d, 30d or never, password?}
Rooftree::Task<void> CreateDriveShare(Session& session, ApiRequest request)
{
    Reader::DriveShareInput const& in = request->Get<Reader::DriveShareInput>();
    TableFields fields(request->GetErrors());
    Optional<uint32> fileId = FolderOf(in.fileId());
    Optional<uint32> folderId = FolderOf(in.folderId());
    if (fileId.has_value() == folderId.has_value())
        fields.AddError(fileId ? "folderId" : "fileId", fileId ? "invalid" : "required");
    std::string expires = fields.Choice("expires", in.expires(), { "1d", "7d", "30d", "never" });
    std::string password = ReadText(in.password());
    if (CountCharacters(password) > MaxSharePasswordLength)
        fields.AddError("password", "too_long");
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    Optional<SystemTimePoint> expiresAt;
    if (expires != "never")
        expiresAt = SystemTimePoint::clock::now() + std::chrono::days(Rooftree::StringTo<int>(expires.substr(0, expires.size() - 1)).value_or(1));

    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;

    Optional<ShareItem> share = co_await session.OnWorker([userId = owner.Id, fileId, folderId, password, expiresAt]() -> Optional<ShareItem>
    {
        if (fileId ? !Drive::WorkerFile(userId, *fileId) : !Drive::WorkerFolder(userId, *folderId))
            return {};

        Optional<std::string> hash;
        if (!password.empty() && !(hash = HashPassword(password, SharePasswordHash)))
            return {};

        std::string token = Rooftree::Server::GenerateToken(24);
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_DRIVE_SHARE);
        stmt->setString(0, token);
        stmt->setUInt32(1, userId);
        SetFolder(stmt, 2, fileId);
        SetFolder(stmt, 3, folderId);
        if (hash)
            stmt->setString(4, *hash);
        else
            stmt->setNull(4);
        if (expiresAt)
            stmt->setDate(5, *expiresAt);
        else
            stmt->setNull(5);
        BlogDatabase.DirectExecute(stmt);

        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_SHARE_BY_TOKEN);
        stmt->setUInt32(0, userId);
        stmt->setString(1, token);
        PreparedQueryResult created = BlogDatabase.Query(stmt);
        if (!created)
            return {};

        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_SHARES);
        stmt->setUInt32(0, userId);
        stmt->setUInt32(1, created->Fetch()[0].GetUInt32());
        stmt->setUInt32(2, created->Fetch()[0].GetUInt32());
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return {};
        return ShareItem::FromFields(row->Fetch(), Drive::WorkerAllFolders(userId));
    });
    if (!share)
        co_return RespondNotFound(request);
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(share->Table(builder));
    request->Respond(builder, http::status::created);
}

// Who opened a link, by name: how often, how often they downloaded, first and last time
Rooftree::Task<void> ListDriveShareAccess(Session& session, ApiRequest request)
{
    Reader::DriveIdRequest const& in = request->Get<Reader::DriveIdRequest>();
    if (!in.id() || !*in.id())
        co_return RespondNotFound(request);

    struct Visitor
    {
        uint32 UserId;
        std::string DisplayName, Username;
        uint32 Views, Downloads;
        SystemTimePoint FirstAt, LastAt;
    };

    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), false);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = *in.id();

    std::vector<Visitor> access = co_await session.OnWorker([userId = owner.Id, id]
    {
        std::vector<Visitor> access;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_SHARE_ACCESS);
        stmt->setUInt32(0, id);
        stmt->setUInt32(1, userId);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                access.push_back({ fields[0].GetUInt32(), fields[1].GetString(), fields[2].GetString(), fields[3].GetUInt32(),
                    fields[4].GetUInt32(), fields[5].GetDate(), fields[6].GetDate() });
            }
            while (rows->NextRow());
        }
        return access;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::ShareVisitor>> items;
    for (Visitor const& visitor : access)
        items.push_back(Reader::CreateShareVisitor(builder, visitor.UserId, builder.CreateString(visitor.DisplayName),
            builder.CreateString(visitor.Username), visitor.Views, visitor.Downloads, TimeString(builder, visitor.FirstAt),
            TimeString(builder, visitor.LastAt)));
    builder.Finish(Reader::CreateDriveShareAccess(builder, builder.CreateVector(items)));
    request->Respond(builder);
}

// Revoked: the link is a 404 from now on
Rooftree::Task<void> DeleteDriveShare(Session& session, ApiRequest request)
{
    Reader::DriveIdRequest const& in = request->Get<Reader::DriveIdRequest>();
    if (!in.id() || !*in.id())
        co_return RespondNotFound(request);

    Optional<UserRow> drive = co_await ResolveDrive(session, request, in.user(), true);
    if (!drive)
        co_return;
    UserRow const& owner = *drive;
    auto id = *in.id();

    bool found = co_await session.OnWorker([userId = owner.Id, id]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_SHARES);
        stmt->setUInt32(0, userId);
        stmt->setUInt32(1, id);
        stmt->setUInt32(2, id);
        if (!BlogDatabase.Query(stmt))
            return false;

        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_DRIVE_SHARE);
        stmt->setUInt32(0, id);
        stmt->setUInt32(1, userId);
        BlogDatabase.DirectExecute(stmt);
        return true;
    });
    if (!found)
        co_return RespondNotFound(request);
    RespondOk(request);
}

// What a link shows: {share: {kind, name, owner, expiresAt, locked}}, and unless locked the file, or
// the folder asked for (the link's own by default) with the way there from the link's folder
Rooftree::Task<void> GetShare(Session& session, ApiRequest request)
{
    Reader::ShareGetRequest const& in = request->Get<Reader::ShareGetRequest>();
    std::string token = ReadText(in.token());
    Optional<uint32> folder = FolderOf(in.folder());
    if (!IsShareToken(token))
        co_return RespondNotFound(request);

    struct Result
    {
        Optional<ShareRow> Share;
        bool Found = true;
        bool Locked = false;
        std::string Name;
        Optional<FileRow> File;
        Optional<FolderRow> Folder;
        Drive::FolderContents Contents;
        std::vector<FolderRow> Path;
    };

    uint32 viewerId = request->GetAuth().User.Id;
    Result result = co_await session.OnWorker([viewerId, token, folder]
    {
        Result result;
        result.Share = WorkerFindShare(token, viewerId);
        if (!result.Share || result.Share->Expired)
            return result;

        ShareRow const& share = *result.Share;
        result.Locked = IsLocked(share, viewerId);
        if (!result.Locked)
        {
            if (share.FileId)
            {
                result.File = Drive::WorkerFile(share.OwnerId, *share.FileId);
                if (!result.File)
                {
                    result.Found = false;
                    return result;
                }
                result.Name = result.File->Name;
            }
            else
            {
                std::map<uint32, FolderRow> folders = Drive::WorkerAllFolders(share.OwnerId);
                Optional<uint32> shown = folder.value_or(*share.FolderId);
                result.Path = Drive::PathOf(folders, shown, *share.FolderId);
                if (result.Path.empty())
                {
                    result.Found = false;
                    return result;
                }
                result.Name = folders.at(*share.FolderId).Name;
                result.Contents = Drive::WorkerListFolder(share.OwnerId, shown);
                result.Folder = folders.at(*shown);
            }
        }

        // Opening the link counts once what it shows is shown, going into its folders doesn't
        if (!folder && share.OwnerId != viewerId && !result.Locked)
            WorkerCount(BLOG_INS_DRIVE_SHARE_VIEW, share.Id, viewerId);
        return result;
    });
    if (!CheckShare(request, result.Share))
        co_return;
    if (!result.Found)
        co_return RespondNotFound(request);

    ShareRow const& share = *result.Share;
    flatbuffers::FlatBufferBuilder builder;
    auto info = Reader::CreateShareInfo(builder, builder.CreateString(share.FileId ? "file" : "folder"), builder.CreateString(share.OwnerName),
        TimeString(builder, share.ExpiresAt), result.Locked, share.OwnerId == viewerId, result.Locked ? 0 : builder.CreateString(result.Name));
    flatbuffers::Offset<Reader::ShareView> table;
    if (result.File)
    {
        auto file = FileTable(builder, *result.File);
        table = Reader::CreateShareView(builder, info, file);
    }
    else if (result.Folder)
    {
        auto [folders, files] = ContentsTable(builder, result.Contents);
        auto shown = FolderTable(builder, *result.Folder);
        table = Reader::CreateShareView(builder, info, 0, folders, files, shown, PathTable(builder, result.Path));
    }
    else
        table = Reader::CreateShareView(builder, info);
    builder.Finish(table);
    request->Respond(builder);
}

// {password}: the link's password, once per account
Rooftree::Task<void> UnlockShare(Session& session, ApiRequest request)
{
    Reader::ShareUnlockRequest const& in = request->Get<Reader::ShareUnlockRequest>();
    std::string token = ReadText(in.token());
    if (!IsShareToken(token))
        co_return RespondNotFound(request);

    flatbuffers::String const* password = in.password();
    if (!password || !password->size() || password->size() > MaxSharePasswordLength * 4)
        co_return RespondValidation(request, { { "password", "required" } });

    // Wrong ones of this account for this link, the old ones forgotten
    uint32 viewerId = request->GetAuth().User.Id;
    std::string key = std::to_string(viewerId) + ":" + token;
    SystemTimePoint now = SystemTimePoint::clock::now();
    std::vector<SystemTimePoint>& failures = UnlockFailureTimes[key];
    std::erase_if(failures, [&](SystemTimePoint time) { return now - time >= UnlockFailureWindow; });
    if (failures.size() >= UnlockFailures)
    {
        Seconds wait = std::chrono::duration_cast<Seconds>(failures.front() + UnlockFailureWindow - now) + 1s;
        co_return request->RespondError({ .Status = uint16(http::status::too_many_requests), .Code = "too_many_attempts", .RetryAfter = uint32(wait.count()) });
    }

    struct Result
    {
        Optional<ShareRow> Share;
        bool Right = false;
    };

    Result result = co_await session.OnWorker([viewerId, token, password = password->str()]
    {
        Result result;
        result.Share = WorkerFindShare(token, viewerId);
        if (!result.Share || result.Share->Expired)
            return result;

        result.Right = result.Share->PasswordHash.empty() || Rooftree::Crypto::Argon2::Verify(password, result.Share->PasswordHash);
        if (result.Right && !result.Share->PasswordHash.empty())
            WorkerCount(BLOG_INS_DRIVE_SHARE_UNLOCK, result.Share->Id, viewerId);
        return result;
    });
    if (!CheckShare(request, result.Share))
        co_return;
    if (!result.Right)
    {
        UnlockFailureTimes[key].push_back(SystemTimePoint::clock::now());
        co_return request->RespondError(http::status::forbidden, "wrong_password");
    }
    UnlockFailureTimes.erase(key);
    RespondOk(request);
}

// A zip of a folder link: its folder, or ?folder= under it, with all under that. Counts as a download
Rooftree::Task<void> BlogSession::HandleShareZip(Request request)
{
    Optional<AuthInfo> login = co_await RequireUserAsync(request);
    if (!login)
        co_return;
    AuthInfo const* auth = &*login;

    std::string token(request->GetPathParameter("token"));
    Optional<uint32> folder;
    if (!IsShareToken(token) || !GetFolderParameter(request, folder))
        co_return RespondNotFound(request);

    struct Result
    {
        Optional<ShareRow> Share;
        bool Locked = false;
        Optional<Zip> File;
    };

    Result result = co_await OnWorker([viewerId = auth->User.Id, token, folder, directory = sWorld->GetSettings().DriveDirectory]
    {
        Result result;
        result.Share = WorkerFindShare(token, viewerId);
        if (!result.Share || result.Share->Expired)
            return result;

        ShareRow const& share = *result.Share;
        result.Locked = IsLocked(share, viewerId);
        if (result.Locked || !share.FolderId)
            return result;

        std::map<uint32, FolderRow> folders = Drive::WorkerAllFolders(share.OwnerId);
        uint32 shown = folder.value_or(*share.FolderId);
        if (!Drive::IsUnder(folders, shown, *share.FolderId))
            return result;
        result.File = MakeZip(Drive::WorkerZipEntries(share.OwnerId, folders, {}, { shown }, directory), folders.at(shown).Name);
        if (result.File && share.OwnerId != viewerId)
            WorkerCount(BLOG_INS_DRIVE_SHARE_DOWNLOAD, share.Id, viewerId);
        return result;
    });
    if (!CheckShare(request, result.Share))
        co_return;
    if (result.Locked)
        co_return request->RespondError(http::status::forbidden, "share_locked");
    if (!result.File)
        co_return RespondNotFound(request);
    RespondZip(request, *result.File);
}

// The small picture of a file of the link
Rooftree::Task<void> BlogSession::HandleShareThumb(Request request)
{
    Optional<AuthInfo> login = co_await RequireUserAsync(request);
    if (!login)
        co_return;
    AuthInfo const* auth = &*login;

    std::string token(request->GetPathParameter("token"));
    Optional<uint32> id = GetPathId(request);
    if (!IsShareToken(token) || !id)
        co_return RespondNotFound(request);

    struct Result
    {
        Optional<ShareRow> Share;
        bool Locked = false;
        std::string Type;
        std::string Bytes;
    };

    Result result = co_await OnWorker([viewerId = auth->User.Id, token, id = *id, directory = sWorld->GetSettings().DriveDirectory]
    {
        Result result;
        result.Share = WorkerFindShare(token, viewerId);
        if (!result.Share || result.Share->Expired)
            return result;
        ShareRow const& share = *result.Share;
        result.Locked = IsLocked(share, viewerId);
        if (result.Locked)
            return result;

        Optional<FileRow> file = Drive::WorkerFile(share.OwnerId, id);
        if (!file || (share.FileId ? *share.FileId != file->Id : !Drive::IsUnder(Drive::WorkerAllFolders(share.OwnerId), file->FolderId, *share.FolderId)))
            return result;
        result.Type = file->Thumb;
        result.Bytes = WorkerReadThumb(*file, directory);
        return result;
    });
    if (!CheckShare(request, result.Share))
        co_return;
    if (result.Locked)
        co_return request->RespondError(http::status::forbidden, "share_locked");
    RespondThumb(request, result.Type, std::move(result.Bytes));
}

// A file of the link: the linked file, or one under the linked folder. ?download=1 counts as a download
Rooftree::Task<void> BlogSession::HandleShareDownload(Request request)
{
    Optional<AuthInfo> login = co_await RequireUserAsync(request);
    if (!login)
        co_return;
    AuthInfo const* auth = &*login;

    std::string token(request->GetPathParameter("token"));
    Optional<uint32> id = GetPathId(request);
    if (!IsShareToken(token) || !id)
        co_return RespondNotFound(request);

    bool attachment = request->GetQueryParameter("download").value_or("") == "1";
    struct Result
    {
        Optional<ShareRow> Share;
        bool Locked = false;
        Optional<Download> File;
    };

    Result result = co_await OnWorker([viewerId = auth->User.Id, token, id = *id, attachment, range = std::string(request->GetHeader(http::field::range)),
        directory = sWorld->GetSettings().DriveDirectory]
    {
        Result result;
        result.Share = WorkerFindShare(token, viewerId);
        if (!result.Share || result.Share->Expired)
            return result;

        ShareRow const& share = *result.Share;
        result.Locked = IsLocked(share, viewerId);
        if (result.Locked)
            return result;

        Optional<FileRow> file = Drive::WorkerFile(share.OwnerId, id);
        if (!file || (share.FileId ? *share.FileId != file->Id : !Drive::IsUnder(Drive::WorkerAllFolders(share.OwnerId), file->FolderId, *share.FolderId)))
            return result;

        Download download{ .File = std::move(*file) };
        download.Range = Drive::ParseRange(range, download.File.Size);
        WorkerOpen(download, directory);
        if (attachment && share.OwnerId != viewerId && download.Range.First == 0 && download.Stream)
            WorkerCount(BLOG_INS_DRIVE_SHARE_DOWNLOAD, share.Id, viewerId);
        result.File = std::move(download);
        return result;
    });
    if (!CheckShare(request, result.Share))
        co_return;
    if (result.Locked)
        co_return request->RespondError(http::status::forbidden, "share_locked");
    if (!result.File)
        co_return RespondNotFound(request);
    RespondDownload(request, *result.File, attachment);
}

// Every drive, how full, and the disk they are on: {disk: {total, free, minFree}, quotas, used, accounts: [...]}.
// trashed: what of used is in the trash (files deleted themselves; not those in a deleted folder)
Rooftree::Task<void> AdminDrives(Session& session, ApiRequest request)
{
    BlogSettings const& settings = sWorld->GetSettings();
    std::string payload = co_await session.OnWorker([directory = settings.DriveDirectory, quota = settings.DriveQuota,
        adminQuota = settings.DriveAdminQuota, trashDays = settings.DriveTrashDays, minFree = settings.DriveMinFreeSpace]
    {
        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<Admin::DriveAccount>> accounts;
        uint64 quotas = 0;
        uint64 used = 0;
        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_ACCOUNTS)))
        {
            do
            {
                Field* fields = rows->Fetch();
                std::vector<std::string> granted = SplitList(fields[3].GetString());
                bool admin = fields[2].GetString() == RoleAdmin;
                bool enabled = admin || std::ranges::find(granted, GrantDrive) != granted.end();
                uint64 size = fields[4].GetUInt64OrNull().value_or(admin ? adminQuota : quota);
                if (enabled)
                    quotas += size;
                used += fields[5].GetUInt64();
                accounts.push_back(Admin::CreateDriveAccount(builder, fields[0].GetUInt32(), builder.CreateString(fields[1].GetString()),
                    builder.CreateString(fields[7].GetString()), builder.CreateString(fields[2].GetString()), enabled, size, fields[5].GetUInt64(),
                    uint64(fields[6].GetInt64()), fields[9].GetUInt64(), std::min(fields[8].GetUInt32OrNull().value_or(trashDays), Drive::MaxTrashDays)));
            }
            while (rows->NextRow());
        }

        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::filesystem::space_info space = std::filesystem::space(directory, error);
        auto disk = Admin::CreateDriveDisk(builder, error ? 0 : space.capacity, error ? 0 : space.available, minFree);
        builder.Finish(Admin::CreateDrives(builder, disk, quotas, used, builder.CreateVector(accounts)));
        return Bytes(builder);
    });
    request->Respond(std::move(payload));
}
}
