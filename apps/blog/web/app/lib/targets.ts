import type { CommentTarget } from './schemas'

// Where a comment's target is shown, without the language: a post by its slug, a note by number,
// a thinking as an anchor in its list
export function targetPath(target: CommentTarget, postSlug: string | null): string {
  if (target === 'guestbook') return '/guestbook'
  const [kind, id] = target.split(':')
  if (kind === 'post') return `/posts/${postSlug ?? ''}`
  if (kind === 'note') return `/notes/${id}`
  return `/thinking#thinking-${id}`
}

export function targetKind(target: CommentTarget): 'post' | 'note' | 'thinking' | 'guestbook' {
  return target === 'guestbook' ? 'guestbook' : (target.split(':')[0] as 'post' | 'note' | 'thinking')
}
