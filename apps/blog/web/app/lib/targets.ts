import type { CommentTarget } from './schemas'

// Where a comment's target is shown, without the language: a post by its slug
export function targetPath(target: CommentTarget, postSlug: string | null): string {
  if (target === 'guestbook') return '/guestbook'
  return `/posts/${postSlug ?? ''}`
}

export function targetKind(target: CommentTarget): 'post' | 'guestbook' {
  return target === 'guestbook' ? 'guestbook' : 'post'
}
