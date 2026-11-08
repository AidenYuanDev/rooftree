/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Accounts' drives: folders and files of one account each, that nobody else lists or reads but the
 * admin (?user=); others only see what a share link shows them. The admin and the accounts the admin
 * gave "drive" (user.granted) have one, of Blog.Drive.Quota bytes or the account's own drive_quota.
 *
 * What is deleted goes to the trash first, with what is under it, for Blog.Drive.TrashDays (or the
 * account's drive_trash_days): hidden from the drive and its links, still counted against the quota,
 * and back where it was when restored. Then it is removed for good, at the latest an hour after.
 *
 * Files are on the disk under Blog.Drive.Directory, by random names:
 *
 *   files/<stored name>   a file, its name and folder in drive_file
 *   thumbs/<stored name>  its small picture, when the uploader's browser made one (drive_file.thumb)
 *   parts/<upload id>     an upload on its way
 *
 * Uploads come in pieces of ChunkSize, each an ordinary request under Server.Http.MaxRequestBodySize: the
 * server appends a piece to the part file on a worker, and moves the part into files/ when it is
 * complete. No request holds more than a piece in memory, a dropped connection loses one piece
 * (the page asks again from where the server got to), and unfinished uploads count against the quota
 * until they are a day old. Downloads are streamed from the file (WorldHttpRequest::RespondStream),
 * with Range for seeking in a video.
 *
 * Worker* functions block on the database and the disk, for OnWorker.
 */

#ifndef BLOG_DRIVE_H
#define BLOG_DRIVE_H

#include "BlogTypes.h"
#include "Duration.h"
#include "ZipStream.h"
#include <map>

namespace Blog
{
struct BlogSettings;

namespace Drive
{
inline constexpr uint64 ChunkSize = 8 * 1024 * 1024;
inline constexpr std::size_t MaxNameLength = 255;
inline constexpr uint32 MaxTrashDays = 365;
// Files and folders in one zip at most
inline constexpr std::size_t MaxZipEntries = 20000;
// A small picture's bytes at most
inline constexpr std::size_t MaxThumbSize = 128 * 1024;

struct FolderRow
{
    uint32 Id = 0;
    Optional<uint32> ParentId;      // none: in the root
    std::string Name;
    SystemTimePoint CreatedAt;
    Optional<SystemTimePoint> TrashedAt;    // itself, not one above it

    // BLOG_DRIVE_FOLDER_COLUMNS
    static FolderRow FromFields(Field const* fields);
};

struct FileRow
{
    uint32 Id = 0;
    Optional<uint32> FolderId;
    std::string Name;
    uint64 Size = 0;
    std::string Mime;
    SystemTimePoint CreatedAt;
    SystemTimePoint UpdatedAt;
    std::string StoredName;
    Optional<SystemTimePoint> TrashedAt;
    std::string Thumb;      // webp, jpeg, png; empty: none

    // BLOG_DRIVE_FILE_COLUMNS
    static FileRow FromFields(Field const* fields);
};

struct FolderContents
{
    std::vector<FolderRow> Folders;
    std::vector<FileRow> Files;
};

// Something in the trash, with what is under it
struct TrashItem
{
    std::string Kind;       // file, folder
    uint32 Id = 0;
    std::string Name;
    uint64 Size = 0;
    Optional<std::string> Mime;     // a file's
    SystemTimePoint TrashedAt;
    SystemTimePoint PurgeAt;
    std::vector<std::string> From;  // the names of the folders it was in
};

// A name for a file or folder: 1 to 255 characters, no control characters, no / or \, not . or ..
bool IsValidName(std::string_view name);
// The type by the name's extension, application/octet-stream when we don't know it
std::string MimeOf(std::string_view name);
// What a browser may show itself, on our origin: pictures (not SVG), audio, video, PDF. Anything else
// is only downloaded, so an uploaded page never runs as the site
bool IsInlineType(std::string_view mime);
// attachment or inline, with the UTF-8 name (RFC 6266) and an ASCII one for old clients
std::string ContentDisposition(bool attachment, std::string_view name);
// "photo (2).jpg" for the n-th "photo.jpg"
std::string NumberedName(std::string_view name, uint32 n);

// A single Range of bytes=, against a file of size bytes; Whole without one (or for several, which we
// don't do), Unsatisfiable past its end
struct ByteRange
{
    enum Kind { Whole, Partial, Unsatisfiable } Type = Whole;
    uint64 First = 0;
    uint64 Last = 0;    // inclusive
};
ByteRange ParseRange(std::string_view header, uint64 size);

// Bytes an account may keep: its own quota, else the setting for its role
uint64 QuotaOf(UserRow const& user, BlogSettings const& settings);
// Days its trash keeps what it deleted, 0: deleted at once
uint32 TrashDaysOf(UserRow const& user, BlogSettings const& settings);

std::string FilePath(std::string const& directory, std::string_view storedName);
std::string PartPath(std::string const& directory, std::string_view uploadId);
std::string ThumbPath(std::string const& directory, std::string_view storedName);
// The type of a small picture by its first bytes (WebP, JPEG, PNG), empty for anything else
std::string_view ThumbType(std::string_view bytes);

// Worker: bytes in its files, unfinished uploads and the images editors put in its posts, and how many files
std::pair<uint64, uint64> WorkerUsage(uint32 userId);
// Worker: the account's folder, none when it isn't one of its or in the trash
Optional<FolderRow> WorkerFolder(uint32 userId, uint32 folderId);
// Worker: every folder of the account by id, but those in the trash and under them unless withTrash
std::map<uint32, FolderRow> WorkerAllFolders(uint32 userId, bool withTrash = false);
// Worker: the account's file, none when it isn't one of its or in the trash (or a folder of it)
Optional<FileRow> WorkerFile(uint32 userId, uint32 fileId, bool withTrash = false);
// Worker: the folders and files in a folder (none: the root)
FolderContents WorkerListFolder(uint32 userId, Optional<uint32> folderId);
// Worker: a folder of that name or a file of that name in the folder
bool WorkerNameTaken(uint32 userId, Optional<uint32> folderId, std::string const& name);
// Worker: the folders from the root down to folderId (none: the root, empty), stopping after stopAt
// when it is met on the way up (a link's folder: its path starts there). Empty too when stopAt is
// given and not met
std::vector<FolderRow> PathOf(std::map<uint32, FolderRow> const& folders, Optional<uint32> folderId, Optional<uint32> stopAt = {});
// folderId is root or under it
bool IsUnder(std::map<uint32, FolderRow> const& folders, Optional<uint32> folderId, uint32 root);
// Worker: what a zip of the account's files holds: each folder of folderIds with all under it, each
// file of fileIds, by their names; the whole drive when both are empty. folders: the account's, but
// those in the trash. None when one isn't the account's (or is in the trash), or there are more than
// MaxZipEntries
Optional<std::vector<ZipEntry>> WorkerZipEntries(uint32 userId, std::map<uint32, FolderRow> const& folders,
    std::vector<uint32> const& fileIds, std::vector<uint32> const& folderIds, std::string const& directory);
// Worker: removes a folder with all under it, rows and files
void WorkerDeleteFolder(uint32 userId, uint32 folderId, std::string const& directory);
// Worker: removes a file, row and file
void WorkerDeleteFile(uint32 userId, FileRow const& file, std::string const& directory);
// Worker: what the account's trash holds (files and folders deleted, each with what is under it),
// newest first
std::vector<TrashItem> WorkerListTrash(uint32 userId, uint32 days);
// Worker: removes what has been in the trash for its account's days, of one account or every one;
// everything in its trash with all
void WorkerPurgeTrash(std::string const& directory, uint32 defaultDays, Optional<uint32> userId = {}, bool all = false);
// Worker: removes the account's whole drive (it was deleted)
void WorkerDeleteAccountDrive(uint32 userId, std::string const& directory);
// Worker: removes the uploads unfinished for a day, and part files without an upload
void WorkerRemoveStaleUploads(std::string const& directory);
}
}

#endif // BLOG_DRIVE_H
