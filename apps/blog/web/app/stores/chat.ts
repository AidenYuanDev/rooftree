import { create } from 'zustand'

interface ChatState {
  isOpen: boolean
  activeChannelId: string | null
  setOpen: (open: boolean) => void
  toggleOpen: () => void
  setActiveChannelId: (id: string | null) => void
  openChannel: (id: string) => void
  openDirectChat: (friendId: number, currentUserId: number) => void
}

export function makeDmChannelId(u1: number, u2: number): string {
  return `dm:${Math.min(u1, u2)}:${Math.max(u1, u2)}`
}

export const useChatStore = create<ChatState>()((set) => ({
  isOpen: false,
  activeChannelId: null,
  setOpen: (open) => set({ isOpen: open }),
  toggleOpen: () => set((state) => ({ isOpen: !state.isOpen })),
  setActiveChannelId: (id) => set({ activeChannelId: id }),
  openChannel: (id) => set({ isOpen: true, activeChannelId: id }),
  openDirectChat: (friendId, currentUserId) => {
    const dmId = makeDmChannelId(friendId, currentUserId)
    set({ isOpen: true, activeChannelId: dmId })
  },
}))
