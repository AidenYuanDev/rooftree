/*
 * Packets (Packet.h): the header, the Error packet, and FlatJsonSchema turning a FlatBuffer into JSON
 * and back by a binary schema (TestPackets.fbs).
 */

#include "rooftree_catch2.h"

#include "FlatJson.h"
#include "Packet.h"
#include "Packets_generated.h"
#include "TestPackets_bfbs_generated.h"
#include "TestPackets_generated.h"
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

using namespace Rooftree::Server;

namespace
{
std::span<uint8 const> Bytes(std::string_view data)
{
    return { reinterpret_cast<uint8 const*>(data.data()), data.size() };
}

boost::json::object Parse(std::string_view json)
{
    return boost::json::parse(json).as_object();
}

struct Schema
{
    Schema()
    {
        std::string error;
        REQUIRE(Flat.Load(error));
        Everything = Flat.Find("Test.Everything");
        REQUIRE(Everything);
    }

    FlatJsonSchema Flat{ Test::EverythingBinarySchema::data() };
    reflection::Object const* Everything = nullptr;
};
}

TEST_CASE("Packet headers", "[server][packet]")
{
    std::string packet = WritePacket({ .Opcode = 0x1234, .Flags = 0, .RequestId = 0xA1B2C3D4 }, Bytes("xyz"));
    REQUIRE(packet.size() == PacketHeaderSize + 3);
    CHECK(packet.substr(0, 8) == std::string("\x34\x12\x00\x00\xD4\xC3\xB2\xA1", 8));

    Optional<PacketHeader> header = ReadPacketHeader(packet);
    REQUIRE(header);
    CHECK(header->Opcode == 0x1234);
    CHECK(header->RequestId == 0xA1B2C3D4);
    CHECK(packet.substr(PacketHeaderSize) == "xyz");

    CHECK_FALSE(ReadPacketHeader(std::string_view("\x01\x02\x03", 3)));
}

TEST_CASE("Error packets", "[server][packet]")
{
    std::string packet = WriteErrorPacket(9, 0x0101, 400, "validation_failed", "bad", { { "slug", "taken" }, { "translations.zh", "required" } });
    Optional<PacketHeader> header = ReadPacketHeader(packet);
    REQUIRE(header);
    CHECK(header->Opcode == ErrorOpcode);
    CHECK(header->RequestId == 9);

    std::span<uint8 const> payload = Bytes(packet).subspan(PacketHeaderSize);
    flatbuffers::Verifier verifier(payload.data(), payload.size());
    REQUIRE(verifier.VerifyBuffer<Rooftree::Packets::Error>(nullptr));
    Rooftree::Packets::Error const* error = flatbuffers::GetRoot<Rooftree::Packets::Error>(payload.data());
    CHECK(error->status() == 400);
    CHECK(error->code()->str() == "validation_failed");
    CHECK(error->message()->str() == "bad");
    CHECK(error->opcode() == 0x0101);
    REQUIRE(error->fields()->size() == 2);
    CHECK(error->fields()->Get(1)->name()->str() == "translations.zh");
    CHECK(error->fields()->Get(1)->code()->str() == "required");
    CHECK(error->retryAfter() == 0);
    CHECK_FALSE(error->details());
}

TEST_CASE("Error packets with details", "[server][packet]")
{
    std::string payload = MakeErrorPayload(0x0201, PacketError{ .Status = 403, .Code = "banned", .RetryAfter = 30,
        .Details = { { .Name = "until", .Value = "2030-01-01T00:00:00Z" }, { .Name = "translations", .Values = std::vector<std::string>{ "en" } } } });
    flatbuffers::Verifier verifier(reinterpret_cast<uint8 const*>(payload.data()), payload.size());
    REQUIRE(verifier.VerifyBuffer<Rooftree::Packets::Error>(nullptr));
    Rooftree::Packets::Error const* error = flatbuffers::GetRoot<Rooftree::Packets::Error>(payload.data());
    CHECK(error->status() == 403);
    CHECK(error->opcode() == 0x0201);
    CHECK(error->retryAfter() == 30);
    CHECK_FALSE(error->message());
    REQUIRE(error->details()->size() == 2);
    CHECK(error->details()->Get(0)->name()->str() == "until");
    CHECK(error->details()->Get(0)->value()->str() == "2030-01-01T00:00:00Z");
    CHECK_FALSE(error->details()->Get(0)->values());
    CHECK_FALSE(error->details()->Get(1)->value());
    REQUIRE(error->details()->Get(1)->values()->size() == 1);
    CHECK(error->details()->Get(1)->values()->Get(0)->str() == "en");
}

TEST_CASE("FlatJsonSchema to JSON", "[server][packet]")
{
    Schema schema;

    SECTION("every field, what wasn't set as JSON says nothing")
    {
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(Test::EverythingBuilder(builder).Finish());
        CHECK(schema.Flat.ToJson(*schema.Everything, { builder.GetBufferPointer(), builder.GetSize() }) == Parse(R"({"flag": null, "small": null,
            "count": null, "big": null, "ratio": null, "plain": 7, "color": null, "name": "", "gone": null, "child": null, "items": [], "tags": [],
            "numbers": [], "colors": [], "named": null})"));
    }

    SECTION("what was set")
    {
        flatbuffers::FlatBufferBuilder builder;
        auto items = builder.CreateVector({ Test::CreateItem(builder, 1, builder.CreateString("a")), Test::CreateItem(builder, 2) });
        auto tags = builder.CreateVectorOfStrings({ "x", "y" });
        auto numbers = builder.CreateVector(std::vector<int16>{ -3, 4 });
        auto colors = builder.CreateVector(std::vector<Test::Color>{ Test::Color::Blue, Test::Color(9) });
        auto name = builder.CreateString("名字");
        auto extra = builder.CreateString("");
        Test::EverythingBuilder everything(builder);
        everything.add_flag(false);
        everything.add_count(0);
        everything.add_big(std::numeric_limits<uint64>::max());
        everything.add_ratio(2.5);
        everything.add_color(Test::Color::Green);
        everything.add_name(name);
        everything.add_items(items);
        everything.add_tags(tags);
        everything.add_numbers(numbers);
        everything.add_colors(colors);
        everything.add_extra(extra);
        builder.Finish(everything.Finish());

        std::span<uint8 const> buffer(builder.GetBufferPointer(), builder.GetSize());
        REQUIRE(schema.Flat.Verify(*schema.Everything, buffer));
        CHECK(schema.Flat.ToJson(*schema.Everything, buffer) == Parse(R"({"flag": false, "small": null, "count": 0, "big": 18446744073709551615,
            "ratio": 2.5, "plain": 7, "color": "Green", "name": "名字", "gone": null, "child": null,
            "items": [{"id": 1, "name": "a"}, {"id": 2, "name": ""}], "tags": ["x", "y"], "numbers": [-3, 4], "colors": ["Blue", 9],
            "extra": "", "named": null})"));
    }

    SECTION("json_null: empty is null")
    {
        flatbuffers::FlatBufferBuilder builder;
        auto gone = builder.CreateString("");
        auto child = Test::CreateItem(builder);
        Test::EverythingBuilder everything(builder);
        everything.add_gone(gone);
        everything.add_child(child);
        builder.Finish(everything.Finish());

        boost::json::object json = schema.Flat.ToJson(*schema.Everything, { builder.GetBufferPointer(), builder.GetSize() });
        CHECK(json["gone"].is_null());
        CHECK(json["child"].is_null());
    }

    SECTION("a buffer that isn't one doesn't verify")
    {
        std::string garbage("\xff\xff\xff\x7f\x01\x02\x03\x04", 8);
        CHECK_FALSE(schema.Flat.Verify(*schema.Everything, Bytes(garbage)));
        CHECK_FALSE(schema.Flat.Verify(*schema.Everything, Bytes("")));
    }
}

TEST_CASE("FlatJsonSchema json_null on a scalar", "[server][packet]")
{
    Schema schema;
    reflection::Object const* move = schema.Flat.Find("Test.Move");
    REQUIRE(move);
    FlatJsonErrors errors;

    // null: there, as 0; absent: not there
    std::string buffer = schema.Flat.FromJson(*move, Parse(R"({"folder": null})"), errors);
    CHECK(errors.empty());
    REQUIRE(flatbuffers::GetRoot<Test::Move>(buffer.data())->folder() == 0u);
    CHECK(schema.Flat.ToJson(*move, Bytes(buffer)) == Parse(R"({"folder": null})"));

    buffer = schema.Flat.FromJson(*move, Parse("{}"), errors);
    CHECK_FALSE(flatbuffers::GetRoot<Test::Move>(buffer.data())->folder());
    CHECK(schema.Flat.ToJson(*move, Bytes(buffer)) == Parse(R"({"folder": null})"));

    buffer = schema.Flat.FromJson(*move, Parse(R"({"folder": 7})"), errors);
    CHECK(flatbuffers::GetRoot<Test::Move>(buffer.data())->folder() == 7u);
    CHECK(schema.Flat.ToJson(*move, Bytes(buffer)) == Parse(R"({"folder": 7})"));
}

TEST_CASE("FlatJsonSchema json_map", "[server][packet]")
{
    Schema schema;
    reflection::Object const* maps = schema.Flat.Find("Test.Maps");
    REQUIRE(maps);
    FlatJsonErrors errors;

    SECTION("an object by key, both ways")
    {
        boost::json::object json = Parse(R"({"counts": {"like": 3, "star": 0}, "sizes": {"/a.png": {"width": 1, "height": 2}}})");
        std::string buffer = schema.Flat.FromJson(*maps, json, errors);
        CHECK(errors.empty());
        REQUIRE(schema.Flat.Verify(*maps, Bytes(buffer)));

        Test::Maps const* table = flatbuffers::GetRoot<Test::Maps>(buffer.data());
        REQUIRE(table->counts()->size() == 2);
        CHECK(table->counts()->Get(0)->key()->str() == "like");
        CHECK(table->counts()->Get(0)->count() == 3u);
        CHECK(table->sizes()->Get(0)->key()->str() == "/a.png");
        CHECK(table->sizes()->Get(0)->height() == 2);
        CHECK(schema.Flat.ToJson(*maps, Bytes(buffer)) == json);
    }

    SECTION("none is an empty object")
    {
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(Test::MapsBuilder(builder).Finish());
        CHECK(schema.Flat.ToJson(*maps, { builder.GetBufferPointer(), builder.GetSize() }) == Parse(R"({"counts": {}, "sizes": {}})"));
    }

    SECTION("what doesn't fit, named by its key")
    {
        std::string buffer = schema.Flat.FromJson(*maps, Parse(R"({"counts": {"like": -1, "star": 2}, "sizes": {"/a.png": 5}})"), errors);
        CHECK(errors == FlatJsonErrors{ { "counts.like", "out_of_range" }, { "sizes./a.png", "not_an_object" } });
        CHECK(schema.Flat.ToJson(*maps, Bytes(buffer)) == Parse(R"({"counts": {}, "sizes": {}})"));

        schema.Flat.FromJson(*maps, Parse(R"({"counts": [1]})"), errors);
        CHECK(errors == FlatJsonErrors{ { "counts", "not_an_object" } });
    }
}

TEST_CASE("FlatJsonSchema from JSON", "[server][packet]")
{
    Schema schema;
    FlatJsonErrors errors;

    SECTION("back to the same JSON, a default stored when it was sent")
    {
        boost::json::object json = Parse(R"({"flag": false, "small": -128, "count": 0, "big": 18446744073709551615, "ratio": 1, "plain": 7,
            "color": "Blue", "name": "名字", "gone": null, "child": null, "items": [{"id": 4294967295, "name": ""}], "tags": [],
            "numbers": [32767], "colors": [2, "Red"], "named": {"id": 3, "name": "x"}})");
        std::string buffer = schema.Flat.FromJson(*schema.Everything, json, errors);
        CHECK(errors.empty());
        REQUIRE(schema.Flat.Verify(*schema.Everything, Bytes(buffer)));

        Test::Everything const* everything = flatbuffers::GetRoot<Test::Everything>(buffer.data());
        CHECK(everything->flag() == false);
        CHECK(everything->count() == 0u);
        CHECK(everything->color() == Test::Color::Blue);
        CHECK(everything->name()->str() == "名字");
        // json_null's null: empty, not absent
        REQUIRE(everything->gone());
        CHECK(everything->gone()->size() == 0);
        CHECK(everything->child());

        json["ratio"] = 1.0;
        json["colors"] = boost::json::array{ "Blue", "Red" };
        CHECK(schema.Flat.ToJson(*schema.Everything, Bytes(buffer)) == json);
    }

    SECTION("what Boost.JSON holds as uint64, of a C++ unsigned")
    {
        boost::json::object json{ { "count", uint32(5) }, { "items", boost::json::array{ boost::json::object{ { "id", uint64(7) } } } } };
        REQUIRE(json["count"].is_uint64());
        std::string buffer = schema.Flat.FromJson(*schema.Everything, json, errors);
        CHECK(errors.empty());
        CHECK(flatbuffers::GetRoot<Test::Everything>(buffer.data())->count() == 5u);
    }

    SECTION("other nulls and unknown keys are left out")
    {
        std::string buffer = schema.Flat.FromJson(*schema.Everything, Parse(R"({"count": null, "name": null, "what": 1})"), errors);
        CHECK(errors.empty());
        Test::Everything const* everything = flatbuffers::GetRoot<Test::Everything>(buffer.data());
        CHECK_FALSE(everything->count());
        CHECK_FALSE(everything->name());
    }

    SECTION("what doesn't fit is left out and named, the rest is there")
    {
        auto fails = [&](std::string_view json)
        {
            std::string buffer = schema.Flat.FromJson(*schema.Everything, Parse(json), errors);
            REQUIRE(schema.Flat.Verify(*schema.Everything, Bytes(buffer)));
            REQUIRE(errors.size() == 1);
            return errors[0].first + ": " + errors[0].second;
        };

        CHECK(fails(R"({"small": 128})") == "small: out_of_range");
        CHECK(fails(R"({"count": -1})") == "count: out_of_range");
        CHECK(fails(R"({"count": 1.5})") == "count: out_of_range");
        CHECK(fails(R"({"count": "1"})") == "count: not_an_integer");
        CHECK(fails(R"({"flag": 1})") == "flag: not_a_bool");
        CHECK(fails(R"({"color": "Purple"})") == "color: unknown_value");
        CHECK(fails(R"({"name": 3})") == "name: not_a_string");
        CHECK(fails(R"({"child": []})") == "child: not_an_object");
        CHECK(fails(R"({"items": [{"id": 1}, {"id": -1}]})") == "items.1.id: out_of_range");
        CHECK(fails(R"({"tags": ["a", null]})") == "tags.1: null");
        CHECK(fails(R"({"numbers": [1, "2"]})") == "numbers.1: not_an_integer");
        CHECK(fails(R"({"named": {"id": 1, "what": 2}})") == "named: unknown_key");

        std::string buffer = schema.Flat.FromJson(*schema.Everything, Parse(R"({"count": "x", "flag": 2, "name": "kept", "tags": [1]})"), errors);
        CHECK(errors.size() == 3);
        Test::Everything const* everything = flatbuffers::GetRoot<Test::Everything>(buffer.data());
        CHECK(everything->name()->str() == "kept");
        CHECK_FALSE(everything->count());
        CHECK_FALSE(everything->tags());
    }

    SECTION("a URL's text as a field's value")
    {
        CHECK(schema.Flat.FromText(*schema.Everything, "count", "12") == boost::json::value(12));
        CHECK(schema.Flat.FromText(*schema.Everything, "flag", "true") == boost::json::value(true));
        CHECK(schema.Flat.FromText(*schema.Everything, "name", "a b") == boost::json::value("a b"));
        CHECK_FALSE(schema.Flat.FromText(*schema.Everything, "count", "abc"));
        CHECK_FALSE(schema.Flat.FromText(*schema.Everything, "nothing", "1"));
        CHECK(schema.Flat.GetFieldNames(*schema.Everything).size() == 16);
    }
}
