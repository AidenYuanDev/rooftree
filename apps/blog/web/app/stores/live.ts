import { useEffect } from 'react'
import { create } from 'zustand'

// What the WebSocket (components/live.tsx) last heard: readers of this page and of the whole site.
// null until it's connected; pages show nothing then.
// And what the page shows that others may change while it's open ("post:12", "guestbook"): each
// part of the page watching one says so (useWatch), the socket tells the server, and when the
// server says a post's text changed its version here goes up (useContentVersion)
interface LiveState {
  page: number | null
  site: number | null
  set: (counts: { page: number | null; site: number | null }) => void
  // By target, how many parts of the page watch it
  watching: Record<string, number>
  watch: (target: string) => () => void
  // By target, how often its text changed since the page opened
  content: Record<string, number>
  contentChanged: (target: string) => void
}

export const useLiveStore = create<LiveState>()((set) => ({
  page: null,
  site: null,
  set: (counts) => set(counts),
  watching: {},
  watch: (target) => {
    set((state) => ({ watching: { ...state.watching, [target]: (state.watching[target] ?? 0) + 1 } }))
    return () =>
      set((state) => {
        const { [target]: count = 1, ...others } = state.watching
        return { watching: count > 1 ? { ...others, [target]: count - 1 } : others }
      })
  },
  content: {},
  contentChanged: (target) => set((state) => ({ content: { ...state.content, [target]: (state.content[target] ?? 0) + 1 } })),
}))

// While mounted: the server tells this page when target changes
export function useWatch(target: string | null | undefined) {
  const watch = useLiveStore((state) => state.watch)
  useEffect(() => (target ? watch(target) : undefined), [watch, target])
}

// 0, then one more each time the server says target's text changed
export function useContentVersion(target: string | null | undefined) {
  useWatch(target)
  return useLiveStore((state) => (target ? (state.content[target] ?? 0) : 0))
}
