/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "Captcha.h"
#include "CryptoRandom.h"
#include "Random.h"
#include "SecurityUtils.h"
#include "Util.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>

namespace Rooftree::Server
{
namespace
{
constexpr std::string_view Alphabet = "ACDEFHJKMNPRTUVWXY34679";

constexpr uint32 ImageWidth = 160;
constexpr uint32 ImageHeight = 56;

struct Point
{
    float X;
    float Y;
};

using Stroke = std::vector<Point>;

// The glyphs as strokes on a grid 4 wide and 6 high, y down
std::vector<Stroke> const& GetGlyph(char c)
{
    static std::unordered_map<char, std::vector<Stroke>> const glyphs =
    {
        { 'A', { { { 0, 6 }, { 2, 0 }, { 4, 6 } }, { { 1, 4 }, { 3, 4 } } } },
        { 'C', { { { 4, 1 }, { 3, 0 }, { 1, 0 }, { 0, 1.5f }, { 0, 4.5f }, { 1, 6 }, { 3, 6 }, { 4, 5 } } } },
        { 'D', { { { 0, 0 }, { 0, 6 }, { 2.5f, 6 }, { 4, 4.5f }, { 4, 1.5f }, { 2.5f, 0 }, { 0, 0 } } } },
        { 'E', { { { 4, 0 }, { 0, 0 }, { 0, 6 }, { 4, 6 } }, { { 0, 3 }, { 3, 3 } } } },
        { 'F', { { { 4, 0 }, { 0, 0 }, { 0, 6 } }, { { 0, 3 }, { 3, 3 } } } },
        { 'H', { { { 0, 0 }, { 0, 6 } }, { { 4, 0 }, { 4, 6 } }, { { 0, 3 }, { 4, 3 } } } },
        { 'J', { { { 4, 0 }, { 4, 4.5f }, { 3, 6 }, { 1, 6 }, { 0, 4.5f } } } },
        { 'K', { { { 0, 0 }, { 0, 6 } }, { { 4, 0 }, { 0, 3.5f } }, { { 1.3f, 2.6f }, { 4, 6 } } } },
        { 'M', { { { 0, 6 }, { 0, 0 }, { 2, 3.5f }, { 4, 0 }, { 4, 6 } } } },
        { 'N', { { { 0, 6 }, { 0, 0 }, { 4, 6 }, { 4, 0 } } } },
        { 'P', { { { 0, 6 }, { 0, 0 }, { 3, 0 }, { 4, 1 }, { 4, 2 }, { 3, 3 }, { 0, 3 } } } },
        { 'R', { { { 0, 6 }, { 0, 0 }, { 3, 0 }, { 4, 1 }, { 4, 2 }, { 3, 3 }, { 0, 3 } }, { { 2, 3 }, { 4, 6 } } } },
        { 'T', { { { 0, 0 }, { 4, 0 } }, { { 2, 0 }, { 2, 6 } } } },
        { 'U', { { { 0, 0 }, { 0, 4.5f }, { 1, 6 }, { 3, 6 }, { 4, 4.5f }, { 4, 0 } } } },
        { 'V', { { { 0, 0 }, { 2, 6 }, { 4, 0 } } } },
        { 'W', { { { 0, 0 }, { 1, 6 }, { 2, 2.5f }, { 3, 6 }, { 4, 0 } } } },
        { 'X', { { { 0, 0 }, { 4, 6 } }, { { 4, 0 }, { 0, 6 } } } },
        { 'Y', { { { 0, 0 }, { 2, 3 } }, { { 4, 0 }, { 2, 3 }, { 2, 6 } } } },
        { '3', { { { 0, 1 }, { 1, 0 }, { 3, 0 }, { 4, 1 }, { 4, 2 }, { 3, 3 }, { 1.5f, 3 } },
                 { { 3, 3 }, { 4, 4 }, { 4, 5 }, { 3, 6 }, { 1, 6 }, { 0, 5 } } } },
        { '4', { { { 2.5f, 0 }, { 0, 4 }, { 4, 4 } }, { { 3, 2 }, { 3, 6 } } } },
        { '6', { { { 3.5f, 0 }, { 1.5f, 0 }, { 0, 2 }, { 0, 5 }, { 1, 6 }, { 3, 6 }, { 4, 5 }, { 4, 4 }, { 3, 3 }, { 1, 3 }, { 0, 4 } } } },
        { '7', { { { 0, 0 }, { 4, 0 }, { 1.5f, 6 } } } },
        { '9', { { { 4, 2 }, { 3, 3 }, { 1, 3 }, { 0, 2 }, { 0, 1 }, { 1, 0 }, { 3, 0 }, { 4, 1 }, { 4, 4 }, { 2.5f, 6 }, { 0.5f, 6 } } } },
    };
    return glyphs.at(c);
}

// How much of every pixel is ink, 0 to 1
class Canvas
{
public:
    Canvas(uint32 width, uint32 height) : _width(width), _height(height), _ink(width * height, 0.0f) { }

    // A line width wide with soft edges
    void Line(Point a, Point b, float width)
    {
        float half = width / 2.0f;
        int32 minX = std::max(0, int32(std::floor(std::min(a.X, b.X) - half - 1)));
        int32 maxX = std::min(int32(_width) - 1, int32(std::ceil(std::max(a.X, b.X) + half + 1)));
        int32 minY = std::max(0, int32(std::floor(std::min(a.Y, b.Y) - half - 1)));
        int32 maxY = std::min(int32(_height) - 1, int32(std::ceil(std::max(a.Y, b.Y) + half + 1)));

        float dx = b.X - a.X;
        float dy = b.Y - a.Y;
        float length2 = dx * dx + dy * dy;
        for (int32 y = minY; y <= maxY; ++y)
        {
            for (int32 x = minX; x <= maxX; ++x)
            {
                float px = float(x) + 0.5f - a.X;
                float py = float(y) + 0.5f - a.Y;
                float t = length2 > 0.0f ? std::clamp((px * dx + py * dy) / length2, 0.0f, 1.0f) : 0.0f;
                float ex = px - t * dx;
                float ey = py - t * dy;
                float ink = std::clamp(half + 0.5f - std::sqrt(ex * ex + ey * ey), 0.0f, 1.0f);
                float& pixel = _ink[y * _width + x];
                pixel = std::max(pixel, ink);
            }
        }
    }

    // A polyline, cut into short pieces so the wave can bend them
    void Polyline(Stroke const& points, float width, auto&& warp)
    {
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            constexpr int32 Pieces = 6;
            Point previous = warp(points[i - 1]);
            for (int32 piece = 1; piece <= Pieces; ++piece)
            {
                float t = float(piece) / Pieces;
                Point next = warp({ points[i - 1].X + (points[i].X - points[i - 1].X) * t, points[i - 1].Y + (points[i].Y - points[i - 1].Y) * t });
                Line(previous, next, width);
                previous = next;
            }
        }
    }

    float At(uint32 x, uint32 y) const { return _ink[y * _width + x]; }

private:
    uint32 _width;
    uint32 _height;
    std::vector<float> _ink;
};

uint32 Crc32(std::span<uint8 const> data, uint32 crc = 0)
{
    static std::array<uint32, 256> const table = []
    {
        std::array<uint32, 256> values{};
        for (uint32 n = 0; n < 256; ++n)
        {
            uint32 c = n;
            for (int32 k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            values[n] = c;
        }
        return values;
    }();

    crc = ~crc;
    for (uint8 byte : data)
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void PutUInt32(std::vector<uint8>& out, uint32 value)
{
    out.push_back(uint8(value >> 24));
    out.push_back(uint8(value >> 16));
    out.push_back(uint8(value >> 8));
    out.push_back(uint8(value));
}

void PutChunk(std::vector<uint8>& png, std::string_view type, std::span<uint8 const> data)
{
    PutUInt32(png, uint32(data.size()));
    std::size_t start = png.size();
    png.insert(png.end(), type.begin(), type.end());
    png.insert(png.end(), data.begin(), data.end());
    PutUInt32(png, Crc32(std::span(png).subspan(start)));
}

// An 8 bit grey PNG, its zlib stream made of stored blocks
std::vector<uint8> EncodePng(uint32 width, uint32 height, std::span<uint8 const> pixels)
{
    std::vector<uint8> raw;
    raw.reserve((width + 1) * height);
    for (uint32 y = 0; y < height; ++y)
    {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), pixels.begin() + y * width, pixels.begin() + (y + 1) * width);
    }

    std::vector<uint8> zlib = { 0x78, 0x01 };
    for (std::size_t offset = 0; offset < raw.size() || offset == 0;)
    {
        std::size_t size = std::min<std::size_t>(raw.size() - offset, 65535);
        bool last = offset + size == raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(uint8(size));
        zlib.push_back(uint8(size >> 8));
        zlib.push_back(uint8(~size));
        zlib.push_back(uint8(~size >> 8));
        zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + size);
        offset += size;
        if (last)
            break;
    }

    uint32 a = 1;
    uint32 b = 0;
    for (uint8 byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    PutUInt32(zlib, (b << 16) | a);

    std::vector<uint8> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    std::vector<uint8> header;
    PutUInt32(header, width);
    PutUInt32(header, height);
    header.insert(header.end(), { 8, 0, 0, 0, 0 }); // 8 bit grey, deflate, adaptive filters, no interlace
    PutChunk(png, "IHDR", header);
    PutChunk(png, "IDAT", zlib);
    PutChunk(png, "IEND", {});
    return png;
}
}

CaptchaImage GenerateCaptcha(std::size_t length)
{
    CaptchaImage image;
    image.Width = ImageWidth;
    image.Height = ImageHeight;

    // The answer from the crypto RNG; the drawing's randomness only has to look different every time
    std::vector<uint8> bytes(length);
    Crypto::GetRandomBytes(bytes.data(), bytes.size());
    for (uint8 byte : bytes)
        image.Answer += Alphabet[byte % Alphabet.size()];

    Canvas canvas(ImageWidth, ImageHeight);

    // One wave bends the whole picture, characters and lines alike
    float waveAmplitude = frand(1.5f, 3.0f);
    float wavePeriod = frand(50.0f, 90.0f);
    float wavePhase = frand(0.0f, 2.0f * std::numbers::pi_v<float>);
    auto wave = [&](Point p) -> Point
    {
        return { p.X + std::sin(p.Y / 9.0f + wavePhase) * 1.5f, p.Y + std::sin(p.X * 2.0f * std::numbers::pi_v<float> / wavePeriod + wavePhase) * waveAmplitude };
    };

    float cellWidth = float(ImageWidth - 16) / float(length);
    for (std::size_t i = 0; i < length; ++i)
    {
        float glyphHeight = frand(26.0f, 34.0f);
        float scaleY = glyphHeight / 6.0f;
        float scaleX = scaleY * frand(0.75f, 1.0f);
        float angle = frand(-0.3f, 0.3f);
        float shear = frand(-0.2f, 0.2f);
        float centerX = 8.0f + cellWidth * (float(i) + 0.5f) + frand(-3.0f, 3.0f);
        float centerY = float(ImageHeight) / 2.0f + frand(-4.0f, 4.0f);
        float width = frand(2.8f, 3.6f);
        float cosine = std::cos(angle);
        float sine = std::sin(angle);
        auto place = [&](Point p) -> Point
        {
            float x = (p.X - 2.0f) * scaleX;
            float y = (p.Y - 3.0f) * scaleY;
            x += shear * y;
            return wave({ centerX + x * cosine - y * sine, centerY + x * sine + y * cosine });
        };

        for (Stroke const& stroke : GetGlyph(image.Answer[i]))
            canvas.Polyline(stroke, width, place);
    }

    // Lines across, in the same ink and nearly as thick as the strokes
    for (int32 line = 0; line < 2; ++line)
    {
        Stroke points;
        float y = frand(12.0f, float(ImageHeight) - 12.0f);
        float slope = frand(-0.15f, 0.15f);
        float amplitude = frand(3.0f, 7.0f);
        float period = frand(40.0f, 120.0f);
        float phase = frand(0.0f, 2.0f * std::numbers::pi_v<float>);
        for (float x = -4.0f; x <= float(ImageWidth) + 4.0f; x += 8.0f)
            points.push_back({ x, y + slope * (x - float(ImageWidth) / 2.0f) + std::sin(x * 2.0f * std::numbers::pi_v<float> / period + phase) * amplitude });
        canvas.Polyline(points, frand(1.4f, 2.0f), wave);
    }

    // Dots
    for (int32 dot = 0, dots = int32(urand(25, 40)); dot < dots; ++dot)
    {
        Point p = { frand(0.0f, float(ImageWidth)), frand(0.0f, float(ImageHeight)) };
        canvas.Line(p, { p.X + frand(-1.5f, 1.5f), p.Y + frand(-1.5f, 1.5f) }, frand(1.2f, 2.2f));
    }

    // Ink dark on a light, grainy background
    std::vector<uint8> pixels(ImageWidth * ImageHeight);
    uint8 ink = uint8(urand(30, 70));
    for (uint32 y = 0; y < ImageHeight; ++y)
    {
        for (uint32 x = 0; x < ImageWidth; ++x)
        {
            float background = 215.0f + 25.0f * float(x) / float(ImageWidth) + frand(-12.0f, 12.0f);
            float value = background + (float(ink) - background) * canvas.At(x, y);
            pixels[y * ImageWidth + x] = uint8(std::clamp(value, 0.0f, 255.0f));
        }
    }

    image.Png = EncodePng(ImageWidth, ImageHeight, pixels);
    return image;
}

std::string NormalizeCaptchaAnswer(std::string_view answer)
{
    std::string normalized;
    for (char c : answer)
        if (c != ' ' && c != '\t')
            normalized += Rooftree::CharToUpper(c);
    return normalized;
}

CaptchaStore::CaptchaStore(Seconds lifetime, std::size_t maxCount) : _lifetime(lifetime), _maxCount(std::max<std::size_t>(maxCount, 1))
{
}

CaptchaStore::Issued CaptchaStore::Issue(std::size_t length, TimePoint now)
{
    Cleanup(now);
    while (_captchas.size() >= _maxCount && !_order.empty())
    {
        _captchas.erase(_order.front());
        _order.pop_front();
    }

    Issued issued{ .Id = GenerateToken(18), .Image = GenerateCaptcha(length) };
    _captchas[issued.Id] = { .Answer = issued.Image.Answer, .Expires = now + _lifetime };
    _order.push_back(issued.Id);
    return issued;
}

bool CaptchaStore::Check(std::string_view id, std::string_view answer, TimePoint now)
{
    auto itr = _captchas.find(std::string(id));
    if (itr == _captchas.end())
        return false;

    Entry entry = std::move(itr->second);
    _captchas.erase(itr);
    return now < entry.Expires && ConstantTimeEquals(NormalizeCaptchaAnswer(answer), entry.Answer);
}

void CaptchaStore::Cleanup(TimePoint now)
{
    while (!_order.empty())
    {
        auto itr = _captchas.find(_order.front());
        if (itr != _captchas.end() && now < itr->second.Expires)
            break;

        if (itr != _captchas.end())
            _captchas.erase(itr);
        _order.pop_front();
    }

    // Ids checked already wait in _order until they reach its front; don't let them pile up behind a
    // young one
    if (_order.size() > 2 * _captchas.size() + 64)
        std::erase_if(_order, [this](std::string const& id) { return !_captchas.contains(id); });
}
}
