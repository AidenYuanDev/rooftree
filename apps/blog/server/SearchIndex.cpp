/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "SearchIndex.h"
#include "BlogDatabase.h"
#include "BlogTypes.h"
#include "Field.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "Validation.h"
#include <utf8.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <tuple>

namespace Blog
{
namespace
{
// A title's token weighs as much as this many of the body's
constexpr float TitleWeight = 3;
constexpr float Bm25K1 = 1.2f;
constexpr float Bm25B = 0.75f;
// What a longer word a search's word starts counts, and one it is a part of
constexpr float PrefixWeight = 0.5f;
constexpr float InfixWeight = 0.3f;
// What a word a letter away counts, per letter: below a longer word
constexpr float TypoWeight = 0.25f;
// At most this many words taken for a typo, the closest, and for a start, the shortest
constexpr std::size_t MaxTypos = 8;
// At most this many words a search's word starts or is a part of: the best, then the shortest
constexpr std::size_t MaxWords = 50;
// A tag named as searched: above any text's score
constexpr float TagScore = 1000;

// Full width ASCII as ASCII, ASCII letters in lower case
uint32 Normalize(uint32 c)
{
    if (c >= 0xFF01 && c <= 0xFF5E)
        c -= 0xFEE0;
    else if (c == 0x3000)
        c = ' ';
    if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    return c;
}

enum class CharKind
{
    Separator,
    Word,
    Cjk
};

CharKind Classify(uint32 c)
{
    if (IsCjk(c))
        return CharKind::Cjk;
    return IsWordCharacter(c) ? CharKind::Word : CharKind::Separator;
}

// The runs of word characters and of CJK in text, normalized: run(kind, its characters, each as UTF-8)
template<typename Callable>
void ForEachRun(std::string_view text, Callable&& run)
{
    std::vector<std::string> characters;
    CharKind kind = CharKind::Separator;
    auto end = [&]
    {
        if (!characters.empty())
            run(kind, characters);
        characters.clear();
    };

    auto it = text.begin();
    while (it != text.end())
    {
        uint32 c = 0;
        try
        {
            c = Normalize(utf8::next(it, text.end()));
        }
        catch (utf8::exception const&)
        {
            break;
        }

        CharKind next = Classify(c);
        if (next != kind)
        {
            end();
            kind = next;
        }
        if (kind != CharKind::Separator)
            utf8::append(c, std::back_inserter(characters.emplace_back()));
    }
    end();
}

std::string Join(std::vector<std::string> const& characters)
{
    std::string joined;
    for (std::string const& character : characters)
        joined += character;
    return joined;
}

// The text normalized as words are, without the spaces around: how tags compare
std::string Normalized(std::string_view text)
{
    std::string normalized;
    auto it = text.begin();
    while (it != text.end())
    {
        try
        {
            utf8::append(Normalize(utf8::next(it, text.end())), std::back_inserter(normalized));
        }
        catch (utf8::exception const&)
        {
            break;
        }
    }
    normalized.erase(0, normalized.find_first_not_of(" \t"));
    normalized.erase(normalized.find_last_not_of(" \t") + 1);
    return normalized;
}

// markdown without link and image targets: their addresses aren't the text
std::string StripLinkTargets(std::string_view markdown)
{
    std::string text;
    text.reserve(markdown.size());
    std::size_t i = 0;
    while (i < markdown.size())
    {
        if (markdown.substr(i, 2) == "](")
        {
            std::size_t end = markdown.find(')', i + 2);
            i = end == std::string_view::npos ? markdown.size() : end + 1;
            text += ' ';
        }
        else
            text += markdown[i++];
    }
    return text;
}

// The distinct pairs of neighbouring characters of a word, each as UTF-8
std::vector<std::string> CharacterPairs(std::string_view word)
{
    std::vector<std::size_t> starts;
    for (auto it = word.begin(); it != word.end(); utf8::unchecked::next(it))
        starts.push_back(std::size_t(it - word.begin()));
    starts.push_back(word.size());

    std::vector<std::string> pairs;
    for (std::size_t i = 0; i + 2 < starts.size(); ++i)
        pairs.emplace_back(word.substr(starts[i], starts[i + 2] - starts[i]));
    std::ranges::sort(pairs);
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

bool IsAsciiWord(std::string_view word)
{
    return !word.empty() && word[0] >= 'a' && word[0] <= 'z'
        && std::ranges::all_of(word, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

// Optimal string alignment distance (insertions, deletions, substitutions and swaps of neighbours,
// each once), or maxDistance + 1 as soon as it is known to be more
uint32 EditDistance(std::string_view a, std::string_view b, uint32 maxDistance)
{
    std::size_t const n = a.size();
    std::size_t const m = b.size();
    uint32 const over = maxDistance + 1;
    if ((n > m ? n - m : m - n) > maxDistance)
        return over;

    // Three rows of the table: two back (for swaps), the last and this one. Only the band of cells
    // within maxDistance of the diagonal can stay within it, the others are "over"
    std::vector<uint32> before(m + 1, over), last(m + 1, over), row(m + 1, over);
    for (std::size_t j = 0; j <= std::min<std::size_t>(m, maxDistance); ++j)
        last[j] = uint32(j);

    for (std::size_t i = 1; i <= n; ++i)
    {
        std::ranges::fill(row, over);
        if (i <= maxDistance)
            row[0] = uint32(i);

        uint32 best = row[0];
        std::size_t from = i > maxDistance ? i - maxDistance : 1;
        std::size_t to = std::min(m, i + maxDistance);
        for (std::size_t j = from; j <= to; ++j)
        {
            uint32 cost = a[i - 1] == b[j - 1] ? 0 : 1;
            uint32 value = std::min({ last[j] + 1, row[j - 1] + 1, last[j - 1] + cost });
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                value = std::min(value, before[j - 2] + 1);
            row[j] = std::min(value, over);
            best = std::min(best, row[j]);
        }

        if (best > maxDistance)
            return over;
        std::swap(before, last);
        std::swap(last, row);
    }
    return std::min(last[m], over);
}

template<typename Owner>
SearchText const& FindText(Owner const& owner, std::string_view lang)
{
    for (std::string_view wanted : { lang, std::string_view(owner.OriginalLang) })
        for (SearchText const& text : owner.Texts)
            if (text.Lang == wanted)
                return text;
    return owner.Texts.front();
}
}

SearchText const& SearchPost::GetText(std::string_view lang) const
{
    return FindText(*this, lang);
}

SearchIndex::SearchIndex(std::vector<SearchPost> posts, std::vector<SearchTag> const& tags)
    : _posts(std::move(posts))
{
    // Every version a document, in order: the postings come out ascending by document
    float totalLength = 0;
    auto addTexts = [&](uint32 owner, std::vector<SearchText>& texts)
    {
        for (uint32 version = 0; version < texts.size(); ++version)
        {
            SearchText& text = texts[version];
            uint32 document = uint32(_documents.size());
            uint32 position = 0;
            Add(document, text.Title, position);
            uint32 bodyStart = position;
            // The summary is searched as the body
            Add(document, text.Summary, position);
            Add(document, StripLinkTargets(text.Body), position);

            float length = TitleWeight * float(bodyStart) + float(position - bodyStart);
            _documents.push_back({ owner, version, bodyStart, length });
            totalLength += length;
            text.Body = {};
        }
    };

    for (uint32 i = 0; i < _posts.size(); ++i)
    {
        _postById[_posts[i].Id] = i;
        addTexts(i, _posts[i].Texts);
    }
    if (!_documents.empty())
        _averageLength = std::max(1.0f, totalLength / float(_documents.size()));

    // Built: no room to grow left (vectors double as they grow)
    for (auto& [term, postings] : _terms)
    {
        postings.Documents.shrink_to_fit();
        postings.Starts.shrink_to_fit();
        postings.Positions.shrink_to_fit();
    }
    _documents.shrink_to_fit();

    // The words, sorted: those a search's word starts are together, as are those of a first letter
    for (auto const& [term, postings] : _terms)
    {
        auto it = term.begin();
        if (Classify(utf8::unchecked::next(it)) == CharKind::Word)
            _words.push_back(term);
    }
    std::ranges::sort(_words);

    // And by their pairs of characters, for the words a fragment is in
    for (uint32 i = 0; i < _words.size(); ++i)
        for (std::string& pair : CharacterPairs(_words[i]))
            _wordPairs[std::move(pair)].push_back(i);
    for (auto& [pair, words] : _wordPairs)
        words.shrink_to_fit();

    // Tags by every name a search may give
    std::unordered_map<std::string, std::vector<uint32>> postsByTag;
    for (uint32 i = 0; i < _posts.size(); ++i)
        for (std::string const& slug : _posts[i].Tags)
            postsByTag[slug].push_back(i);
    for (SearchTag const& tag : tags)
    {
        auto tagged = postsByTag.find(tag.Slug);
        if (tagged == postsByTag.end())
            continue;
        for (std::string_view name : { tag.Slug, tag.NameZh, tag.NameEn })
        {
            std::string key = Normalized(name);
            if (key.empty())
                continue;
            std::vector<uint32>& found = _tagPosts[key];
            found.insert(found.end(), tagged->second.begin(), tagged->second.end());
            std::ranges::sort(found);
            found.erase(std::unique(found.begin(), found.end()), found.end());
        }
    }
}

void SearchIndex::Add(uint32 document, std::string_view text, uint32& position)
{
    auto add = [&](std::string term, uint32 at)
    {
        Postings& postings = _terms[std::move(term)];
        if (postings.Documents.empty() || postings.Documents.back() != document)
        {
            postings.Documents.push_back(document);
            postings.Starts.push_back(uint32(postings.Positions.size()));
        }
        postings.Positions.push_back(at);
    };

    ForEachRun(text, [&](CharKind kind, std::vector<std::string> const& characters)
    {
        if (kind == CharKind::Word)
        {
            add(Join(characters), position++);
            return;
        }

        for (std::size_t i = 0; i < characters.size(); ++i, ++position)
        {
            add(characters[i], position);
            if (i + 1 < characters.size())
                add(characters[i] + characters[i + 1], position);
        }
    });
}

bool SearchIndex::IsSearchable(std::string_view query)
{
    bool cjk = false;
    ForEachRun(query, [&](CharKind kind, std::vector<std::string> const&) { cjk = cjk || kind == CharKind::Cjk; });
    // Punctuation counts too: "C++" may be a tag
    return cjk || CountCharacters(Normalized(query)) >= 2;
}

std::span<uint32 const> SearchIndex::Postings::PositionsAt(std::size_t i) const
{
    uint32 end = i + 1 < Starts.size() ? Starts[i + 1] : uint32(Positions.size());
    return std::span(Positions).subspan(Starts[i], end - Starts[i]);
}

void SearchIndex::CountPositions(Postings const& postings, float weight, Matches& matches) const
{
    for (std::size_t i = 0; i < postings.Documents.size(); ++i)
    {
        std::span<uint32 const> positions = postings.PositionsAt(i);
        uint32 bodyStart = _documents[postings.Documents[i]].BodyStart;
        auto title = std::ranges::lower_bound(positions, bodyStart) - positions.begin();
        auto body = std::ptrdiff_t(positions.size()) - title;
        matches.Documents.push_back(postings.Documents[i]);
        matches.Counts.push_back(weight * (TitleWeight * float(title) + float(body)));
    }
}

SearchIndex::Matches SearchIndex::MatchWord(std::string const& word) const
{
    // The word, the longer ones it starts and those it is a part of (from two characters: "c" would be
    // in too many)
    std::vector<std::pair<std::string_view, float>> terms;
    bool partial = CountCharacters(word) >= 2;
    for (auto it = std::ranges::lower_bound(_words, std::string_view(word)); it != _words.end() && it->starts_with(word); ++it)
    {
        if (*it == word)
            terms.emplace_back(*it, 1);
        else if (partial)
            terms.emplace_back(*it, PrefixWeight);
    }
    if (partial)
        for (uint32 i : FindWordsContaining(word))
            if (!_words[i].starts_with(word))
                terms.emplace_back(_words[i], InfixWeight);
    if (terms.size() > MaxWords)
    {
        // The best, and of those the closest to what was typed: the shortest (the word itself first)
        std::ranges::stable_sort(terms, [](auto const& a, auto const& b)
        {
            return std::tuple(-a.second, a.first.size()) < std::tuple(-b.second, b.first.size());
        });
        terms.resize(MaxWords);
    }

    // Neither: a typo of words that are there?
    if (terms.empty())
        for (auto [typo, distance] : FindTypos(word))
            terms.emplace_back(typo, std::pow(TypoWeight, float(distance)));
    return MatchTerms(terms);
}

// The documents of several terms merged, each counting as weighed
SearchIndex::Matches SearchIndex::MatchTerms(std::vector<std::pair<std::string_view, float>> const& terms) const
{
    Matches matches;
    if (terms.size() == 1)
    {
        CountPositions(_terms.find(std::string(terms[0].first))->second, terms[0].second, matches);
        return matches;
    }

    std::unordered_map<uint32, float> merged;
    for (auto [term, weight] : terms)
    {
        Matches one;
        CountPositions(_terms.find(std::string(term))->second, weight, one);
        for (std::size_t i = 0; i < one.Documents.size(); ++i)
            merged[one.Documents[i]] += one.Counts[i];
    }

    std::vector<std::pair<uint32, float>> sorted(merged.begin(), merged.end());
    std::ranges::sort(sorted);
    for (auto [document, count] : sorted)
    {
        matches.Documents.push_back(document);
        matches.Counts.push_back(count);
    }
    return matches;
}

SearchIndex::Matches SearchIndex::MatchCjk(std::vector<std::string> const& characters) const
{
    Matches matches;
    if (characters.size() == 1)
    {
        if (auto it = _terms.find(characters[0]); it != _terms.end())
            CountPositions(it->second, 1, matches);
        return matches;
    }

    // The pairs, each one position after the one before
    std::vector<Postings const*> pairs;
    for (std::size_t i = 0; i + 1 < characters.size(); ++i)
    {
        auto it = _terms.find(characters[i] + characters[i + 1]);
        if (it == _terms.end())
            return matches;
        pairs.push_back(&it->second);
    }

    std::vector<std::size_t> cursors(pairs.size(), 0);
    std::vector<std::span<uint32 const>> others(pairs.size() - 1);
    for (std::size_t first = 0; first < pairs[0]->Documents.size(); ++first)
    {
        // The other pairs' positions in this document
        uint32 document = pairs[0]->Documents[first];
        bool inAll = true;
        for (std::size_t i = 1; i < pairs.size() && inAll; ++i)
        {
            std::vector<uint32> const& documents = pairs[i]->Documents;
            auto it = std::lower_bound(documents.begin() + std::ptrdiff_t(cursors[i]), documents.end(), document);
            cursors[i] = std::size_t(it - documents.begin());
            inAll = it != documents.end() && *it == document;
            if (inAll)
                others[i - 1] = pairs[i]->PositionsAt(cursors[i]);
        }
        if (!inAll)
            continue;

        // Where all of them follow one another
        uint32 bodyStart = _documents[document].BodyStart;
        float count = 0;
        for (uint32 position : pairs[0]->PositionsAt(first))
        {
            bool inRow = true;
            for (std::size_t i = 0; i < others.size() && inRow; ++i)
                inRow = std::ranges::binary_search(others[i], position + uint32(i + 1));
            if (inRow)
                count += position < bodyStart ? TitleWeight : 1;
        }
        if (count > 0)
        {
            matches.Documents.push_back(document);
            matches.Counts.push_back(count);
        }
    }
    return matches;
}

std::vector<uint32> SearchIndex::FindWordsContaining(std::string_view fragment) const
{
    // The words with all of the fragment's pairs, the rarest pair's first
    std::vector<std::vector<uint32> const*> lists;
    for (std::string const& pair : CharacterPairs(fragment))
    {
        auto it = _wordPairs.find(pair);
        if (it == _wordPairs.end())
            return {};
        lists.push_back(&it->second);
    }
    if (lists.empty())
        return {};
    std::ranges::sort(lists, {}, [](std::vector<uint32> const* list) { return list->size(); });

    // Then those with the pairs in a row: "engine" has "in" and "ng", not "ing"
    std::vector<uint32> found;
    for (uint32 word : *lists[0])
    {
        bool inAll = std::ranges::all_of(lists.begin() + 1, lists.end(), [word](std::vector<uint32> const* list)
        {
            return std::ranges::binary_search(*list, word);
        });
        if (inAll && _words[word].find(fragment) != std::string_view::npos)
            found.push_back(word);
    }
    return found;
}

std::vector<std::pair<std::string_view, uint32>> SearchIndex::FindTypos(std::string_view word) const
{
    std::vector<std::pair<std::string_view, uint32>> found;
    if (word.size() < 5 || !IsAsciiWord(word))
        return found;

    // The words of the same first letter, ASCII and of three letters or more
    uint32 maxDistance = word.size() <= 8 ? 1 : 2;
    auto from = std::ranges::lower_bound(_words, word.substr(0, 1));
    auto to = std::ranges::lower_bound(_words, std::string(1, char(word[0] + 1)));
    for (std::string_view candidate : std::ranges::subrange(from, to))
    {
        if (candidate.size() < 3 || !IsAsciiWord(candidate))
            continue;
        uint32 distance = EditDistance(word, candidate, maxDistance);
        if (distance <= maxDistance)
            found.emplace_back(candidate, distance);
    }

    // The closest, then the shortest: "netwrok" is "network" before "networks"
    std::ranges::sort(found, [](auto const& a, auto const& b)
    {
        return std::tuple(a.second, a.first.size(), a.first) < std::tuple(b.second, b.first.size(), b.first);
    });
    if (found.size() > MaxTypos)
        found.resize(MaxTypos);
    return found;
}

std::vector<SearchIndex::Hit> SearchIndex::Search(std::string_view query, std::string_view lang, std::size_t maxPosts) const
{
    auto textOf = [&](Document const& document) -> SearchText const&
    {
        return _posts[document.Owner].Texts[document.Version];
    };

    // The documents with each word of the search
    std::vector<Matches> words;
    ForEachRun(query, [&](CharKind kind, std::vector<std::string> const& characters)
    {
        words.push_back(kind == CharKind::Word ? MatchWord(Join(characters)) : MatchCjk(characters));
    });

    // The best version of each post
    std::unordered_map<uint32, Hit> postScores;
    bool everyWord = !words.empty() && std::ranges::none_of(words, [](Matches const& matches) { return matches.Documents.empty(); });
    if (everyWord)
    {
        // Rarest first: its documents are the most any intersection can have
        std::ranges::sort(words, [](Matches const& a, Matches const& b) { return a.Documents.size() < b.Documents.size(); });
        float total = float(_documents.size());
        std::vector<float> idf;
        for (Matches const& matches : words)
        {
            float frequency = float(matches.Documents.size());
            idf.push_back(std::log(1 + (total - frequency + 0.5f) / (frequency + 0.5f)));
        }

        std::vector<std::size_t> cursors(words.size(), 0);
        for (std::size_t first = 0; first < words[0].Documents.size(); ++first)
        {
            uint32 document = words[0].Documents[first];
            Document const& found = _documents[document];
            if (!lang.empty() && textOf(found).Lang != lang)
                continue;
            float norm = Bm25K1 * (1 - Bm25B + Bm25B * found.Length / _averageLength);
            auto bm25 = [&](std::size_t word, float count) { return idf[word] * count * (Bm25K1 + 1) / (count + norm); };

            float score = bm25(0, words[0].Counts[first]);
            bool inAll = true;
            for (std::size_t i = 1; i < words.size() && inAll; ++i)
            {
                std::vector<uint32> const& documents = words[i].Documents;
                auto it = std::lower_bound(documents.begin() + std::ptrdiff_t(cursors[i]), documents.end(), document);
                cursors[i] = std::size_t(it - documents.begin());
                inAll = it != documents.end() && *it == document;
                if (inAll)
                    score += bm25(i, words[i].Counts[cursors[i]]);
            }
            if (!inAll)
                continue;

            Hit& best = postScores[found.Owner];
            if (!best.Text || score > best.Score)
                best = { 0, score, &textOf(found) };
        }
    }

    // A tag named as searched: its posts first
    if (auto tagged = _tagPosts.find(Normalized(query)); tagged != _tagPosts.end())
    {
        for (uint32 post : tagged->second)
        {
            // Found by its words or not, in the language searched
            Hit& hit = postScores[post];
            if (!hit.Text && !lang.empty())
            {
                auto text = std::ranges::find(_posts[post].Texts, lang, &SearchText::Lang);
                if (text == _posts[post].Texts.end())
                {
                    postScores.erase(post);
                    continue;
                }
                hit.Text = &*text;
            }
            hit.Score += TagScore;
        }
    }

    std::vector<Hit> hits;
    for (auto const& [owner, hit] : postScores)
        hits.push_back({ _posts[owner].Id, hit.Score, hit.Text });
    // Ties: the newer (the higher id) first
    std::ranges::sort(hits, [](Hit const& a, Hit const& b) { return std::tuple(a.Score, a.Id) > std::tuple(b.Score, b.Id); });
    if (hits.size() > maxPosts)
        hits.resize(maxPosts);
    return hits;
}

SearchPost const* SearchIndex::FindPost(uint32 id) const
{
    auto it = _postById.find(id);
    return it == _postById.end() ? nullptr : &_posts[it->second];
}

std::unique_ptr<SearchIndex> SearchIndex::Load()
{
    // A row per language version, a post's rows together
    std::vector<SearchPost> posts;
    if (PreparedQueryResult result = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_SEARCH_POSTS)))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 id = fields[0].GetUInt32();
            if (posts.empty() || posts.back().Id != id)
            {
                SearchPost& post = posts.emplace_back();
                post.Id = id;
                post.Slug = fields[1].GetString();
                post.Cover = fields[2].GetString();
                post.PublishedAt = fields[3].GetDate();
                post.UpdatedAt = fields[4].GetDate();
                post.OriginalLang = fields[5].GetString();
                post.Tags = SplitList(fields[10].GetStringOrNull().value_or(""));
            }
            posts.back().Texts.push_back({ fields[6].GetString(), fields[7].GetString(), fields[8].GetString(), fields[9].GetString() });
        } while (result->NextRow());
    }

    std::vector<SearchTag> tags;
    if (PreparedQueryResult result = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_SEARCH_TAGS)))
    {
        do
        {
            Field* fields = result->Fetch();
            tags.push_back({ fields[0].GetString(), fields[1].GetString(), fields[2].GetString() });
        } while (result->NextRow());
    }

    return std::make_unique<SearchIndex>(std::move(posts), tags);
}
}
