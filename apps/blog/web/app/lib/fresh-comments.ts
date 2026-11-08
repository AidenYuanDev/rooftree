import { createContext, useEffect, useState } from 'react'
import type { Comment, CommentTarget } from './schemas'

const empty: ReadonlySet<number> = new Set()

// The comments others wrote while the list was open (the server said so, components/live.tsx): shown
// highlighted for a moment
export const FreshComments = createContext<ReadonlySet<number>>(empty)

// Of target's comments, those with a higher id than any shown before, not one's own; nothing on the
// first load (of another target too), nor for the older ones a next page brings
export function useFreshComments(target: CommentTarget, comments: Comment[] | undefined, pages: number, userId: number | undefined) {
  const [newest, setNewest] = useState<{ target: CommentTarget; id: number; pages: number } | null>(null)
  const [fresh, setFresh] = useState<ReadonlySet<number>>(empty)
  if (comments) {
    const top = Math.max(0, ...comments.map((comment) => comment.id))
    if (newest?.target !== target) {
      setNewest({ target, id: top, pages })
      if (fresh.size > 0) setFresh(empty)
    } else if (pages > newest.pages) {
      setNewest({ target, id: Math.max(newest.id, top), pages })
    } else if (top > newest.id || pages !== newest.pages) {
      if (top > newest.id) {
        const added = comments.filter((comment) => comment.id > newest.id && comment.author?.id !== userId).map((comment) => comment.id)
        if (added.length > 0) setFresh(new Set([...fresh, ...added]))
      }
      setNewest({ target, id: Math.max(newest.id, top), pages })
    }
  }
  useEffect(() => {
    if (fresh.size === 0) return
    const timer = setTimeout(() => setFresh(empty), 3500)
    return () => clearTimeout(timer)
  }, [fresh])
  return fresh
}
