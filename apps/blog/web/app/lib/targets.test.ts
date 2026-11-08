import { targetKind, targetPath } from './targets'

test('where a comment target is shown', () => {
  expect(targetPath('post:7', 'hello')).toBe('/posts/hello')
  expect(targetPath('guestbook', null)).toBe('/guestbook')
  expect(targetKind('guestbook')).toBe('guestbook')
})
