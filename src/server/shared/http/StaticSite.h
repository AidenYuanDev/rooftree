/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Static files answered on the network thread, without a reverse proxy in front.
 *
 * A site (a directory of built pages, scripts and fonts) is read into memory once, so a request never
 * waits for the disk and never reaches the main thread: a route or the fallback handler of the
 * service answers it with RequestHandlerResult::Handled. A new build is loaded on another thread and
 * swapped in as a whole (StaticSiteHolder), requests see the old site or the new one, never a mix.
 *
 * Only the files that were loaded can be answered, a path is a key in a map, so no request can reach
 * outside the directory. name.gz next to name is its gzip encoding (made at build time, nothing is
 * compressed while serving), answered to clients that accept gzip.
 */

#ifndef ROOFTREE_STATIC_SITE_H
#define ROOFTREE_STATIC_SITE_H

#include "Define.h"
#include "HttpCommon.h"
#include "Optional.h"
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Rooftree::Server
{
struct StaticFile
{
    std::string Body;
    std::string GzipBody;           // empty: no .gz of it
    std::string_view ContentType;
    std::string ETag;               // quoted, of the content
};

class StaticSite
{
public:
    // Every regular file under directory (itself possibly a symlink to a release), keyed by its URL
    // path: "/zh/index.html". Null, with error set, if the directory can't be read
    static std::shared_ptr<StaticSite const> Load(std::filesystem::path const& directory, std::string& error);

    // path as GetRequestPath() returns it
    StaticFile const* Find(std::string_view path) const;

    std::size_t GetFileCount() const { return _files.size(); }
    std::size_t GetSize() const { return _size; }

private:
    std::unordered_map<std::string, StaticFile> _files;
    std::size_t _size = 0;
};

// The site being served, any thread
class StaticSiteHolder
{
public:
    std::shared_ptr<StaticSite const> Get() const;
    void Set(std::shared_ptr<StaticSite const> site);

private:
    mutable std::mutex _mutex;
    std::shared_ptr<StaticSite const> _site;
};

// The target's path percent-decoded, without the query: "/zh/posts/a b/". Empty if the target is
// malformed, the same targets routes refuse (DecodePathSegments)
Optional<std::string> GetRequestPath(Net::Http::Request const& request);

// By the extension, application/octet-stream when unknown
std::string_view GetContentType(std::string_view path);

// Sets the response to file: its type, ETag and the gzip body when the client accepts it, or 304
// when the client has it already (If-None-Match)
void SetStaticResponse(Net::Http::RequestContext& context, StaticFile const& file, std::string_view cacheControl,
    boost::beast::http::status status = boost::beast::http::status::ok);

// A file read from the disk, for what is not part of a site (uploads). Blocks the network thread while
// it reads, keep such files small. Empty if it can't be read
Optional<std::string> ReadWholeFile(std::filesystem::path const& path);
}

#endif // ROOFTREE_STATIC_SITE_H
