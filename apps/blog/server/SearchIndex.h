/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The search, in memory: an inverted index of everything published, posts and notes, every language
 * version of each (a document per version). Built once from the database (Load, a worker or startup),
 * then only read, from any thread; BlogWorld swaps in a new one after a change (BlogWorld.h).
 *
 * Tokens: words of ASCII letters and digits (or letters of other scripts) in lower case, and CJK as
 * every character and every pair of neighbours, each at the position of its first character. Spaces
 * and punctuation of any width split; full width ASCII counts as ASCII ("Ｃ＋＋" is "c++").
 *
 * A search finds the documents with all of its words: a word as itself, as the start of longer ones
 * ("ep" is also "epoll": what one is still typing) and as a part of others ("fd" is also "epfd"), each
 * scored lower, found through the words' pairs of characters (not the text's: every distinct word
 * once, so it stays small and quick however many there are), CJK of one character as that
 * character, longer CJK as its pairs at consecutive positions (the characters in a row, not apart). A
 * word of five letters or more that neither is nor starts any word is taken as a typo: the words a
 * letter away (two from nine letters on, swapping neighbours counts once) with the same first letter,
 * scored lower still.
 * Scores are BM25 with the title weighing three times the body; a search equal to a tag's name or
 * slug finds that tag's posts first.
 * A search is in one language, its versions only (a post or note without one is not found), or in
 * all: each found in the version that matched best.
 */

#ifndef BLOG_SEARCH_INDEX_H
#define BLOG_SEARCH_INDEX_H

#include "Define.h"
#include "Duration.h"
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Blog
{
// What a result shows, and while building, what is indexed
struct SearchText
{
    std::string Lang;
    std::string Title;
    std::string Summary;            // posts: their summary; notes: the start of the body
    std::string Body;               // markdown, cleared once indexed
};

struct SearchPost
{
    uint32 Id = 0;
    std::string Slug;
    std::string Cover;
    std::string OriginalLang;
    SystemTimePoint PublishedAt;
    SystemTimePoint UpdatedAt;
    std::vector<std::string> Tags;  // slugs
    std::vector<SearchText> Texts;  // by lang

    // The version in lang, else the original
    SearchText const& GetText(std::string_view lang) const;
};

struct SearchNote
{
    uint32 Id = 0;
    std::string OriginalLang;
    SystemTimePoint PublishedAt;
    std::vector<SearchText> Texts;

    SearchText const& GetText(std::string_view lang) const;
};

struct SearchTag
{
    std::string Slug;
    std::string NameZh;
    std::string NameEn;
};

class SearchIndex
{
public:
    struct Hit
    {
        uint32 Id = 0;
        float Score = 0;
        SearchText const* Text = nullptr;   // the version found
    };

    struct Results
    {
        std::vector<Hit> Posts;
        std::vector<Hit> Notes;
    };

    SearchIndex(std::vector<SearchPost> posts, std::vector<SearchNote> notes, std::vector<SearchTag> const& tags);

    // Everything published, with blocking queries: a worker, or startup
    static std::unique_ptr<SearchIndex> Load();

    // Something to look for: a CJK character, or two characters of words
    static bool IsSearchable(std::string_view query);

    // Best first, at most maxPosts and maxNotes; in the versions in lang, or with no lang in all
    Results Search(std::string_view query, std::string_view lang, std::size_t maxPosts, std::size_t maxNotes) const;

    SearchPost const* FindPost(uint32 id) const;
    SearchNote const* FindNote(uint32 id) const;
    std::size_t GetDocumentCount() const { return _documents.size(); }

private:
    struct Document
    {
        bool IsNote = false;
        uint32 Owner = 0;           // index into _posts or _notes
        uint32 Version = 0;         // index into its Texts
        uint32 BodyStart = 0;       // positions from here on are the body's
        float Length = 0;           // three times the title's tokens plus the body's
    };

    // Where a term is: the documents with it, ascending, and in each its positions, ascending. Three
    // arrays, not a vector per document: a few million small vectors would weigh more than the
    // positions themselves
    struct Postings
    {
        std::vector<uint32> Documents;
        std::vector<uint32> Starts;     // Documents[i]'s positions start at Positions[Starts[i]]
        std::vector<uint32> Positions;

        std::span<uint32 const> PositionsAt(std::size_t i) const;
    };

    // The documents of one word of the search, ascending, with its weighted count in each
    struct Matches
    {
        std::vector<uint32> Documents;
        std::vector<float> Counts;
    };

    void Add(uint32 document, std::string_view text, uint32& position);
    Matches MatchWord(std::string const& word) const;
    Matches MatchTerms(std::vector<std::pair<std::string_view, float>> const& terms) const;
    Matches MatchCjk(std::vector<std::string> const& characters) const;
    void CountPositions(Postings const& postings, float weight, Matches& matches) const;
    std::vector<std::pair<std::string_view, uint32>> FindTypos(std::string_view word) const;
    std::vector<uint32> FindWordsContaining(std::string_view fragment) const;

    std::vector<SearchPost> _posts;
    std::vector<SearchNote> _notes;
    std::unordered_map<uint32, uint32> _postById;
    std::unordered_map<uint32, uint32> _noteById;

    std::vector<Document> _documents;
    std::unordered_map<std::string, Postings> _terms;
    std::vector<std::string_view> _words;                           // the keys of _terms that are words, sorted
    std::unordered_map<std::string, std::vector<uint32>> _wordPairs; // two characters: indexes into _words with them, ascending
    std::unordered_map<std::string, std::vector<uint32>> _tagPosts;   // a tag's names and slug, normalized: indexes into _posts
    float _averageLength = 1;
};
}

#endif // BLOG_SEARCH_INDEX_H
