// Length of a text the way readers count it: each CJK character is a word, other words are runs of
// letters and digits. Code and link targets are left out. The server counts the same way for the
// home page's total (CountWords, server/Validation.cpp)
export function countWords(markdown: string): number {
  const text = markdown
    .replace(/```[\s\S]*?```/g, ' ')
    .replace(/`[^`]*`/g, ' ')
    .replace(/\]\([^)]*\)/g, ' ')
  const cjk = text.match(/[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\p{Script=Hangul}]/gu)?.length ?? 0
  const words =
    text.replace(/[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\p{Script=Hangul}]/gu, ' ').match(/[\p{L}\p{N}]+/gu)?.length ?? 0
  return cjk + words
}

// Something the search looks for: a CJK character, or two characters (the server checks the same,
// SearchIndex::IsSearchable)
export function isSearchable(query: string): boolean {
  const trimmed = query.trim()
  return /[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\p{Script=Hangul}]/u.test(trimmed) || [...trimmed].length >= 2
}

// Minutes to read: about 400 CJK characters or 220 English words a minute
export function readingMinutes(markdown: string): number {
  const cjk = markdown.match(/\p{Script=Han}/gu)?.length ?? 0
  const other = countWords(markdown) - cjk
  return Math.max(1, Math.round(cjk / 400 + other / 220))
}

// A plain excerpt of markdown: no syntax, no code, no images, at most max characters
export function plainText(markdown: string, max = 160): string {
  const text = markdown
    .replace(/```[\s\S]*?(```|$)/g, ' ')
    .replace(/!\[[^\]]*\]\([^)]*\)/g, ' ')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/`([^`]*)`/g, '$1')
    .replace(/^\s{0,3}(#{1,6}|>|[-*+]|\d+\.)\s+/gm, '')
    .replace(/[*_~]{1,3}([^*_~]+)[*_~]{1,3}/g, '$1')
    .replace(/\s+/g, ' ')
    .trim()
  const characters = [...text]
  return characters.length > max ? `${characters.slice(0, max).join('').trimEnd()}…` : text
}
