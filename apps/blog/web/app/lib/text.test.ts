import { countWords, isSearchable, plainText, readingMinutes } from './text'

test('words: CJK characters one by one, other words by runs', () => {
  expect(countWords('你好，世界')).toBe(4)
  expect(countWords('Hello, world')).toBe(2)
  expect(countWords('用 C++ 写 game server')).toBe(5)
})

test('markdown syntax and code are not words', () => {
  expect(countWords('# 标题\n\n![图](/uploads/a.png) [链接](https://example.com)')).toBe(5)
  expect(countWords('前\n\n```cpp\nint main() { return 0; }\n```\n\n后 `inline code`')).toBe(2)
})

test('reading time', () => {
  expect(readingMinutes('短')).toBe(1)
  expect(readingMinutes('字'.repeat(4000))).toBe(10)
  expect(readingMinutes('word '.repeat(2200))).toBe(10)
})

test('plain excerpts', () => {
  expect(plainText('# 标题\n\n**粗体** 和 [链接](https://x) `code`\n\n```js\nx()\n```\n![图](/a.png)')).toBe('标题 粗体 和 链接 code')
  expect(plainText('一二三四五', 3)).toBe('一二三…')
})

test('searchable: a CJK character, or two characters', () => {
  expect(isSearchable('猫')).toBe(true)
  expect(isSearchable(' x ')).toBe(false)
  expect(isSearchable('C++')).toBe(true)
  expect(isSearchable('')).toBe(false)
})
