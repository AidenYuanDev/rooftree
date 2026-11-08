/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Accounts' pictures. Each is a file of Blog.Uploads.Directory named avatar-<token>.<extension>,
 * served at /uploads/<name> like any upload; user.avatar holds the name. A new picture gets a new
 * name (the files are cached for good) and the old file is removed.
 *
 * Uploaded ones come from the account page, which crops and encodes them in the browser; the server
 * only takes JPEG, PNG and WebP of at most MaxAvatarSize. A provider's one is downloaded by the
 * server from the address the provider gave at the last login or link, so readers never load
 * pictures from GitHub or Google.
 */

#ifndef BLOG_AVATAR_H
#define BLOG_AVATAR_H

#include "Define.h"
#include "Optional.h"
#include <string>
#include <string_view>

namespace Blog
{
inline constexpr std::size_t MaxAvatarSize = 512 * 1024;

// "png", "jpg" or "webp" by the content, empty for anything else (GIFs included: no animations)
std::string_view AvatarExtension(std::string_view data);

// Worker: data stored as userId's picture, its previous file removed. The new name, or empty when the
// file could not be written
std::string WorkerSaveAvatar(uint32 userId, std::string_view data, std::string const& directory);

// Worker: userId's picture removed, back to its initials
void WorkerRemoveAvatar(uint32 userId, std::string const& directory);

// Worker: the file of a picture name removed, when there is one
void RemoveAvatarFile(std::string const& directory, std::string_view name);

// Worker: the picture at a provider's address (asked for at 256 pixels). Empty with error set when it
// didn't come or isn't a picture we take
Optional<std::string> WorkerFetchProviderAvatar(std::string_view provider, std::string const& url, std::string& error);
}

#endif // BLOG_AVATAR_H
