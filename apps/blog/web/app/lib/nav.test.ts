import { activeNav, mainNav, moreNav } from './nav'

const all = [...mainNav, ...moreNav]

test('the section of a page', () => {
  expect(activeNav('/zh', all)?.key).toBe('home')
  expect(activeNav('/en/', all)?.key).toBe('home')
  expect(activeNav('/zh/posts', all)?.key).toBe('posts')
  expect(activeNav('/zh/posts/hello', all)?.key).toBe('posts')
  expect(activeNav('/en/notes/12', all)?.key).toBe('notes')
  expect(activeNav('/en/friends', all)?.key).toBe('friends')
  // Not a prefix match of a longer word
  expect(activeNav('/zh/postscript', all)).toBeUndefined()
  expect(activeNav('/zh/login', all)).toBeUndefined()
})
