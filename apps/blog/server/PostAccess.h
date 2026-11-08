/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * What a user may do to a post (PostRights of BlogTypes.h): the admin and its owner all of it, an
 * editor (post_editor) its text and the rights it was given, anyone else nothing. The post operations
 * (PostHandlers.cpp) and its editors' documents (Collab.h) ask the same.
 */

#ifndef BLOG_POST_ACCESS_H
#define BLOG_POST_ACCESS_H

#include "BlogTypes.h"
#include <algorithm>
#include <string>
#include <vector>

namespace Blog
{
// A post's text in one language: its Markdown at most, in characters
inline constexpr std::size_t MaxBodyLength = 200000;
inline constexpr std::size_t MaxTitleLength = 200;
inline constexpr std::size_t MaxSummaryLength = 500;

struct PostAccess
{
    bool Admin = false;
    bool Owner = false;
    bool Editor = false;
    std::vector<std::string> Rights;    // an editor's
    uint32 OwnerId = 0;

    bool CanEdit() const { return Admin || Owner || Editor; }
    bool Can(std::string_view right) const { return Admin || Owner || (Editor && std::ranges::find(Rights, right) != Rights.end()); }
    bool CanDelete() const { return Admin || Owner; }

    // What the post's pages show it may do on top of editing
    std::vector<std::string> List() const
    {
        std::vector<std::string> list;
        for (std::string_view right : PostRights)
            if (Can(right))
                list.emplace_back(right);
        if (CanDelete())
            list.emplace_back("delete");
        return list;
    }
};

// Worker: none when the post doesn't exist
Optional<PostAccess> WorkerPostAccess(uint32 postId, UserRow const& user);
}

#endif // BLOG_POST_ACCESS_H
