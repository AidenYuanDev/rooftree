/*
 * What a request costs before and after its handler, the database aside: the blog's admin posts
 * (BenchPackets.fbs) over HTTP's JSON and as packets (Packet.h), with the handlers working in JSON
 * (the packets bridged by FlatJsonSchema) or in FlatBuffers (JSON converted by it at the edge).
 *
 *   now    HTTP: JSON in, JSON out             packet: FB -> JSON -> handler -> JSON -> FB
 *   A      HTTP: JSON -> FB -> handler -> FB -> JSON      packet: FB in, FB out
 *
 * A handler "reads" its request as validation does (every field, its length) and builds its answer
 * from a row as the blog's handlers do.
 */

#include "rooftree_benchmark.h"

#include "BenchPackets_bfbs_generated.h"
#include "BenchPackets_generated.h"
#include "FlatJson.h"
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

using Rooftree::Server::FlatJsonSchema;

namespace
{
struct Text
{
    std::string Lang, Title, Summary, Body;
};

// A post as the database gives it
struct PostRow
{
    uint32 Id = 42;
    std::string Slug = "one-terminal-on-windows-and-linux";
    std::string Status = "published";
    std::string OriginalLang = "zh";
    std::string Cover = "/uploads/AbCdEfGhIjKlMnOp.webp";
    bool CommentsEnabled = true;
    std::vector<std::string> Tags = { "terminal", "zsh", "windows" };
    std::vector<Text> Translations;
    std::string PublishedAt = "2026-10-28T12:56:00Z", UpdatedAt = "2026-10-28T12:56:00Z", CreatedAt = "2026-10-28T12:40:00Z";
};

std::string Repeat(std::string_view piece, std::size_t bytes)
{
    std::string text;
    while (text.size() < bytes)
        text += piece;
    return text;
}

// A real post's size: 18 KB of Chinese, 12 KB of English
PostRow MakePost()
{
    PostRow post;
    post.Translations = {
        { "zh", "让 Windows 和 Linux 用同一个终端：MSYS2 + zsh + powerlevel10k", "一套配置，两个系统。",
            Repeat("在 Windows 上装 MSYS2，然后把 zsh 设成默认 shell，再装上 powerlevel10k。\n", 18000) },
        { "en", "One Terminal on Windows and Linux: MSYS2 + zsh + powerlevel10k", "One setup, two systems.",
            Repeat("Install MSYS2 on Windows, make zsh the default shell, then add powerlevel10k.\n", 12000) },
    };
    return post;
}

// AdminPostJson
boost::json::object PostJson(PostRow const& post)
{
    boost::json::object translations;
    for (Text const& text : post.Translations)
        translations[text.Lang] = { { "title", text.Title }, { "summary", text.Summary }, { "body", text.Body } };
    boost::json::array tags;
    for (std::string const& tag : post.Tags)
        tags.emplace_back(tag);
    return {
        { "id", post.Id }, { "slug", post.Slug }, { "status", post.Status }, { "originalLang", post.OriginalLang }, { "cover", post.Cover },
        { "commentsEnabled", post.CommentsEnabled }, { "tags", std::move(tags) }, { "translations", std::move(translations) },
        { "publishedAt", post.PublishedAt }, { "publishAt", nullptr }, { "updatedAt", post.UpdatedAt }, { "createdAt", post.CreatedAt },
    };
}

flatbuffers::Offset<BenchPackets::PostText> TextTable(flatbuffers::FlatBufferBuilder& builder, PostRow const& post, std::string_view lang)
{
    for (Text const& text : post.Translations)
        if (text.Lang == lang)
            return BenchPackets::CreatePostTextDirect(builder, text.Title.c_str(), text.Summary.c_str(), text.Body.c_str());
    return 0;
}

flatbuffers::Offset<BenchPackets::Post> PostTable(flatbuffers::FlatBufferBuilder& builder, PostRow const& post)
{
    auto zh = TextTable(builder, post, "zh");
    auto en = TextTable(builder, post, "en");
    auto translations = BenchPackets::CreatePostTranslations(builder, zh, en);
    std::vector<flatbuffers::Offset<flatbuffers::String>> tags;
    for (std::string const& tag : post.Tags)
        tags.push_back(builder.CreateString(tag));
    return BenchPackets::CreatePostDirect(builder, post.Id, post.Slug.c_str(), post.Status.c_str(), post.OriginalLang.c_str(), post.Cover.c_str(),
        post.CommentsEnabled, &tags, translations, post.PublishedAt.c_str(), nullptr, post.UpdatedAt.c_str(), post.CreatedAt.c_str());
}

// A page of the admin's list: 20 posts without their bodies
boost::json::object ListJson(PostRow const& post)
{
    boost::json::array posts;
    for (uint32 i = 0; i < 20; ++i)
        posts.push_back({ { "id", post.Id + i }, { "slug", post.Slug }, { "status", post.Status }, { "originalLang", post.OriginalLang },
            { "updatedAt", post.UpdatedAt }, { "publishedAt", post.PublishedAt }, { "title", post.Translations[0].Title },
            { "translations", boost::json::array{ "zh", "en" } }, { "publishAt", nullptr } });
    return { { "posts", std::move(posts) }, { "hasMore", true } };
}

flatbuffers::Offset<BenchPackets::PostsList> ListTable(flatbuffers::FlatBufferBuilder& builder, PostRow const& post)
{
    std::vector<flatbuffers::Offset<BenchPackets::PostsListItem>> posts;
    std::vector<std::string> langs = { "zh", "en" };
    for (uint32 i = 0; i < 20; ++i)
    {
        auto translations = builder.CreateVectorOfStrings(langs);
        posts.push_back(BenchPackets::CreatePostsListItem(builder, post.Id + i, builder.CreateString(post.Slug), builder.CreateString(post.Status),
            builder.CreateString(post.OriginalLang), builder.CreateString(post.UpdatedAt), builder.CreateString(post.PublishedAt),
            builder.CreateString(post.Translations[0].Title), translations));
    }
    return BenchPackets::CreatePostsList(builder, builder.CreateVector(posts), true);
}

// What a create's validation looks at, as JSON (RequestFields) and as a table
std::size_t ReadInput(boost::json::object const& body)
{
    std::size_t total = 0;
    auto text = [&](boost::json::object const& object, std::string_view key)
    {
        if (boost::json::value const* value = object.if_contains(key); value && value->is_string())
            total += value->get_string().size();
    };
    for (std::string_view key : { "slug", "status", "originalLang", "cover" })
        text(body, key);
    if (boost::json::value const* tags = body.if_contains("tags"); tags && tags->is_array())
        for (boost::json::value const& tag : tags->get_array())
            total += tag.is_string() ? tag.get_string().size() : 0;
    if (boost::json::value const* translations = body.if_contains("translations"); translations && translations->is_object())
        for (auto const& [lang, value] : translations->get_object())
            if (value.is_object())
                for (std::string_view key : { "title", "summary", "body" })
                    text(value.get_object(), key);
    return total;
}

std::size_t ReadInput(BenchPackets::PostInput const& input)
{
    std::size_t total = 0;
    for (flatbuffers::String const* text : { input.slug(), input.status(), input.originalLang(), input.cover() })
        total += text ? text->size() : 0;
    if (input.tags())
        for (flatbuffers::String const* tag : *input.tags())
            total += tag->size();
    if (BenchPackets::PostTranslations const* translations = input.translations())
        for (BenchPackets::PostText const* text : { translations->zh(), translations->en() })
            if (text)
                for (flatbuffers::String const* field : { text->title(), text->summary(), text->body() })
                    total += field ? field->size() : 0;
    return total;
}

boost::json::object InputJson(PostRow const& post)
{
    boost::json::object json = PostJson(post);
    for (std::string_view key : { "id", "publishedAt", "publishAt", "updatedAt", "createdAt" })
        json.erase(key);
    return json;
}

struct Schema
{
    Schema()
    {
        std::string error;
        REQUIRE(Flat.Load(error));
        Post = Flat.Find("BenchPackets.Post");
        Input = Flat.Find("BenchPackets.PostInput");
        List = Flat.Find("BenchPackets.PostsList");
    }

    FlatJsonSchema Flat{ BenchPackets::PostBinarySchema::data() };
    reflection::Object const* Post;
    reflection::Object const* Input;
    reflection::Object const* List;
};

std::span<uint8 const> Bytes(std::string const& data)
{
    return { reinterpret_cast<uint8 const*>(data.data()), data.size() };
}
}

TEST_CASE("Admin posts: JSON handlers or FlatBuffers handlers", "[packet]")
{
    Schema schema;
    PostRow const post = MakePost();

    // What clients send: a create's body as JSON text and as a FlatBuffer
    std::string const inputText = boost::json::serialize(InputJson(post));
    Rooftree::Server::FlatJsonErrors errors;
    std::string const inputPacket = schema.Flat.FromJson(*schema.Input, InputJson(post), errors);
    REQUIRE(errors.empty());

    INFO("create: " << inputText.size() << " bytes of JSON, " << inputPacket.size() << " of FlatBuffer");

    // A handler working in JSON (the blog now)
    BENCHMARK("now, HTTP: create's request (parse, read)")
    {
        return ReadInput(boost::json::parse(inputText).as_object());
    };
    BENCHMARK("now, packet: create's request (verify, to JSON, text, parse, read)")
    {
        if (!schema.Flat.Verify(*schema.Input, Bytes(inputPacket)))
            return std::size_t(0);
        std::string text = boost::json::serialize(schema.Flat.ToJson(*schema.Input, Bytes(inputPacket)));
        return ReadInput(boost::json::parse(text).as_object());
    };
    BENCHMARK("now, HTTP: a post's answer (build JSON, text)")
    {
        return boost::json::serialize(PostJson(post)).size();
    };
    BENCHMARK("now, packet: a post's answer (build JSON, to FlatBuffer)")
    {
        return schema.Flat.FromJson(*schema.Post, PostJson(post), errors).size();
    };
    BENCHMARK("now, HTTP: a list's answer")
    {
        return boost::json::serialize(ListJson(post)).size();
    };
    BENCHMARK("now, packet: a list's answer")
    {
        return schema.Flat.FromJson(*schema.List, ListJson(post), errors).size();
    };

    // A handler working in FlatBuffers (A)
    BENCHMARK("A, packet: create's request (verify, read)")
    {
        flatbuffers::Verifier verifier(reinterpret_cast<uint8 const*>(inputPacket.data()), inputPacket.size());
        if (!verifier.VerifyBuffer<BenchPackets::PostInput>(nullptr))
            return std::size_t(0);
        return ReadInput(*flatbuffers::GetRoot<BenchPackets::PostInput>(inputPacket.data()));
    };
    BENCHMARK("A, HTTP: create's request (parse, to FlatBuffer, read)")
    {
        std::string buffer = schema.Flat.FromJson(*schema.Input, boost::json::parse(inputText).as_object(), errors);
        return ReadInput(*flatbuffers::GetRoot<BenchPackets::PostInput>(buffer.data()));
    };
    BENCHMARK("A, packet: a post's answer (build FlatBuffer)")
    {
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(PostTable(builder, post));
        return builder.GetSize();
    };
    BENCHMARK("A, HTTP: a post's answer (build FlatBuffer, to JSON, text)")
    {
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(PostTable(builder, post));
        return boost::json::serialize(schema.Flat.ToJson(*schema.Post, { builder.GetBufferPointer(), builder.GetSize() })).size();
    };
    BENCHMARK("A, packet: a list's answer")
    {
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(ListTable(builder, post));
        return builder.GetSize();
    };
    BENCHMARK("A, HTTP: a list's answer")
    {
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(ListTable(builder, post));
        return boost::json::serialize(schema.Flat.ToJson(*schema.List, { builder.GetBufferPointer(), builder.GetSize() })).size();
    };
}
