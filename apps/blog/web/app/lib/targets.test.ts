import { targetKind, targetPath } from './targets'

test('where a comment target is shown', () => {
  expect(targetPath('post:7', 'hello')).toBe('/posts/hello')
  expect(targetPath('note:3', null)).toBe('/notes/3')
  expect(targetPath('thinking:5', null)).toBe('/thinking#thinking-5')
  expect(targetPath('guestbook', null)).toBe('/guestbook')
  expect(targetKind('note:3')).toBe('note')
  expect(targetKind('guestbook')).toBe('guestbook')
})
