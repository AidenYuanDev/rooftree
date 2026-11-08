import { create } from 'zustand'
import { persist } from 'zustand/middleware'
import type { Lang } from '~/lib/schemas'

// The editors' unsaved changes, kept in localStorage until saved: per post ("new" for one not created
// yet)
export interface PostDraft {
  slug: string
  status: 'draft' | 'published'
  originalLang: Lang
  cover: string
  commentsEnabled: boolean
  tags: string[]
  translations: Partial<Record<Lang, { title: string; summary: string; body: string }>>
  publishAt?: string
}

interface DraftState {
  // PostDraft
  drafts: Record<string, { draft: object; savedAt: number }>
  saveDraft: (key: string, draft: object) => void
  discardDraft: (key: string) => void
}

export const useDraftStore = create<DraftState>()(
  persist(
    (set) => ({
      drafts: {},
      saveDraft: (key, draft) => set((state) => ({ drafts: { ...state.drafts, [key]: { draft, savedAt: Date.now() } } })),
      discardDraft: (key) =>
        set((state) => {
          const { [key]: _, ...rest } = state.drafts
          return { drafts: rest }
        }),
    }),
    { name: 'blog-drafts' },
  ),
)
