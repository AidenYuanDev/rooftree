/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A zip of files on the disk, made while it is read: stored (not compressed, a drive's pictures and
 * videos are already), each file's CRC-32 counted as it goes out and written after it (a data
 * descriptor), so nothing is read twice and only one file is open at a time. Its length is known
 * before the first byte, for Content-Length. ZIP64 where a file, an offset or the count needs it.
 *
 * The stream reads the files when it is read, blocking: on the network thread, as
 * WorldHttpRequest::RespondStream does. A file that is shorter than said, or gone, ends the stream
 * early (the connection is closed, the download fails rather than holds wrong bytes).
 */

#ifndef BLOG_ZIP_STREAM_H
#define BLOG_ZIP_STREAM_H

#include "Define.h"
#include "Duration.h"
#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace Blog
{
struct ZipEntry
{
    std::string Name;           // in the zip, with / between folders; a folder ends with /
    std::string Path;           // on the disk, empty for a folder
    uint64 Size = 0;
    SystemTimePoint Modified;
};

// The zip of entries, and its length in bytes
std::pair<std::shared_ptr<std::istream>, uint64> MakeZipStream(std::vector<ZipEntry> entries);
}

#endif // BLOG_ZIP_STREAM_H
