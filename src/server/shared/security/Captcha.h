/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A picture captcha for the requests that mail someone (registering, subscribing, a password reset),
 * so a script can't make the server mail any address it likes:
 *
 *  - GenerateCaptcha draws a few characters as strokes, each turned, sheared and scaled on its own,
 *    the whole picture bent by a wave, crossed by lines drawn like the characters and sprinkled with
 *    dots, and encodes it as a grey PNG. No font and no image library: the glyphs are polylines here,
 *    the PNG is written with stored (uncompressed) deflate blocks, about 8 KiB
 *  - CaptchaStore keeps what was issued, on one thread (the main thread): an id per picture, its
 *    answer and when it expires. A check uses the captcha up whether it was right or not, so every
 *    guess needs a new picture
 *
 * The characters leave out the ones easy to mistake for another (0/O, 1/I/L, 2/Z, 5/S, 8/B, 6/G);
 * answers are compared without case and spaces.
 */

#ifndef ROOFTREE_CAPTCHA_H
#define ROOFTREE_CAPTCHA_H

#include "Define.h"
#include "Duration.h"
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rooftree::Server
{
struct CaptchaImage
{
    std::string Answer;
    uint32 Width = 0;
    uint32 Height = 0;
    std::vector<uint8> Png;
};

CaptchaImage GenerateCaptcha(std::size_t length = 5);

// The answer as people type it: upper case, without spaces
std::string NormalizeCaptchaAnswer(std::string_view answer);

class CaptchaStore
{
public:
    // maxCount: past it a new captcha replaces the oldest, so asking for pictures can't eat memory
    explicit CaptchaStore(Seconds lifetime = 10min, std::size_t maxCount = 10000);

    struct Issued
    {
        std::string Id;
        CaptchaImage Image;
    };

    Issued Issue(std::size_t length = 5, TimePoint now = TimePoint::clock::now());

    // The id is known, not expired, and answer is its answer. Either way the captcha is used up
    bool Check(std::string_view id, std::string_view answer, TimePoint now = TimePoint::clock::now());

    // Every now and then: drops expired captchas
    void Cleanup(TimePoint now = TimePoint::clock::now());

    std::size_t GetCount() const { return _captchas.size(); }

    void SetLifetime(Seconds lifetime) { _lifetime = lifetime; }

private:
    struct Entry
    {
        std::string Answer;
        TimePoint Expires;
    };

    Seconds _lifetime;
    std::size_t _maxCount;
    std::unordered_map<std::string, Entry> _captchas;
    // Ids by age, for Cleanup and the oldest to replace; an id checked already is skipped
    std::deque<std::string> _order;
};
}

#endif // ROOFTREE_CAPTCHA_H
