/*
 * The picture captcha: GenerateCaptcha's answer and PNG (its chunks and checksums, read back the way a
 * decoder would), and CaptchaStore: answers without case and spaces, one check per captcha, expiry,
 * the oldest replaced past the limit.
 */

#include "rooftree_catch2.h"

#include "Captcha.h"
#include <cstring>

using namespace Rooftree::Server;
using namespace std::chrono_literals;

namespace
{
uint32 ReadUInt32(std::vector<uint8> const& data, std::size_t offset)
{
    return (uint32(data[offset]) << 24) | (uint32(data[offset + 1]) << 16) | (uint32(data[offset + 2]) << 8) | data[offset + 3];
}
}

TEST_CASE("GenerateCaptcha draws its answer as a grey PNG", "[Captcha]")
{
    CaptchaImage image = GenerateCaptcha(5);
    REQUIRE(image.Answer.size() == 5);
    for (char c : image.Answer)
        REQUIRE(std::string_view("ACDEFHJKMNPRTUVWXY34679").find(c) != std::string_view::npos);

    std::vector<uint8> const& png = image.Png;
    REQUIRE(png.size() > 8);
    REQUIRE(std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0);

    // IHDR, IDAT, IEND in order; IDAT holds a row of width bytes plus its filter byte per row
    std::vector<std::string> chunks;
    std::size_t offset = 8;
    std::size_t imageData = 0;
    while (offset + 12 <= png.size())
    {
        uint32 size = ReadUInt32(png, offset);
        std::string type(png.begin() + offset + 4, png.begin() + offset + 8);
        chunks.push_back(type);
        if (type == "IHDR")
        {
            REQUIRE(ReadUInt32(png, offset + 8) == image.Width);
            REQUIRE(ReadUInt32(png, offset + 12) == image.Height);
        }
        if (type == "IDAT")
            imageData = size;
        offset += 12 + size;
    }
    REQUIRE(offset == png.size());
    REQUIRE(chunks == std::vector<std::string>{ "IHDR", "IDAT", "IEND" });
    // zlib header, 5 bytes per stored block, the rows, adler32
    std::size_t rows = (image.Width + 1) * image.Height;
    REQUIRE(imageData == 2 + 5 * ((rows + 65534) / 65535) + rows + 4);

    // Two captchas differ
    REQUIRE(GenerateCaptcha(5).Png != image.Png);
}

TEST_CASE("CaptchaStore checks an answer once", "[Captcha]")
{
    TimePoint now = TimePoint::clock::now();
    CaptchaStore store(10min, 100);

    SECTION("right, without case and spaces")
    {
        CaptchaStore::Issued issued = store.Issue(5, now);
        std::string answer = issued.Image.Answer;
        for (char& c : answer)
            c = char(std::tolower(c));
        answer.insert(2, " ");
        REQUIRE(store.Check(issued.Id, answer, now));
        // Used up
        REQUIRE_FALSE(store.Check(issued.Id, issued.Image.Answer, now));
    }

    SECTION("a wrong answer uses it up too")
    {
        CaptchaStore::Issued issued = store.Issue(5, now);
        REQUIRE_FALSE(store.Check(issued.Id, "nope", now));
        REQUIRE_FALSE(store.Check(issued.Id, issued.Image.Answer, now));
        REQUIRE(store.GetCount() == 0);
    }

    SECTION("unknown ids and empty answers")
    {
        CaptchaStore::Issued issued = store.Issue(5, now);
        REQUIRE_FALSE(store.Check("unknown", issued.Image.Answer, now));
        REQUIRE_FALSE(store.Check(issued.Id, "", now));
    }

    SECTION("expired")
    {
        CaptchaStore::Issued issued = store.Issue(5, now);
        REQUIRE_FALSE(store.Check(issued.Id, issued.Image.Answer, now + 10min));

        CaptchaStore::Issued other = store.Issue(5, now);
        store.Cleanup(now + 11min);
        REQUIRE(store.GetCount() == 0);
        REQUIRE_FALSE(store.Check(other.Id, other.Image.Answer, now + 11min));
    }
}

TEST_CASE("CaptchaStore replaces the oldest past its limit", "[Captcha]")
{
    TimePoint now = TimePoint::clock::now();
    CaptchaStore store(10min, 3);
    std::vector<CaptchaStore::Issued> issued;
    for (int32 i = 0; i < 4; ++i)
        issued.push_back(store.Issue(4, now + std::chrono::seconds(i)));

    REQUIRE(store.GetCount() == 3);
    REQUIRE_FALSE(store.Check(issued[0].Id, issued[0].Image.Answer, now + 5s));
    REQUIRE(store.Check(issued[3].Id, issued[3].Image.Answer, now + 5s));
    REQUIRE(store.Check(issued[1].Id, issued[1].Image.Answer, now + 5s));
}
