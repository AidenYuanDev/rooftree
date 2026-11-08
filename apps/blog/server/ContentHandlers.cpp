/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Notes and thinking, the shorter kinds of writing next to posts.
 *
 * A note is a diary entry: addressed by its number (/notes/12) instead of a slug, with a mood and the
 * weather. A thinking is a one-liner, shown whole in its list. Both have translations like posts: one
 * per language, the original one always there; each language's site has only those written in it,
 * like posts (PostHandlers.cpp). Comments on both go through CommentHandlers.cpp (note:12, thinking:5).
 */

#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Transaction.h"
#include "Validation.h"
#include <boost/json/array.hpp>
#include <map>
#include <variant>

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
constexpr uint32 DefaultPageSize = 10;
constexpr uint32 MaxPageSize = 50;
constexpr uint32 AdminPageSize = 50;
constexpr std::size_t MaxNoteLength = 200000;
constexpr std::size_t MaxThinkingLength = 2000;

struct NoteText
{
    std::string Lang;
    std::string Title;
    std::string Body;
};

struct NoteRow
{
    uint32 Id = 0;
    std::string Status;
    std::string OriginalLang;
    std::string Mood;
    std::string Weather;
    std::string Cover;
    bool CommentsEnabled = true;
    Optional<SystemTimePoint> PublishedAt;
    SystemTimePoint UpdatedAt;
    Optional<SystemTimePoint> CreatedAt;
    std::vector<std::string> Langs;
    std::vector<NoteText> Translations;

    bool IsPublished() const { return Status == "published"; }
};

// ?limit= and ?cursor= of a list; responds 400 and returns false for a bad cursor
// A list's page: false, answered, for a cursor that isn't one
bool ReadPage(ApiRequest const& request, uint32& limit, SystemTimePoint& beforeTime, uint32& beforeId)
{
    Reader::PageRequest const& in = request->Get<Reader::PageRequest>();
    limit = std::clamp(in.limit().value_or(DefaultPageSize), 1u, MaxPageSize);

    // The first page starts after everything
    beforeTime = PermanentBanTime();
    beforeId = std::numeric_limits<uint32>::max();
    if (in.cursor() && in.cursor()->size())
    {
        Optional<std::pair<SystemTimePoint, uint32>> decoded = DecodeCursor(in.cursor()->string_view());
        if (!decoded)
        {
            request->RespondError(http::status::bad_request, "invalid_cursor");
            return false;
        }
        std::tie(beforeTime, beforeId) = *decoded;
    }
    return true;
}

// A call's (ApiCall.h)
Optional<uint32> GetId(flatbuffers::Optional<uint32> id)
{
    return id && *id ? Optional<uint32>(*id) : Optional<uint32>();
}

// Worker
Optional<NoteRow> WorkerLoadNote(uint32 id)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_NOTE_BY_ID);
    stmt->setUInt32(0, id);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    Field* fields = result->Fetch();
    NoteRow note;
    note.Id = fields[0].GetUInt32();
    note.Status = fields[1].GetString();
    note.OriginalLang = fields[2].GetString();
    note.Mood = fields[3].GetString();
    note.Weather = fields[4].GetString();
    note.Cover = fields[5].GetString();
    note.CommentsEnabled = fields[6].GetBool();
    note.PublishedAt = fields[7].GetDateOrNull();
    note.UpdatedAt = fields[8].GetDate();
    note.CreatedAt = fields[9].GetDate();

    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_NOTE_TRANSLATIONS);
    stmt->setUInt32(0, id);
    if (PreparedQueryResult translations = BlogDatabase.Query(stmt))
    {
        do
        {
            Field* row = translations->Fetch();
            note.Translations.push_back({ row[0].GetString(), row[1].GetString(), row[2].GetString() });
            note.Langs.push_back(row[0].GetString());
        } while (translations->NextRow());
    }

    return note;
}

void RespondNote(ApiRequest const& request, NoteRow const& note, http::status status = http::status::ok)
{
    flatbuffers::FlatBufferBuilder builder;
    flatbuffers::Offset<Admin::NoteText> texts[2];
    for (NoteText const& text : note.Translations)
        texts[text.Lang == "en"] = Admin::CreateNoteTextDirect(builder, text.Title.c_str(), text.Body.c_str());
    auto translations = Admin::CreateNoteTranslations(builder, texts[0], texts[1]);
    builder.Finish(Admin::CreateNote(builder, note.Id, builder.CreateString(note.Status), builder.CreateString(note.OriginalLang),
        builder.CreateString(note.Mood), builder.CreateString(note.Weather), builder.CreateString(note.Cover), note.CommentsEnabled, translations,
        TimeString(builder, note.PublishedAt), TimeString(builder, note.UpdatedAt), TimeString(builder, note.CreatedAt)));
    request->Respond(builder, status);
}

struct NoteNeighbour
{
    uint32 Id;
    std::string Title;
};

Optional<NoteNeighbour> WorkerFindNoteNeighbour(BlogDatabaseStatements statement, std::string const& lang, SystemTimePoint publishedAt, uint32 id)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(statement);
    stmt->setString(0, lang);
    stmt->setDate(1, publishedAt);
    stmt->setDate(2, publishedAt);
    stmt->setUInt32(3, id);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    Field* fields = result->Fetch();
    return NoteNeighbour{ fields[0].GetUInt32(), fields[1].GetString() };
}

flatbuffers::Offset<Reader::NoteNeighbour> NoteNeighbourTable(flatbuffers::FlatBufferBuilder& builder, Optional<NoteNeighbour> const& neighbour)
{
    return neighbour ? Reader::CreateNoteNeighbourDirect(builder, neighbour->Id, neighbour->Title.c_str()) : 0;
}

std::string OriginalTitle(NoteRow const& note)
{
    for (NoteText const& text : note.Translations)
        if (text.Lang == note.OriginalLang)
            return text.Title;
    return {};
}

void WorkerUpdateNoteWords(NoteRow const& note)
{
    for (NoteText const& text : note.Translations)
    {
        if (text.Lang != note.OriginalLang)
            continue;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_NOTE_WORDS);
        stmt->setUInt32(0, uint32(CountWords(text.Title) + CountWords(text.Body)));
        stmt->setUInt32(1, note.Id);
        BlogDatabase.DirectExecute(stmt);
    }
}

bool IsValidImage(std::string_view url)
{
    return url.empty() || url.starts_with("/uploads/") || url.starts_with("https://");
}

// The translations of a create (required, the original language among them) or an update (any; an
// empty one, null in JSON, deletes a language). readText reads one language's table
template<typename Text, typename Translations, typename ReadText>
std::map<std::string, Optional<Text>> ReadTranslations(TableFields& fields, Translations const* translations, bool creating,
    std::string const& originalLang, ReadText readText)
{
    std::map<std::string, Optional<Text>> result;
    if (translations)
    {
        for (auto [lang, text] : { std::pair(std::string("zh"), translations->zh()), std::pair(std::string("en"), translations->en()) })
        {
            if (!text)
                continue;

            if (IsEmptyTable(text) && !creating)
            {
                result[lang] = std::nullopt;
                continue;
            }

            fields.SetPrefix("translations." + lang);
            Text read = readText(fields, *text);
            fields.SetPrefix({});
            read.Lang = lang;
            result[lang] = std::move(read);
        }
    }
    else if (creating)
        fields.AddError("translations", "required");

    if (creating && !originalLang.empty() && !result.contains(originalLang))
        fields.AddError("translations." + originalLang, "required");
    return result;
}

struct NoteInput
{
    Optional<std::string> Status;
    Optional<std::string> OriginalLang;
    Optional<std::string> Mood;
    Optional<std::string> Weather;
    Optional<std::string> Cover;
    Optional<bool> CommentsEnabled;
    // Mail it to the subscribers, when published (once)
    bool Notify = false;
    std::map<std::string, Optional<NoteText>> Translations;
};

Optional<NoteInput> ReadNoteInput(bool creating, Admin::NoteInput const& in, TableFields& fields)
{
    NoteInput input;
    if (creating || in.status())
    {
        input.Status = fields.Choice("status", in.status(), { "draft", "published" }, false);
        if (input.Status->empty())
            input.Status = "draft";
    }
    if (creating || in.originalLang())
        input.OriginalLang = fields.Lang("originalLang", in.originalLang());
    if (creating || in.mood())
        input.Mood = fields.Text("mood", in.mood(), 0, 20, false);
    if (creating || in.weather())
        input.Weather = fields.Text("weather", in.weather(), 0, 20, false);
    if (creating || in.cover())
    {
        input.Cover = fields.Text("cover", in.cover(), 0, 255, false);
        if (!IsValidImage(*input.Cover))
            fields.AddError("cover", "invalid");
    }
    if (in.commentsEnabled())
        input.CommentsEnabled = *in.commentsEnabled();
    input.Notify = in.notify().value_or(false);

    input.Translations = ReadTranslations<NoteText>(fields, in.translations(), creating, input.OriginalLang.value_or(""),
        [](TableFields& text, Admin::NoteText const& table)
    {
        NoteText note;
        note.Title = text.Text("title", table.title(), 1, 200);
        note.Body = text.Text("body", table.body(), 1, MaxNoteLength, true, true);
        return note;
    });

    if (!fields.IsValid())
        return {};
    return input;
}

// The languages of a row after an update: they must still include the original one
template<typename Text>
bool KeepsOriginal(std::vector<std::string> langs, std::map<std::string, Optional<Text>> const& changes, std::string const& originalLang)
{
    for (auto const& [lang, text] : changes)
    {
        std::erase(langs, lang);
        if (text)
            langs.push_back(lang);
    }
    return std::ranges::find(langs, originalLang) != langs.end();
}
}

Rooftree::Task<void> ListNotes(Session& session, ApiRequest request)
{
    std::string lang = ReadLang(request->Get<Reader::PageRequest>().lang());
    uint32 limit;
    SystemTimePoint beforeTime;
    uint32 beforeId;
    if (!ReadPage(request, limit, beforeTime, beforeId))
        co_return;

    struct Item
    {
        uint32 Id;
        std::string Mood, Weather, Cover;
        SystemTimePoint PublishedAt, UpdatedAt;
        std::string OriginalLang, Lang, Title, Excerpt;
        std::vector<std::string> Langs;
        int64 CommentCount;
    };

    std::vector<Item> notes = co_await session.OnWorker([=]
    {
        std::vector<Item> notes;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_NOTES);
        stmt->setString(0, lang);
        stmt->setDate(1, beforeTime);
        stmt->setDate(2, beforeTime);
        stmt->setUInt32(3, beforeId);
        // One more than a page tells whether there is a next one
        stmt->setUInt32(4, limit + 1);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                // The excerpt is the start of the text, still markdown: lists show a plain excerpt of it
                notes.push_back({ fields[0].GetUInt32(), fields[1].GetString(), fields[2].GetString(), fields[3].GetString(), fields[4].GetDate(),
                    fields[5].GetDate(), fields[6].GetString(), fields[7].GetString(), fields[8].GetString(), fields[9].GetString(),
                    SplitList(fields[10].GetStringOrNull().value_or("")), fields[11].GetInt64() });
            } while (rows->NextRow());
        }
        return notes;
    });
    std::string nextCursor;
    if (notes.size() > limit)
    {
        notes.resize(limit);
        nextCursor = EncodeCursor(notes.back().PublishedAt, notes.back().Id);
    }

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::NoteSummary>> items;
    for (Item const& note : notes)
        items.push_back(Reader::CreateNoteSummary(builder, note.Id, builder.CreateString(note.Mood), builder.CreateString(note.Weather),
            builder.CreateString(note.Cover), TimeString(builder, note.PublishedAt), TimeString(builder, note.UpdatedAt),
            builder.CreateString(note.OriginalLang), builder.CreateString(note.Lang), note.Lang != lang, builder.CreateString(note.Title),
            builder.CreateString(note.Excerpt), StringList(builder, note.Langs), note.CommentCount));
    builder.Finish(Reader::CreateNotesList(builder, builder.CreateVector(items), nextCursor.empty() ? 0 : builder.CreateString(nextCursor)));
    request->Respond(builder);
}

Rooftree::Task<void> GetNote(Session& session, ApiRequest request)
{
    Reader::NotesGetRequest const& in = request->Get<Reader::NotesGetRequest>();
    Optional<uint32> id = GetId(in.id());
    if (!id)
        co_return RespondNotFound(request);

    std::string lang = ReadLang(in.lang());
    struct Found
    {
        uint32 Id;
        std::string Mood, Weather, Cover;
        bool CommentsEnabled;
        SystemTimePoint PublishedAt, UpdatedAt;
        std::string OriginalLang;
        uint32 WordCount;
        std::vector<std::string> Langs;
        int64 CommentCount;
        std::string Title, Body;
        Optional<NoteNeighbour> Previous, Next;
    };

    // Found, or not in lang: the languages it has
    using Result = std::variant<std::monostate, Found, std::vector<std::string>>;
    Result result = co_await session.OnWorker([id = *id, lang]() -> Result
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_NOTE);
        stmt->setUInt32(0, id);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row || row->Fetch()[1].GetString() != "published")
            return {};

        Field* fields = row->Fetch();
        Found note{ .Id = fields[0].GetUInt32(), .Mood = fields[2].GetString(), .Weather = fields[3].GetString(), .Cover = fields[4].GetString(),
            .CommentsEnabled = fields[5].GetBool(), .PublishedAt = fields[6].GetDate(), .UpdatedAt = fields[7].GetDate(),
            .OriginalLang = fields[8].GetString(), .WordCount = fields[9].GetUInt32(), .Langs = SplitList(fields[10].GetStringOrNull().value_or("")),
            .CommentCount = fields[11].GetInt64() };

        if (std::ranges::find(note.Langs, lang) == note.Langs.end())
            return std::move(note.Langs);
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_NOTE_TRANSLATION);
        stmt->setUInt32(0, id);
        stmt->setString(1, lang);
        PreparedQueryResult text = BlogDatabase.Query(stmt);
        if (!text)
            return {};

        note.Title = text->Fetch()[1].GetString();
        note.Body = text->Fetch()[2].GetString();
        // Older and newer
        note.Previous = WorkerFindNoteNeighbour(BLOG_SEL_NOTE_PREV, lang, note.PublishedAt, id);
        note.Next = WorkerFindNoteNeighbour(BLOG_SEL_NOTE_NEXT, lang, note.PublishedAt, id);
        return note;
    });
    if (auto* langs = std::get_if<std::vector<std::string>>(&result))
        co_return RespondNotInLang(request, *langs);
    Found const* note = std::get_if<Found>(&result);
    if (!note)
        co_return RespondNotFound(request);

    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateNote(builder, note->Id, builder.CreateString(note->Mood), builder.CreateString(note->Weather),
        builder.CreateString(note->Cover), note->CommentsEnabled, TimeString(builder, note->PublishedAt), TimeString(builder, note->UpdatedAt),
        builder.CreateString(note->OriginalLang), note->WordCount, StringList(builder, note->Langs), note->CommentCount,
        builder.CreateString(lang), false, builder.CreateString(note->Title), builder.CreateString(note->Body),
        NoteNeighbourTable(builder, note->Previous), NoteNeighbourTable(builder, note->Next)));
    request->Respond(builder);
}

// A row of an admin's list of notes or thinking
struct AdminContentItem
{
    uint32 Id;
    std::string Status, OriginalLang;
    SystemTimePoint UpdatedAt;
    Optional<SystemTimePoint> PublishedAt;
    std::string Text;       // a note's title, a thinking's body
    std::vector<std::string> Langs;

    static AdminContentItem FromFields(Field const* fields)
    {
        return { .Id = fields[0].GetUInt32(), .Status = fields[1].GetString(), .OriginalLang = fields[2].GetString(), .UpdatedAt = fields[3].GetDate(),
            .PublishedAt = fields[4].GetDateOrNull(), .Text = fields[5].GetString(), .Langs = SplitList(fields[6].GetStringOrNull().value_or("")) };
    }
};

Rooftree::Task<void> AdminListNotes(Session& session, ApiRequest request)
{
    Admin::NotesListRequest const& in = request->Get<Admin::NotesListRequest>();
    std::string status = in.status() ? in.status()->str() : std::string();
    if (!status.empty() && status != "draft" && status != "published")
        co_return RespondValidation(request, { { "status", "invalid" } });

    uint32 page = std::max(1u, in.page().value_or(1));
    std::vector<AdminContentItem> notes = co_await session.OnWorker([status, page]
    {
        std::vector<AdminContentItem> notes;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_NOTES);
        stmt->setString(0, status);
        stmt->setString(1, status);
        stmt->setUInt32(2, AdminPageSize + 1);
        stmt->setUInt32(3, (page - 1) * AdminPageSize);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                notes.push_back(AdminContentItem::FromFields(rows->Fetch()));
            } while (rows->NextRow());
        }
        return notes;
    });
    bool hasMore = notes.size() > AdminPageSize;
    if (hasMore)
        notes.resize(AdminPageSize);

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::NotesListItem>> items;
    for (AdminContentItem const& note : notes)
        items.push_back(Admin::CreateNotesListItem(builder, note.Id, builder.CreateString(note.Status), builder.CreateString(note.OriginalLang),
            TimeString(builder, note.UpdatedAt), TimeString(builder, note.PublishedAt), builder.CreateString(note.Text),
            StringList(builder, note.Langs)));
    builder.Finish(Admin::CreateNotesList(builder, builder.CreateVector(items), hasMore));
    request->Respond(builder);
}

Rooftree::Task<void> AdminGetNote(Session& session, ApiRequest request)
{
    Optional<uint32> id = GetId(request->Get<Admin::IdRequest>().id());
    if (!id)
        co_return RespondNotFound(request);

    Optional<NoteRow> note = co_await session.OnWorker([id = *id] { return WorkerLoadNote(id); });
    if (!note)
        co_return RespondNotFound(request);
    RespondNote(request, *note);
}

Rooftree::Task<void> AdminCreateNote(Session& session, ApiRequest request)
{
    Admin::NoteInput const& in = request->Get<Admin::NoteInput>();
    TableFields fields(request->GetErrors());
    Optional<NoteInput> input = ReadNoteInput(true, in, fields);
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    bool notify = input->Notify;
    Optional<NoteRow> note = co_await session.OnWorker([input = std::move(*input)]() -> Optional<NoteRow>
    {
        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_NOTE);
        stmt->setString(0, *input.Status);
        stmt->setString(1, *input.OriginalLang);
        stmt->setString(2, *input.Mood);
        stmt->setString(3, *input.Weather);
        stmt->setString(4, *input.Cover);
        stmt->setBool(5, input.CommentsEnabled.value_or(true));
        NoteText const& original = *input.Translations.at(*input.OriginalLang);
        stmt->setUInt32(6, uint32(CountWords(original.Title) + CountWords(original.Body)));
        if (*input.Status == "published")
            stmt->setDate(7, SystemTimePoint::clock::now());
        else
            stmt->setNull(7);
        trans->Append(stmt);

        // In the transaction that inserted the note, LAST_INSERT_ID() is still its id
        for (auto const& [lang, text] : input.Translations)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_NEW_NOTE_TRANSLATION);
            stmt->setString(0, lang);
            stmt->setString(1, text->Title);
            stmt->setString(2, text->Body);
            trans->Append(stmt);
        }
        BlogDatabase.DirectCommitTransaction(trans);

        PreparedQueryResult newest = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_NEWEST_NOTE_ID));
        if (!newest || newest->Fetch()[0].IsNull())
            return {};
        return WorkerLoadNote(newest->Fetch()[0].GetUInt32());
    });
    if (!note)
        co_return RespondInternalError(request);

    RT_LOG_INFO("blog.admin", "Created note {}", note->Id);
    if (note->IsPublished())
    {
        sWorld->RequestRebuild();
        sWorld->AnnouncePublished("note", Rooftree::StringFormat("/notes/{}", note->Id), OriginalTitle(*note));
        if (notify)
            sWorld->QueueNewsletter("note", note->Id);
    }
    RespondNote(request, *note, http::status::created);
}

Rooftree::Task<void> AdminUpdateNote(Session& session, ApiRequest request)
{
    Admin::NoteInput const& in = request->Get<Admin::NoteInput>();
    Optional<uint32> id = GetId(in.id());
    if (!id)
        co_return RespondNotFound(request);

    TableFields fields(request->GetErrors());
    Optional<NoteInput> input = ReadNoteInput(false, in, fields);
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        bool NotFound = false;
        std::string MissingOriginal;
        bool WasPublished = false;
        Optional<NoteRow> Note;
    };

    bool notify = input->Notify;
    Result result = co_await session.OnWorker([input = std::move(*input), id = *id]
    {
        Result result;
        Optional<NoteRow> current = WorkerLoadNote(id);
        if (!current)
        {
            result.NotFound = true;
            return result;
        }

        result.WasPublished = current->IsPublished();
        std::string status = input.Status.value_or(current->Status);
        std::string originalLang = input.OriginalLang.value_or(current->OriginalLang);
        if (!KeepsOriginal(current->Langs, input.Translations, originalLang))
        {
            result.MissingOriginal = originalLang;
            return result;
        }

        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_NOTE);
        stmt->setString(0, status);
        stmt->setString(1, originalLang);
        stmt->setString(2, input.Mood.value_or(current->Mood));
        stmt->setString(3, input.Weather.value_or(current->Weather));
        stmt->setString(4, input.Cover.value_or(current->Cover));
        stmt->setBool(5, input.CommentsEnabled.value_or(current->CommentsEnabled));
        if (status == "published")
            stmt->setDate(6, SystemTimePoint::clock::now());
        else
            stmt->setNull(6);
        stmt->setUInt32(7, id);
        trans->Append(stmt);

        for (auto const& [lang, text] : input.Translations)
        {
            if (text)
            {
                stmt = BlogDatabase.GetPreparedStatement(BLOG_REP_NOTE_TRANSLATION);
                stmt->setUInt32(0, id);
                stmt->setString(1, lang);
                stmt->setString(2, text->Title);
                stmt->setString(3, text->Body);
            }
            else
            {
                stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_NOTE_TRANSLATION);
                stmt->setUInt32(0, id);
                stmt->setString(1, lang);
            }
            trans->Append(stmt);
        }

        BlogDatabase.DirectCommitTransaction(trans);
        result.Note = WorkerLoadNote(id);
        if (result.Note)
            WorkerUpdateNoteWords(*result.Note);
        return result;
    });
    if (result.NotFound)
        co_return RespondNotFound(request);
    if (!result.MissingOriginal.empty())
        co_return RespondValidation(request, { { "translations." + result.MissingOriginal, "required" } });
    if (!result.Note)
        co_return RespondInternalError(request);
    // Mailed at most once (mailed_at), so asking again for a published note does nothing
    if (notify && result.Note->IsPublished())
        sWorld->QueueNewsletter("note", result.Note->Id);

    RT_LOG_INFO("blog.admin", "Updated note {}", result.Note->Id);
    if (result.WasPublished || result.Note->IsPublished())
    {
        sWorld->RequestRebuild();
        sWorld->AnnounceChanged(Rooftree::StringFormat("note:{}", result.Note->Id), "content");
    }
    if (!result.WasPublished && result.Note->IsPublished())
        sWorld->AnnouncePublished("note", Rooftree::StringFormat("/notes/{}", result.Note->Id), OriginalTitle(*result.Note));
    RespondNote(request, *result.Note);
}

Rooftree::Task<void> AdminDeleteNote(Session& session, ApiRequest request)
{
    Optional<uint32> id = GetId(request->Get<Admin::IdRequest>().id());
    if (!id)
        co_return RespondNotFound(request);

    Optional<NoteRow> note = co_await session.OnWorker([id = *id]
    {
        Optional<NoteRow> note = WorkerLoadNote(id);
        if (note)
        {
            // Its translations and comments go with it (ON DELETE CASCADE)
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_NOTE);
            stmt->setUInt32(0, id);
            BlogDatabase.DirectExecute(stmt);
        }
        return note;
    });
    if (!note)
        co_return RespondNotFound(request);

    RT_LOG_INFO("blog.admin", "Deleted note {}", note->Id);
    if (note->IsPublished())
    {
        sWorld->RequestRebuild();
        sWorld->AnnounceChanged(Rooftree::StringFormat("note:{}", note->Id), "content");
    }
    RespondOk(request);
}

namespace
{
struct ThinkingText
{
    std::string Lang;
    std::string Body;
};

struct ThinkingRow
{
    uint32 Id = 0;
    std::string Status;
    std::string OriginalLang;
    bool CommentsEnabled = true;
    Optional<SystemTimePoint> PublishedAt;
    SystemTimePoint UpdatedAt;
    SystemTimePoint CreatedAt;
    std::vector<std::string> Langs;
    std::vector<ThinkingText> Translations;

    bool IsPublished() const { return Status == "published"; }
};

Optional<ThinkingRow> WorkerLoadThinking(uint32 id)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_THINKING_BY_ID);
    stmt->setUInt32(0, id);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    Field* fields = result->Fetch();
    ThinkingRow thinking;
    thinking.Id = fields[0].GetUInt32();
    thinking.Status = fields[1].GetString();
    thinking.OriginalLang = fields[2].GetString();
    thinking.CommentsEnabled = fields[3].GetBool();
    thinking.PublishedAt = fields[4].GetDateOrNull();
    thinking.UpdatedAt = fields[5].GetDate();
    thinking.CreatedAt = fields[6].GetDate();

    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_THINKING_TRANSLATIONS);
    stmt->setUInt32(0, id);
    if (PreparedQueryResult translations = BlogDatabase.Query(stmt))
    {
        do
        {
            Field* row = translations->Fetch();
            thinking.Translations.push_back({ row[0].GetString(), row[1].GetString() });
            thinking.Langs.push_back(row[0].GetString());
        } while (translations->NextRow());
    }

    return thinking;
}

void RespondThinking(ApiRequest const& request, ThinkingRow const& thinking, http::status status = http::status::ok)
{
    flatbuffers::FlatBufferBuilder builder;
    flatbuffers::Offset<Admin::ThinkingText> texts[2];
    for (ThinkingText const& text : thinking.Translations)
        texts[text.Lang == "en"] = Admin::CreateThinkingTextDirect(builder, text.Body.c_str());
    auto translations = Admin::CreateThinkingTranslations(builder, texts[0], texts[1]);
    builder.Finish(Admin::CreateThinking(builder, thinking.Id, builder.CreateString(thinking.Status), builder.CreateString(thinking.OriginalLang),
        thinking.CommentsEnabled, translations, TimeString(builder, thinking.PublishedAt), TimeString(builder, thinking.UpdatedAt),
        TimeString(builder, thinking.CreatedAt)));
    request->Respond(builder, status);
}

struct ThinkingInput
{
    Optional<std::string> Status;
    Optional<std::string> OriginalLang;
    Optional<bool> CommentsEnabled;
    std::map<std::string, Optional<ThinkingText>> Translations;
};

Optional<ThinkingInput> ReadThinkingInput(bool creating, Admin::ThinkingInput const& in, TableFields& fields)
{
    ThinkingInput input;
    if (creating || in.status())
    {
        input.Status = fields.Choice("status", in.status(), { "draft", "published" }, false);
        if (input.Status->empty())
            input.Status = "published";
    }
    if (creating || in.originalLang())
        input.OriginalLang = fields.Lang("originalLang", in.originalLang());
    if (in.commentsEnabled())
        input.CommentsEnabled = *in.commentsEnabled();

    input.Translations = ReadTranslations<ThinkingText>(fields, in.translations(), creating, input.OriginalLang.value_or(""),
        [](TableFields& text, Admin::ThinkingText const& table)
    {
        return ThinkingText{ .Body = text.Text("body", table.body(), 1, MaxThinkingLength, true, true) };
    });

    if (!fields.IsValid())
        return {};
    return input;
}
}

Rooftree::Task<void> ListThinking(Session& session, ApiRequest request)
{
    std::string lang = ReadLang(request->Get<Reader::PageRequest>().lang());
    uint32 limit;
    SystemTimePoint beforeTime;
    uint32 beforeId;
    if (!ReadPage(request, limit, beforeTime, beforeId))
        co_return;

    struct Item
    {
        uint32 Id;
        SystemTimePoint PublishedAt;
        std::string OriginalLang, Lang, Body;
        std::vector<std::string> Langs;
        int64 CommentCount;
        bool CommentsEnabled;
    };

    std::vector<Item> items = co_await session.OnWorker([=]
    {
        std::vector<Item> items;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_THINKING_LIST);
        stmt->setString(0, lang);
        stmt->setDate(1, beforeTime);
        stmt->setDate(2, beforeTime);
        stmt->setUInt32(3, beforeId);
        stmt->setUInt32(4, limit + 1);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                items.push_back({ fields[0].GetUInt32(), fields[1].GetDate(), fields[2].GetString(), fields[3].GetString(), fields[4].GetString(),
                    SplitList(fields[5].GetStringOrNull().value_or("")), fields[6].GetInt64(), fields[7].GetBool() });
            } while (rows->NextRow());
        }
        return items;
    });
    std::string nextCursor;
    if (items.size() > limit)
    {
        items.resize(limit);
        nextCursor = EncodeCursor(items.back().PublishedAt, items.back().Id);
    }

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::ThinkingItem>> rows;
    for (Item const& item : items)
        rows.push_back(Reader::CreateThinkingItem(builder, item.Id, TimeString(builder, item.PublishedAt), builder.CreateString(item.OriginalLang),
            builder.CreateString(item.Lang), item.Lang != lang, builder.CreateString(item.Body), StringList(builder, item.Langs),
            item.CommentCount, item.CommentsEnabled));
    builder.Finish(Reader::CreateThinkingList(builder, builder.CreateVector(rows), nextCursor.empty() ? 0 : builder.CreateString(nextCursor)));
    request->Respond(builder);
}

Rooftree::Task<void> AdminListThinking(Session& session, ApiRequest request)
{
    uint32 page = std::max(1u, request->Get<Admin::PageRequest>().page().value_or(1));
    std::vector<AdminContentItem> items = co_await session.OnWorker([page]
    {
        std::vector<AdminContentItem> items;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_THINKING);
        stmt->setUInt32(0, AdminPageSize + 1);
        stmt->setUInt32(1, (page - 1) * AdminPageSize);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                items.push_back(AdminContentItem::FromFields(rows->Fetch()));
            } while (rows->NextRow());
        }
        return items;
    });
    bool hasMore = items.size() > AdminPageSize;
    if (hasMore)
        items.resize(AdminPageSize);

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::ThinkingListItem>> rows;
    for (AdminContentItem const& item : items)
        rows.push_back(Admin::CreateThinkingListItem(builder, item.Id, builder.CreateString(item.Status), builder.CreateString(item.OriginalLang),
            TimeString(builder, item.UpdatedAt), TimeString(builder, item.PublishedAt), builder.CreateString(item.Text),
            StringList(builder, item.Langs)));
    builder.Finish(Admin::CreateThinkingList(builder, builder.CreateVector(rows), hasMore));
    request->Respond(builder);
}

Rooftree::Task<void> AdminGetThinking(Session& session, ApiRequest request)
{
    Optional<uint32> id = GetId(request->Get<Admin::IdRequest>().id());
    if (!id)
        co_return RespondNotFound(request);

    Optional<ThinkingRow> thinking = co_await session.OnWorker([id = *id] { return WorkerLoadThinking(id); });
    if (!thinking)
        co_return RespondNotFound(request);
    RespondThinking(request, *thinking);
}

Rooftree::Task<void> AdminCreateThinking(Session& session, ApiRequest request)
{
    Admin::ThinkingInput const& in = request->Get<Admin::ThinkingInput>();
    TableFields fields(request->GetErrors());
    Optional<ThinkingInput> input = ReadThinkingInput(true, in, fields);
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    Optional<ThinkingRow> thinking = co_await session.OnWorker([input = std::move(*input)]() -> Optional<ThinkingRow>
    {
        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_THINKING);
        stmt->setString(0, *input.Status);
        stmt->setString(1, *input.OriginalLang);
        stmt->setBool(2, input.CommentsEnabled.value_or(true));
        if (*input.Status == "published")
            stmt->setDate(3, SystemTimePoint::clock::now());
        else
            stmt->setNull(3);
        trans->Append(stmt);

        for (auto const& [lang, text] : input.Translations)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_NEW_THINKING_TRANSLATION);
            stmt->setString(0, lang);
            stmt->setString(1, text->Body);
            trans->Append(stmt);
        }
        BlogDatabase.DirectCommitTransaction(trans);

        PreparedQueryResult newest = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_NEWEST_THINKING_ID));
        if (!newest || newest->Fetch()[0].IsNull())
            return {};
        return WorkerLoadThinking(newest->Fetch()[0].GetUInt32());
    });
    if (!thinking)
        co_return RespondInternalError(request);

    RT_LOG_INFO("blog.admin", "Created thinking {}", thinking->Id);
    if (thinking->IsPublished())
    {
        sWorld->RequestRebuild();
        sWorld->AnnounceChanged("thinking", "content");
        std::string text;
        for (ThinkingText const& translation : thinking->Translations)
            if (translation.Lang == thinking->OriginalLang)
                text = translation.Body;
        sWorld->AnnouncePublished("thinking", Rooftree::StringFormat("/thinking#thinking-{}", thinking->Id), std::move(text));
    }
    RespondThinking(request, *thinking, http::status::created);
}

Rooftree::Task<void> AdminUpdateThinking(Session& session, ApiRequest request)
{
    Admin::ThinkingInput const& in = request->Get<Admin::ThinkingInput>();
    Optional<uint32> id = GetId(in.id());
    if (!id)
        co_return RespondNotFound(request);

    TableFields fields(request->GetErrors());
    Optional<ThinkingInput> input = ReadThinkingInput(false, in, fields);
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        bool NotFound = false;
        std::string MissingOriginal;
        bool WasPublished = false;
        Optional<ThinkingRow> Thinking;
    };

    Result result = co_await session.OnWorker([input = std::move(*input), id = *id]
    {
        Result result;
        Optional<ThinkingRow> current = WorkerLoadThinking(id);
        if (!current)
        {
            result.NotFound = true;
            return result;
        }

        result.WasPublished = current->IsPublished();
        std::string status = input.Status.value_or(current->Status);
        std::string originalLang = input.OriginalLang.value_or(current->OriginalLang);
        if (!KeepsOriginal(current->Langs, input.Translations, originalLang))
        {
            result.MissingOriginal = originalLang;
            return result;
        }

        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_THINKING);
        stmt->setString(0, status);
        stmt->setString(1, originalLang);
        stmt->setBool(2, input.CommentsEnabled.value_or(current->CommentsEnabled));
        if (status == "published")
            stmt->setDate(3, SystemTimePoint::clock::now());
        else
            stmt->setNull(3);
        stmt->setUInt32(4, id);
        trans->Append(stmt);

        for (auto const& [lang, text] : input.Translations)
        {
            if (text)
            {
                stmt = BlogDatabase.GetPreparedStatement(BLOG_REP_THINKING_TRANSLATION);
                stmt->setUInt32(0, id);
                stmt->setString(1, lang);
                stmt->setString(2, text->Body);
            }
            else
            {
                stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_THINKING_TRANSLATION);
                stmt->setUInt32(0, id);
                stmt->setString(1, lang);
            }
            trans->Append(stmt);
        }

        BlogDatabase.DirectCommitTransaction(trans);
        result.Thinking = WorkerLoadThinking(id);
        return result;
    });
    if (result.NotFound)
        co_return RespondNotFound(request);
    if (!result.MissingOriginal.empty())
        co_return RespondValidation(request, { { "translations." + result.MissingOriginal, "required" } });
    if (!result.Thinking)
        co_return RespondInternalError(request);

    if (result.WasPublished || result.Thinking->IsPublished())
    {
        sWorld->RequestRebuild();
        sWorld->AnnounceChanged("thinking", "content");
    }
    RespondThinking(request, *result.Thinking);
}

Rooftree::Task<void> AdminDeleteThinking(Session& session, ApiRequest request)
{
    Optional<uint32> id = GetId(request->Get<Admin::IdRequest>().id());
    if (!id)
        co_return RespondNotFound(request);

    Optional<ThinkingRow> thinking = co_await session.OnWorker([id = *id]
    {
        Optional<ThinkingRow> thinking = WorkerLoadThinking(id);
        if (thinking)
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_THINKING);
            stmt->setUInt32(0, id);
            BlogDatabase.DirectExecute(stmt);
        }
        return thinking;
    });
    if (!thinking)
        co_return RespondNotFound(request);

    if (thinking->IsPublished())
    {
        sWorld->RequestRebuild();
        sWorld->AnnounceChanged("thinking", "content");
    }
    RespondOk(request);
}
}
