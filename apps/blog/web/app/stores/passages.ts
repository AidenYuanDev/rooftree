import { create } from 'zustand'
import type { CommentAnchorInput } from '~/lib/schemas'

// The comments on passages of the post that is open (components/passages.tsx): which threads the side
// panel shows, which comment is chosen (its passage is highlighted), and what a new selection is for.
// The comment area below the post uses it too: a comment's quote opens its thread here
export type Picking =
  // A reply's own passage, instead of the one of what it answers
  | { kind: 'reply'; threadId: number; replyTo: { id: number; name: string } }
  // A comment's passage chosen again: its author's once the post lost it, a moderator's any time
  | { kind: 'reselect'; commentId: number }

interface PassagesState {
  // The threads in the panel: one marker's, which may be several on the same words
  open: number[] | null
  focused: number | null
  // A new thread being written on the selected passage
  pending: CommentAnchorInput | null
  picking: Picking | null
  // A reply's passage once chosen, by the comment it answers
  replyAnchor: { replyTo: number; anchor: CommentAnchorInput } | null

  openThreads: (threads: number[], focused?: number) => void
  focus: (comment: number) => void
  startNew: (anchor: CommentAnchorInput) => void
  // null: no longer picking
  startPicking: (picking: Picking | null) => void
  setReplyAnchor: (replyAnchor: PassagesState['replyAnchor']) => void
  close: () => void
}

export const usePassages = create<PassagesState>()((set) => ({
  open: null,
  focused: null,
  pending: null,
  picking: null,
  replyAnchor: null,
  openThreads: (threads, focused) => set({ open: threads, focused: focused ?? threads[0] ?? null, pending: null }),
  focus: (comment) => set({ focused: comment }),
  startNew: (anchor) => set({ pending: anchor, open: null, focused: null, picking: null }),
  startPicking: (picking) => set({ picking }),
  setReplyAnchor: (replyAnchor) => set({ replyAnchor, picking: null }),
  close: () => set({ open: null, focused: null, pending: null, picking: null, replyAnchor: null }),
}))
