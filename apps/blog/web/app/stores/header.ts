import { useEffect } from 'react'
import { create } from 'zustand'

// What a page shows in the header once its own title has scrolled away (posts)
interface HeaderState {
  title: string | null
  set: (title: string | null) => void
}

export const useHeaderStore = create<HeaderState>()((set) => ({ title: null, set: (title) => set({ title }) }))

export function useHeaderTitle(title: string | null) {
  const set = useHeaderStore((state) => state.set)
  useEffect(() => {
    set(title)
    return () => set(null)
  }, [title, set])
}
