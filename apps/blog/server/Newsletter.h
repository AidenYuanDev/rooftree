/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Mailing new writing to the subscribers, on a thread of its own: a mailing may take long (one mail
 * after another, at Blog.Newsletter.MailsPerMinute so the SMTP server doesn't take us for spam), and
 * the workers stay free for requests.
 *
 * A post is mailed on its first publication, a note when the author asks for it. The thread marks it
 * mailed (mailed_at) before the first mail, so a post published, unpublished and published again is
 * mailed once; a mailing cut short by a restart isn't resumed.
 */

#ifndef BLOG_NEWSLETTER_H
#define BLOG_NEWSLETTER_H

#include "Define.h"
#include "Mailer.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace Blog
{
// Everything the thread needs, copied from the main thread when queued
struct NewsletterIssue
{
    std::string Kind;           // "post" or "note"
    uint32 Id = 0;
    std::string PublicOrigin;
    std::map<std::string, std::string> SiteNames;   // by language
    Rooftree::Server::MailSettings Mail;
    std::string Outbox;
    uint32 MailsPerMinute = 60;
};

class Newsletter
{
public:
    ~Newsletter();

    // Main thread
    void Queue(NewsletterIssue issue);

    // Shutdown: the mail being sent finishes, the rest is dropped
    void Stop();

private:
    void Run();
    void Send(NewsletterIssue const& issue);

    std::mutex _lock;
    std::condition_variable _wake;
    std::deque<NewsletterIssue> _queue;
    bool _stopping = false;
    std::thread _thread;
};

// Blocking: sends the mail, or writes it into outbox (development, tests) when that's set
void DeliverMail(Rooftree::Server::MailSettings const& settings, std::string const& outbox, Rooftree::Server::MailMessage const& message);
}

#endif // BLOG_NEWSLETTER_H
