/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The site around the writing: its profile (the home page's introduction, the about pages, the
 * links), the numbers on the home page, the timeline of everything published, friends' links and
 * projects, and the image placeholders the prerender build puts into posts.
 *
 * The profile is one JSON document (site_setting "profile"), kept in BlogWorld: every page of the
 * build reads it, and the feeds and mails take the site's name from it. Texts in it are per language
 * ({ "zh": ..., "en": ... }); a reader gets their language, or the other one when it's empty.
 *
 * Friends' links: a logged in reader with a confirmed address applies (pending); the author approves
 * or rejects, and the applicant gets a mail either way. The author may also add links directly.
 */

#include "ApiCall.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Validation.h"
#include <boost/json/array.hpp>
#include <boost/json/serialize.hpp>
#include <chrono>
#include <sstream>

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
constexpr uint32 TimelineItems = 2000;
constexpr std::size_t MaxSocials = 16;
constexpr uint32 MaxPendingFriendsPerUser = 3;

constexpr std::string_view SocialKinds[] = { "github", "x", "email", "rss", "bilibili", "telegram", "music", "mastodon", "bluesky",
    "zhihu", "weibo", "steam", "discord", "youtube" };

// A link the site shows: our own pages, https, or mail
bool IsValidLink(std::string_view url)
{
    return url.starts_with("https://") || url.starts_with("http://") || url.starts_with("mailto:") || (url.starts_with("/") && !url.starts_with("//"));
}

bool IsValidImage(std::string_view url)
{
    return url.empty() || url.starts_with("/uploads/") || url.starts_with("https://");
}

bool IsValidDate(std::string_view date)
{
    std::istringstream in{ std::string(date) };
    std::chrono::year_month_day day;
    in >> std::chrono::parse("%Y-%m-%d", day);
    return !in.fail() && day.ok() && date.size() == 10;
}

// { "zh": ..., "en": ... } of up to maxLength characters each, both optional
boost::json::object ReadLocalized(RequestFields& fields, std::string_view key, std::size_t maxLength, bool multiLine)
{
    boost::json::object localized{ { "zh", "" }, { "en", "" } };
    boost::json::object const* object = fields.Object(key);
    if (!object)
        return localized;

    RequestFields texts(*object);
    texts.SetPrefix(std::string(key));
    for (std::string_view lang : { "zh", "en" })
        localized[lang] = texts.Text(lang, 0, maxLength, false, multiLine);
    for (auto const& [errorKey, code] : texts.TakeErrors())
        fields.AddError(errorKey, code.as_string());
    return localized;
}

// The profile as the admin saves it, normalized: every field there, unknown ones dropped
Optional<boost::json::object> ReadProfile(RequestFields& fields, boost::json::object const& body)
{
    boost::json::object profile;
    profile["name"] = ReadLocalized(fields, "name", 60, false);
    profile["author"] = fields.Text("author", 0, 40, false);
    std::string avatar = fields.Text("avatar", 0, 255, false);
    if (!IsValidImage(avatar))
        fields.AddError("avatar", "invalid");
    profile["avatar"] = avatar;
    profile["tagline"] = ReadLocalized(fields, "tagline", 120, false);
    profile["bio"] = ReadLocalized(fields, "bio", 500, true);
    profile["quote"] = ReadLocalized(fields, "quote", 200, false);
    std::string since = fields.Text("since", 0, 10, false);
    if (!since.empty() && !IsValidDate(since))
        fields.AddError("since", "invalid");
    profile["since"] = since;
    profile["about"] = ReadLocalized(fields, "about", 50000, true);
    profile["aboutSite"] = ReadLocalized(fields, "aboutSite", 50000, true);
    boost::json::value const* review = body.if_contains("reviewWriters");
    profile["reviewWriters"] = review && review->is_bool() && review->as_bool();

    boost::json::array socials;
    if (boost::json::value const* list = body.if_contains("socials"))
    {
        if (!list->is_array() || list->as_array().size() > MaxSocials)
            fields.AddError("socials", "invalid");
        else
        {
            for (std::size_t i = 0; i < list->as_array().size(); ++i)
            {
                boost::json::value const& item = list->as_array()[i];
                if (!item.is_object())
                {
                    fields.AddError("socials", "invalid");
                    continue;
                }

                RequestFields social(item.as_object());
                social.SetPrefix(Rooftree::StringFormat("socials.{}", i));
                std::string kind = social.Choice("kind", std::vector<std::string_view>(std::begin(SocialKinds), std::end(SocialKinds)));
                std::string url = social.Text("url", 1, 255);
                if (!url.empty() && !IsValidLink(url))
                    social.AddError("url", "invalid");
                for (auto const& [errorKey, code] : social.TakeErrors())
                    fields.AddError(errorKey, code.as_string());
                socials.push_back({ { "kind", kind }, { "url", url } });
            }
        }
    }
    profile["socials"] = std::move(socials);

    if (!fields.IsValid())
        return {};
    return profile;
}

// ReadProfile of an admin call's table
Optional<boost::json::object> ReadProfile(TableFields& fields, Admin::SiteProfile const& in)
{
    auto localized = [&](std::string_view key, Admin::Localized const* texts, std::size_t maxLength, bool multiLine)
    {
        boost::json::object localized{ { "zh", "" }, { "en", "" } };
        if (!texts)
            return localized;

        fields.SetPrefix(std::string(key));
        localized["zh"] = fields.Text("zh", texts->zh(), 0, maxLength, false, multiLine);
        localized["en"] = fields.Text("en", texts->en(), 0, maxLength, false, multiLine);
        fields.SetPrefix({});
        return localized;
    };

    boost::json::object profile;
    profile["name"] = localized("name", in.name(), 60, false);
    profile["author"] = fields.Text("author", in.author(), 0, 40, false);
    std::string avatar = fields.Text("avatar", in.avatar(), 0, 255, false);
    if (!IsValidImage(avatar))
        fields.AddError("avatar", "invalid");
    profile["avatar"] = avatar;
    profile["tagline"] = localized("tagline", in.tagline(), 120, false);
    profile["bio"] = localized("bio", in.bio(), 500, true);
    profile["quote"] = localized("quote", in.quote(), 200, false);
    std::string since = fields.Text("since", in.since(), 0, 10, false);
    if (!since.empty() && !IsValidDate(since))
        fields.AddError("since", "invalid");
    profile["since"] = since;
    profile["about"] = localized("about", in.about(), 50000, true);
    profile["aboutSite"] = localized("aboutSite", in.aboutSite(), 50000, true);
    profile["reviewWriters"] = in.reviewWriters();

    boost::json::array socials;
    if (in.socials() && in.socials()->size() > MaxSocials)
        fields.AddError("socials", "invalid");
    else if (in.socials())
    {
        for (flatbuffers::uoffset_t i = 0; i < in.socials()->size(); ++i)
        {
            Admin::Social const* item = in.socials()->Get(i);
            fields.SetPrefix(Rooftree::StringFormat("socials.{}", i));
            std::string kind = fields.Choice("kind", item->kind(), std::vector<std::string_view>(std::begin(SocialKinds), std::end(SocialKinds)));
            std::string url = fields.Text("url", item->url(), 1, 255);
            if (!url.empty() && !IsValidLink(url))
                fields.AddError("url", "invalid");
            fields.SetPrefix({});
            socials.push_back({ { "kind", kind }, { "url", url } });
        }
    }
    profile["socials"] = std::move(socials);

    if (!fields.IsValid())
        return {};
    return profile;
}

// A profile as saved (JSON, maybe empty) as the admin's table, every field there
void RespondProfile(ApiRequest const& request, boost::json::object const& profile)
{
    auto text = [&](boost::json::object const& object, std::string_view key) -> std::string_view
    {
        boost::json::value const* value = object.if_contains(key);
        return value && value->is_string() ? std::string_view(value->get_string()) : std::string_view();
    };

    flatbuffers::FlatBufferBuilder builder;
    auto localized = [&](std::string_view key)
    {
        boost::json::value const* value = profile.if_contains(key);
        boost::json::object const empty;
        boost::json::object const& texts = value && value->is_object() ? value->get_object() : empty;
        return Admin::CreateLocalized(builder, builder.CreateString(text(texts, "zh")), builder.CreateString(text(texts, "en")));
    };

    auto name = localized("name");
    auto tagline = localized("tagline");
    auto bio = localized("bio");
    auto quote = localized("quote");
    auto about = localized("about");
    auto aboutSite = localized("aboutSite");
    std::vector<flatbuffers::Offset<Admin::Social>> socials;
    if (boost::json::value const* list = profile.if_contains("socials"); list && list->is_array())
        for (boost::json::value const& item : list->get_array())
            if (item.is_object())
                socials.push_back(Admin::CreateSocial(builder, builder.CreateString(text(item.get_object(), "kind")),
                    builder.CreateString(text(item.get_object(), "url"))));
    builder.Finish(Admin::CreateSiteProfile(builder, name, builder.CreateString(text(profile, "author")), builder.CreateString(text(profile, "avatar")),
        tagline, bio, quote, builder.CreateString(text(profile, "since")), about, aboutSite, builder.CreateVector(socials),
        profile.contains("reviewWriters") && profile.at("reviewWriters").is_bool() && profile.at("reviewWriters").as_bool()));
    request->Respond(builder);
}

// A text of the profile in lang, or the other language when that one is empty
std::string Localized(boost::json::object const& profile, std::string_view key, std::string_view lang)
{
    boost::json::object const* texts = profile.contains(key) && profile.at(key).is_object() ? &profile.at(key).as_object() : nullptr;
    if (!texts)
        return {};

    for (std::string_view candidate : { lang, lang == "en" ? std::string_view("zh") : std::string_view("en") })
        if (boost::json::value const* text = texts->if_contains(candidate); text && text->is_string() && !text->as_string().empty())
            return std::string(text->as_string());
    return {};
}

std::string GetString(boost::json::object const& object, std::string_view key)
{
    boost::json::value const* value = object.if_contains(key);
    return value && value->is_string() ? std::string(value->as_string()) : std::string();
}

// Whole days from date (YYYY-MM-DD) to today, counting today: a site started today is 1 day old
int64 DaysSince(std::string const& date)
{
    std::istringstream in(date);
    std::chrono::sys_days day;
    in >> std::chrono::parse("%Y-%m-%d", day);
    if (in.fail())
        return 0;
    auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    return std::max<int64>(0, (today - day).count() + 1);
}

std::string CreatedDate(std::string const& since)
{
    return since.empty() ? std::string() : since + "T00:00:00Z";
}

struct FriendRow
{
    uint32 Id = 0;
    std::string Name;
    std::string Url;
    std::string Avatar;
    std::string Description;
    std::string Status;
    std::string Message;
    std::string RejectReason;
    int32 Sort = 0;
    SystemTimePoint CreatedAt;
    Optional<SystemTimePoint> ReviewedAt;
    Optional<uint32> ApplicantId;
    std::string ApplicantUsername;
    std::string ApplicantName;
    std::string ApplicantEmail;
    std::string ApplicantLocale;

    // BLOG_FRIEND_COLUMNS
    static FriendRow FromFields(Field const* fields)
    {
        FriendRow row;
        row.Id = fields[0].GetUInt32();
        row.Name = fields[1].GetString();
        row.Url = fields[2].GetString();
        row.Avatar = fields[3].GetString();
        row.Description = fields[4].GetString();
        row.Status = fields[5].GetString();
        row.Message = fields[6].GetString();
        row.RejectReason = fields[7].GetString();
        row.Sort = fields[8].GetInt32();
        row.CreatedAt = fields[9].GetDate();
        row.ReviewedAt = fields[10].GetDateOrNull();
        row.ApplicantId = fields[11].GetUInt32OrNull();
        row.ApplicantUsername = fields[12].GetStringOrNull().value_or("");
        row.ApplicantName = fields[13].GetStringOrNull().value_or("");
        row.ApplicantEmail = fields[14].GetStringOrNull().value_or("");
        row.ApplicantLocale = fields[15].GetStringOrNull().value_or("zh");
        return row;
    }

    flatbuffers::Offset<Admin::Friend> AdminTable(flatbuffers::FlatBufferBuilder& builder) const
    {
        auto applicant = ApplicantId
            ? Admin::CreateFriendApplicantDirect(builder, *ApplicantId, ApplicantUsername.c_str(), ApplicantName.c_str()) : 0;
        return Admin::CreateFriend(builder, Id, builder.CreateString(Name), builder.CreateString(Url), builder.CreateString(Avatar),
            builder.CreateString(Description), builder.CreateString(Status), builder.CreateString(Message), builder.CreateString(RejectReason), Sort,
            TimeString(builder, CreatedAt), TimeString(builder, ReviewedAt), applicant);
    }
};

Optional<FriendRow> WorkerLoadFriend(uint32 id)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_FRIEND);
    stmt->setUInt32(0, id);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};
    return FriendRow::FromFields(result->Fetch());
}

Optional<uint32> WorkerFindFriend(std::string const& url)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_FRIEND_BY_URL);
    stmt->setString(0, url);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};
    return result->Fetch()[0].GetUInt32();
}

struct FriendInput
{
    std::string Name;
    std::string Url;
    std::string Avatar;
    std::string Description;
    std::string Message;
    int32 Sort = 0;
};

// A site's link: http(s) only, no credentials in it
bool IsValidSiteUrl(std::string_view url)
{
    return (url.starts_with("https://") || url.starts_with("http://")) && url.find('@') == std::string_view::npos
        && url.find(' ') == std::string_view::npos && url.size() > 10;
}

// A reader's application: a word to the author, no place
Optional<FriendInput> ReadFriendInput(TableFields& fields, Reader::FriendApplication const& in)
{
    FriendInput input;
    input.Name = fields.Text("name", in.name(), 1, 50);
    input.Url = fields.Text("url", in.url(), 1, 255);
    if (!input.Url.empty() && !IsValidSiteUrl(input.Url))
        fields.AddError("url", "invalid");
    input.Avatar = fields.Text("avatar", in.avatar(), 0, 255, false);
    if (!input.Avatar.empty() && !input.Avatar.starts_with("https://") && !input.Avatar.starts_with("/uploads/"))
        fields.AddError("avatar", "invalid");
    input.Description = fields.Text("description", in.description(), 0, 200, false);
    input.Message = fields.Text("message", in.message(), 0, 500, false, true);

    if (!fields.IsValid())
        return {};
    return input;
}

// An admin's link (the author's own, or an edit of one): sort too, no message
Optional<FriendInput> ReadFriendInput(TableFields& fields, Admin::FriendInput const& in)
{
    FriendInput input;
    input.Name = fields.Text("name", in.name(), 1, 50);
    input.Url = fields.Text("url", in.url(), 1, 255);
    if (!input.Url.empty() && !IsValidSiteUrl(input.Url))
        fields.AddError("url", "invalid");
    input.Avatar = fields.Text("avatar", in.avatar(), 0, 255, false);
    if (!input.Avatar.empty() && !input.Avatar.starts_with("https://") && !input.Avatar.starts_with("/uploads/"))
        fields.AddError("avatar", "invalid");
    input.Description = fields.Text("description", in.description(), 0, 200, false);
    if (in.sort())
        input.Sort = std::clamp<int32>(*in.sort(), -100000, 100000);

    if (!fields.IsValid())
        return {};
    return input;
}

void RespondFriend(ApiRequest const& request, FriendRow const& link, http::status status = http::status::ok)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(link.AdminTable(builder));
    request->Respond(builder, status);
}

Rooftree::Server::MailMessage FriendReviewMail(FriendRow const& link, bool approved)
{
    std::string site = sWorld->GetSiteName(link.ApplicantLocale);
    std::string page = Rooftree::StringFormat("{}/{}/friends", sWorld->GetSettings().PublicOrigin, link.ApplicantLocale);
    if (link.ApplicantLocale == "en")
    {
        if (approved)
            return { .To = link.ApplicantEmail, .Subject = "Your link is on " + site,
                .TextBody = Rooftree::StringFormat("Hi {},\n\n{} ({}) is now among the friends of {}:\n\n{}\n\nThanks for linking!\n",
                    link.ApplicantName, link.Name, link.Url, site, page) };
        return { .To = link.ApplicantEmail, .Subject = "About your link on " + site,
            .TextBody = Rooftree::StringFormat("Hi {},\n\nsorry, {} ({}) won't be listed among the friends of {}.{}\n", link.ApplicantName, link.Name,
                link.Url, site, link.RejectReason.empty() ? "" : "\n\nThe reason: " + link.RejectReason) };
    }

    if (approved)
        return { .To = link.ApplicantEmail, .Subject = "你的友链已经加到" + site,
            .TextBody = Rooftree::StringFormat("{}，你好：\n\n{}（{}）已经出现在{}的友链里了：\n\n{}\n\n谢谢互链！\n", link.ApplicantName, link.Name, link.Url, site, page) };
    return { .To = link.ApplicantEmail, .Subject = "关于你在" + site + "的友链申请",
        .TextBody = Rooftree::StringFormat("{}，你好：\n\n抱歉，{}（{}）这次没有加入{}的友链。{}\n", link.ApplicantName, link.Name, link.Url, site,
            link.RejectReason.empty() ? "" : "\n\n原因：" + link.RejectReason) };
}

struct ProjectInput
{
    std::string Name;
    std::string Url;
    std::string RepoUrl;
    std::string Icon;
    std::string DescriptionZh;
    std::string DescriptionEn;
    int32 Sort = 0;
};

Optional<ProjectInput> ReadProjectInput(TableFields& fields, Admin::ProjectInput const& in)
{
    ProjectInput input;
    input.Name = fields.Text("name", in.name(), 1, 80);
    input.Url = fields.Text("url", in.url(), 0, 255, false);
    input.RepoUrl = fields.Text("repoUrl", in.repoUrl(), 0, 255, false);
    for (auto const& [key, value] : { std::pair<std::string_view, std::string const*>("url", &input.Url), std::pair<std::string_view, std::string const*>("repoUrl", &input.RepoUrl) })
        if (!value->empty() && !IsValidSiteUrl(*value))
            fields.AddError(key, "invalid");
    input.Icon = fields.Text("icon", in.icon(), 0, 255, false);
    if (!IsValidImage(input.Icon))
        fields.AddError("icon", "invalid");
    input.DescriptionZh = fields.Text("descriptionZh", in.descriptionZh(), 0, 300, false, true);
    input.DescriptionEn = fields.Text("descriptionEn", in.descriptionEn(), 0, 300, false, true);
    if (in.sort())
        input.Sort = std::clamp<int32>(*in.sort(), -100000, 100000);

    if (!fields.IsValid())
        return {};
    return input;
}

// A row of BLOG_SEL_PROJECT or BLOG_SEL_PROJECTS
struct ProjectRow
{
    uint32 Id;
    std::string Name, Url, RepoUrl, Icon, DescriptionZh, DescriptionEn;
    int32 Sort;

    static ProjectRow FromFields(Field const* fields)
    {
        return { .Id = fields[0].GetUInt32(), .Name = fields[1].GetString(), .Url = fields[2].GetString(), .RepoUrl = fields[3].GetString(),
            .Icon = fields[4].GetString(), .DescriptionZh = fields[5].GetString(), .DescriptionEn = fields[6].GetString(), .Sort = fields[7].GetInt32() };
    }
};

void RespondProject(ApiRequest const& request, ProjectRow const& project, http::status status = http::status::ok)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Admin::CreateProjectDirect(builder, project.Id, project.Name.c_str(), project.Url.c_str(), project.RepoUrl.c_str(),
        project.Icon.c_str(), project.DescriptionZh.c_str(), project.DescriptionEn.c_str(), project.Sort));
    request->Respond(builder, status);
}

Optional<ProjectRow> WorkerLoadProject(uint32 id)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_PROJECT);
    stmt->setUInt32(0, id);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};
    return ProjectRow::FromFields(result->Fetch());
}

// An admin call's id (ApiCall.h)
Optional<uint32> GetId(flatbuffers::Optional<uint32> id)
{
    return id && *id ? Optional<uint32>(*id) : Optional<uint32>();
}
}

Optional<boost::json::object> ReadSiteProfile(boost::json::object const& body, boost::json::object& errors)
{
    RequestFields fields(body);
    Optional<boost::json::object> profile = ReadProfile(fields, body);
    errors = fields.TakeErrors();
    return profile;
}

void WorkerSaveSiteProfile(std::string const& json)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_REP_SITE_SETTING);
    stmt->setString(0, std::string_view("profile"));
    stmt->setString(1, json);
    BlogDatabase.DirectExecute(stmt);
}

void ApplySiteProfile(boost::json::object profile)
{
    sWorld->SetSiteProfile(std::move(profile));
    sWorld->RequestRebuild();
}

// The profile, then everything else read from site_setting, at startup
boost::json::object LoadSiteProfileDirect()
{
    PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_SITE_SETTINGS));
    if (rows)
    {
        do
        {
            Field* fields = rows->Fetch();
            if (fields[0].GetString() != "profile")
                continue;

            boost::system::error_code error;
            boost::json::value value = boost::json::parse(fields[1].GetString(), error);
            if (!error && value.is_object())
                return std::move(value.as_object());
            RT_LOG_ERROR("blog", "site_setting \"profile\" is not a JSON object, using an empty profile");
        } while (rows->NextRow());
    }
    return {};
}

namespace
{
// The site's page: the profile as the admin saved it, the counts, the days since it began (a new one
// every day, so the answer itself isn't kept)
void RespondSite(ApiRequest const& request, std::string const& lang, SiteCounts const& counts)
{
    boost::json::object const& profile = sWorld->GetSiteProfile();
    std::string since = GetString(profile, "since");
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::Social>> socials;
    if (boost::json::value const* list = profile.if_contains("socials"); list && list->is_array())
        for (boost::json::value const& item : list->as_array())
            if (item.is_object())
                socials.push_back(Reader::CreateSocial(builder, builder.CreateString(GetString(item.as_object(), "kind")),
                    builder.CreateString(GetString(item.as_object(), "url"))));

    builder.Finish(Reader::CreateSite(builder, builder.CreateString(sWorld->GetSiteName(lang)), builder.CreateString(GetString(profile, "author")),
        builder.CreateString(GetString(profile, "avatar")), builder.CreateString(Localized(profile, "tagline", lang)),
        builder.CreateString(Localized(profile, "bio", lang)), builder.CreateString(Localized(profile, "quote", lang)),
        builder.CreateString(CreatedDate(since)), builder.CreateVector(socials),
        Reader::CreateSiteStats(builder, counts.Posts, counts.Notes, counts.Thinking, counts.Words, DaysSince(since))));
    request->Respond(builder);
}
}

Rooftree::Task<void> GetSite(Session& session, ApiRequest request)
{
    std::string lang = ReadLang(request->Get<Reader::LangRequest>().lang());
    ReadCache& cache = sWorld->GetReadCache();
    if (Optional<SiteCounts> const& counts = cache.GetSiteCounts())
        co_return RespondSite(request, lang, *counts);

    auto generation = cache.GetGeneration();
    SiteCounts counts = co_await session.OnWorker([]
    {
        SiteCounts counts;
        if (PreparedQueryResult row = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_SITE_STATS)))
        {
            Field* fields = row->Fetch();
            counts = { fields[0].GetInt64(), fields[1].GetInt64(), fields[2].GetInt64(), fields[3].GetInt64() };
        }
        return counts;
    });
    sWorld->GetReadCache().StoreSiteCounts(counts, generation);
    RespondSite(request, lang, counts);
}

Rooftree::Task<void> GetAbout(Session& /*session*/, ApiRequest request)
{
    std::string lang = ReadLang(request->Get<Reader::LangRequest>().lang());
    boost::json::object const& profile = sWorld->GetSiteProfile();
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateAboutDirect(builder, Localized(profile, "about", lang).c_str(), Localized(profile, "aboutSite", lang).c_str()));
    request->Respond(builder);
    co_return;
}

Rooftree::Task<void> AdminStatus(Session& session, ApiRequest request)
{
    // The admin's only: readers see the site, not the machine under it
    // Only the samples after since: the page asks for the day once, then for what came since
    int64 since = request->Get<Admin::StatusRequest>().since().value_or(0);
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(sWorld->GetServerStatus().BuildTable(builder, since, sWorld->GetPresence()));
    request->Respond(builder);
    co_return;
}

Rooftree::Task<void> ListTimeline(Session& session, ApiRequest request)
{
    std::string lang = ReadLang(request->Get<Reader::LangRequest>().lang());
    struct Item
    {
        std::string Kind;
        uint32 Id;
        std::string Slug, Title;
        SystemTimePoint PublishedAt;
    };

    std::vector<Item> items = co_await session.OnWorker([lang]
    {
        std::vector<Item> items;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_TIMELINE);
        stmt->setString(0, lang);
        stmt->setString(1, lang);
        stmt->setString(2, lang);
        stmt->setUInt32(3, TimelineItems);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                items.push_back({ fields[0].GetString(), fields[1].GetUInt32(), fields[2].GetString(), fields[3].GetString(), fields[4].GetDate() });
            } while (rows->NextRow());
        }
        return items;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::TimelineItem>> rows;
    for (Item const& item : items)
        rows.push_back(Reader::CreateTimelineItem(builder, builder.CreateString(item.Kind), item.Id, builder.CreateString(item.Slug),
            builder.CreateString(item.Title), TimeString(builder, item.PublishedAt)));
    builder.Finish(Reader::CreateTimeline(builder, builder.CreateVector(rows)));
    request->Respond(builder);
}

Rooftree::Task<void> ListFriends(Session& session, ApiRequest request)
{
    struct Item
    {
        uint32 Id;
        std::string Name, Url, Avatar, Description;
    };

    std::vector<Item> friends = co_await session.OnWorker([]
    {
        std::vector<Item> friends;
        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_FRIENDS)))
        {
            do
            {
                Field* fields = rows->Fetch();
                friends.push_back({ fields[0].GetUInt32(), fields[1].GetString(), fields[2].GetString(), fields[3].GetString(), fields[4].GetString() });
            } while (rows->NextRow());
        }
        return friends;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::Friend>> links;
    for (Item const& link : friends)
        links.push_back(Reader::CreateFriendDirect(builder, link.Id, link.Name.c_str(), link.Url.c_str(), link.Avatar.c_str(),
            link.Description.c_str()));
    builder.Finish(Reader::CreateFriendsList(builder, builder.CreateVector(links)));
    request->Respond(builder);
}

Rooftree::Task<void> ApplyFriend(Session& session, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    if (!auth.User.EmailVerified)
        co_return request->RespondError(http::status::forbidden, "email_unverified");
    if (auth.User.IsDenied(DenyFriend))
        co_return request->RespondError(http::status::forbidden, "not_allowed");

    TableFields fields(request->GetErrors());
    Optional<FriendInput> input = ReadFriendInput(fields, request->Get<Reader::FriendApplication>());
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    uint32 userId = auth.User.Id;
    std::variant<std::string_view, FriendRow> result = co_await session.OnWorker([input = std::move(*input), userId]() -> std::variant<std::string_view, FriendRow>
    {
        if (WorkerFindFriend(input.Url))
            return "taken";

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_PENDING_FRIENDS);
        stmt->setUInt32(0, userId);
        if (PreparedQueryResult count = BlogDatabase.Query(stmt); count && count->Fetch()[0].GetInt64() >= MaxPendingFriendsPerUser)
            return "too_many_pending";

        stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_FRIEND);
        stmt->setString(0, input.Name);
        stmt->setString(1, input.Url);
        stmt->setString(2, input.Avatar);
        stmt->setString(3, input.Description);
        stmt->setString(4, std::string_view("pending"));
        stmt->setUInt32(5, userId);
        stmt->setString(6, input.Message);
        stmt->setInt32(7, 0);
        stmt->setNull(8);
        BlogDatabase.DirectExecute(stmt);

        Optional<uint32> id = WorkerFindFriend(input.Url);
        Optional<FriendRow> link = id ? WorkerLoadFriend(*id) : Optional<FriendRow>();
        if (!link)
            return "internal";
        return std::move(*link);
    });
    if (std::string_view const* error = std::get_if<std::string_view>(&result))
    {
        if (*error == "taken")
            co_return request->RespondError(http::status::conflict, "conflict", {}, { { "url", "taken" } });
        if (*error == "too_many_pending")
            co_return request->RespondError(http::status::too_many_requests, "too_many_pending");
        co_return RespondInternalError(request);
    }

    FriendRow const& link = std::get<FriendRow>(result);
    RT_LOG_INFO("blog", "{} applied for a friend link to {} ({})", link.ApplicantUsername, link.Url, link.Id);
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateFriendAppliedDirect(builder, link.Id, link.Status.c_str()));
    request->Respond(builder, http::status::created);
}

Rooftree::Task<void> AdminListFriends(Session& session, ApiRequest request)
{
    flatbuffers::String const* statusValue = request->Get<Admin::FriendsListRequest>().status();
    std::string status = statusValue ? statusValue->str() : std::string();
    if (!status.empty() && status != "pending" && status != "approved" && status != "rejected")
        co_return RespondValidation(request, { { "status", "invalid" } });

    std::vector<FriendRow> friends = co_await session.OnWorker([status]
    {
        std::vector<FriendRow> friends;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_FRIENDS);
        stmt->setString(0, status);
        stmt->setString(1, status);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
                friends.push_back(FriendRow::FromFields(rows->Fetch()));
            while (rows->NextRow());
        }
        return friends;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::Friend>> links;
    for (FriendRow const& link : friends)
        links.push_back(link.AdminTable(builder));
    builder.Finish(Admin::CreateFriendsList(builder, builder.CreateVector(links)));
    request->Respond(builder);
}

Rooftree::Task<void> AdminCreateFriend(Session& session, ApiRequest request)
{
    TableFields fields(request->GetErrors());
    Optional<FriendInput> input = ReadFriendInput(fields, request->Get<Admin::FriendInput>());
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    Optional<FriendRow> link = co_await session.OnWorker([input = std::move(*input)]() -> Optional<FriendRow>
    {
        if (WorkerFindFriend(input.Url))
            return {};

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_FRIEND);
        stmt->setString(0, input.Name);
        stmt->setString(1, input.Url);
        stmt->setString(2, input.Avatar);
        stmt->setString(3, input.Description);
        stmt->setString(4, std::string_view("approved"));
        stmt->setNull(5);
        stmt->setString(6, std::string_view(""));
        stmt->setInt32(7, input.Sort);
        stmt->setDate(8, SystemTimePoint::clock::now());
        BlogDatabase.DirectExecute(stmt);

        Optional<uint32> id = WorkerFindFriend(input.Url);
        return id ? WorkerLoadFriend(*id) : Optional<FriendRow>();
    });
    if (!link)
        co_return request->RespondError(http::status::conflict, "conflict", {}, { { "url", "taken" } });

    sWorld->RequestRebuild();
    RespondFriend(request, *link, http::status::created);
}

Rooftree::Task<void> AdminUpdateFriend(Session& session, ApiRequest request)
{
    Admin::FriendInput const& in = request->Get<Admin::FriendInput>();
    Optional<uint32> id = GetId(in.id());
    if (!id)
        co_return RespondNotFound(request);

    // Either a review (status, with a reason for a rejection) or an edit of the link
    TableFields fields(request->GetErrors());
    std::string status = in.status() ? fields.Choice("status", in.status(), { "approved", "rejected" }) : std::string();
    std::string reason = fields.Text("rejectReason", in.rejectReason(), 0, 255, false);
    Optional<FriendInput> input;
    if (in.name() || in.url())
        input = ReadFriendInput(fields, in);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        Optional<FriendRow> Before;
        Optional<FriendRow> After;
        bool UrlTaken = false;
    };

    Result result = co_await session.OnWorker([id = *id, status, reason, input]
    {
        Result result;
        result.Before = WorkerLoadFriend(id);
        if (!result.Before)
            return result;

        if (input)
        {
            Optional<uint32> other = WorkerFindFriend(input->Url);
            if (other && *other != id)
            {
                result.UrlTaken = true;
                return result;
            }

            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_FRIEND);
            stmt->setString(0, input->Name);
            stmt->setString(1, input->Url);
            stmt->setString(2, input->Avatar);
            stmt->setString(3, input->Description);
            stmt->setInt32(4, input->Sort);
            stmt->setUInt32(5, id);
            BlogDatabase.DirectExecute(stmt);
        }

        if (!status.empty())
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_FRIEND_REVIEW);
            stmt->setString(0, status);
            stmt->setString(1, status == "rejected" ? reason : std::string());
            stmt->setUInt32(2, id);
            BlogDatabase.DirectExecute(stmt);
        }

        result.After = WorkerLoadFriend(id);
        return result;
    });
    if (!result.Before)
        co_return RespondNotFound(request);
    if (result.UrlTaken)
        co_return request->RespondError(http::status::conflict, "conflict", {}, { { "url", "taken" } });
    if (!result.After)
        co_return RespondInternalError(request);

    FriendRow const& link = *result.After;
    // An application decided: tell the applicant
    if (result.Before->Status == "pending" && link.Status != "pending" && !link.ApplicantEmail.empty())
        sWorld->SendMail(session, FriendReviewMail(link, link.Status == "approved"));
    if (result.Before->Status == "approved" || link.Status == "approved")
        sWorld->RequestRebuild();

    RT_LOG_INFO("blog.admin", "Friend link {} ({}) is {}", link.Url, link.Id, link.Status);
    RespondFriend(request, link);
}

Rooftree::Task<void> AdminDeleteFriend(Session& session, ApiRequest request)
{
    Optional<uint32> id = GetId(request->Get<Admin::IdRequest>().id());
    if (!id)
        co_return RespondNotFound(request);

    Optional<FriendRow> link = co_await session.OnWorker([id = *id]
    {
        Optional<FriendRow> link = WorkerLoadFriend(id);
        if (link)
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_FRIEND);
            stmt->setUInt32(0, id);
            BlogDatabase.DirectExecute(stmt);
        }
        return link;
    });
    if (!link)
        co_return RespondNotFound(request);
    if (link->Status == "approved")
        sWorld->RequestRebuild();
    RespondOk(request);
}

Rooftree::Task<void> ListProjects(Session& session, ApiRequest request)
{
    std::vector<ProjectRow> projects = co_await session.OnWorker([]
    {
        std::vector<ProjectRow> projects;
        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_PROJECTS)))
        {
            do
                projects.push_back(ProjectRow::FromFields(rows->Fetch()));
            while (rows->NextRow());
        }
        return projects;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::Project>> items;
    for (ProjectRow const& project : projects)
        items.push_back(Reader::CreateProjectDirect(builder, project.Id, project.Name.c_str(), project.Url.c_str(), project.RepoUrl.c_str(),
            project.Icon.c_str(), project.DescriptionZh.c_str(), project.DescriptionEn.c_str(), project.Sort));
    builder.Finish(Reader::CreateProjectsList(builder, builder.CreateVector(items)));
    request->Respond(builder);
}

Rooftree::Task<void> AdminCreateProject(Session& session, ApiRequest request)
{
    TableFields fields(request->GetErrors());
    Optional<ProjectInput> input = ReadProjectInput(fields, request->Get<Admin::ProjectInput>());
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    Optional<ProjectRow> project = co_await session.OnWorker([input = std::move(*input)]() -> Optional<ProjectRow>
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_PROJECT);
        stmt->setString(0, input.Name);
        stmt->setString(1, input.Url);
        stmt->setString(2, input.RepoUrl);
        stmt->setString(3, input.Icon);
        stmt->setString(4, input.DescriptionZh);
        stmt->setString(5, input.DescriptionEn);
        stmt->setInt32(6, input.Sort);
        BlogDatabase.DirectExecute(stmt);

        PreparedQueryResult newest = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_NEWEST_PROJECT_ID));
        if (!newest || newest->Fetch()[0].IsNull())
            return {};
        return WorkerLoadProject(newest->Fetch()[0].GetUInt32());
    });
    if (!project)
        co_return RespondInternalError(request);
    sWorld->RequestRebuild();
    RespondProject(request, *project, http::status::created);
}

Rooftree::Task<void> AdminUpdateProject(Session& session, ApiRequest request)
{
    Admin::ProjectInput const& in = request->Get<Admin::ProjectInput>();
    Optional<uint32> id = GetId(in.id());
    if (!id)
        co_return RespondNotFound(request);

    TableFields fields(request->GetErrors());
    Optional<ProjectInput> input = ReadProjectInput(fields, in);
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    Optional<ProjectRow> project = co_await session.OnWorker([input = std::move(*input), id = *id]() -> Optional<ProjectRow>
    {
        if (!WorkerLoadProject(id))
            return {};

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_PROJECT);
        stmt->setString(0, input.Name);
        stmt->setString(1, input.Url);
        stmt->setString(2, input.RepoUrl);
        stmt->setString(3, input.Icon);
        stmt->setString(4, input.DescriptionZh);
        stmt->setString(5, input.DescriptionEn);
        stmt->setInt32(6, input.Sort);
        stmt->setUInt32(7, id);
        BlogDatabase.DirectExecute(stmt);
        return WorkerLoadProject(id);
    });
    if (!project)
        co_return RespondNotFound(request);
    sWorld->RequestRebuild();
    RespondProject(request, *project);
}

Rooftree::Task<void> AdminDeleteProject(Session& session, ApiRequest request)
{
    Optional<uint32> id = GetId(request->Get<Admin::IdRequest>().id());
    if (!id)
        co_return RespondNotFound(request);

    bool found = co_await session.OnWorker([id = *id]
    {
        bool found = WorkerLoadProject(id).has_value();
        if (found)
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_PROJECT);
            stmt->setUInt32(0, id);
            BlogDatabase.DirectExecute(stmt);
        }
        return found;
    });
    if (!found)
        co_return RespondNotFound(request);
    sWorld->RequestRebuild();
    RespondOk(request);
}

Rooftree::Task<void> AdminGetSite(Session& session, ApiRequest request)
{
    RespondProfile(request, sWorld->GetSiteProfile());
    co_return;
}

Rooftree::Task<void> AdminUpdateSite(Session& session, ApiRequest request)
{
    TableFields fields(request->GetErrors());
    Optional<boost::json::object> profile = ReadProfile(fields, request->Get<Admin::SiteProfile>());
    if (!profile)
        co_return RespondValidation(request, fields.TakeErrors());

    std::string json = boost::json::serialize(*profile);
    co_await session.OnWorker([json] { WorkerSaveSiteProfile(json); });
    RT_LOG_INFO("blog.admin", "Updated the site's profile");
    RespondProfile(request, *profile);
    ApplySiteProfile(std::move(*profile));
}

Rooftree::Task<void> ListUploadsMeta(Session& session, ApiRequest request)
{
    struct Image
    {
        std::string Url, Thumbhash;
        uint16 Width, Height;
    };

    std::vector<Image> images = co_await session.OnWorker([]
    {
        std::vector<Image> images;
        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_UPLOAD_META)))
        {
            do
            {
                Field* fields = rows->Fetch();
                images.push_back({ fields[0].GetString(), fields[1].GetStringOrNull().value_or(""), fields[2].GetUInt16(), fields[3].GetUInt16() });
            } while (rows->NextRow());
        }
        return images;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::ImageMeta>> items;
    for (Image const& image : images)
        items.push_back(Reader::CreateImageMetaDirect(builder, image.Url.c_str(), image.Thumbhash.c_str(), image.Width, image.Height));
    builder.Finish(Reader::CreateUploadsMeta(builder, builder.CreateVector(items)));
    request->Respond(builder);
}
}
