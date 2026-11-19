/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * What other parts of the blog need of the chat (ChatHandlers.cpp): where its files are kept, and
 * removing them with an account.
 *
 * A file sent in a chat is kept as Blog.Uploads.Directory/chat/<random name>. /uploads/ only answers
 * names directly in that directory, so these are read through GET /api/chat/files/{message id}, by
 * those who may read the chat, nobody else.
 */

#ifndef BLOG_CHAT_H
#define BLOG_CHAT_H

#include "Define.h"
#include <string>

namespace Blog
{
std::string ChatFileDirectory(std::string const& uploadDirectory);

// Worker: the files userId sent, from the disk, before its account (and so its messages) is purged
void WorkerDeleteUserChatFiles(uint32 userId, std::string const& uploadDirectory);
}

#endif // BLOG_CHAT_H
