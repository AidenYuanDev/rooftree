import { create } from 'zustand'

interface ChatState {
  // The floating dock: open, and the chat it shows
  isOpen: boolean
  activeChannelId: string | null
  // The chat a reader has in front of them, in the dock or on the chat page: no mention toast for it
  viewingChannelId: string | null
  setOpen: (open: boolean) => void
  toggleOpen: () => void
  setActiveChannelId: (id: string | null) => void
  openChannel: (id: string) => void
  openDirectChat: (friendId: number, currentUserId: number) => void
  setViewing: (id: string | null) => void
}

export function makeDmChannelId(u1: number, u2: number): string {
  return `dm:${Math.min(u1, u2)}:${Math.max(u1, u2)}`
}

export const useChatStore = create<ChatState>()((set) => ({
  isOpen: false,
  activeChannelId: null,
  viewingChannelId: null,
  setOpen: (open) => set({ isOpen: open }),
  toggleOpen: () => set((state) => ({ isOpen: !state.isOpen })),
  setActiveChannelId: (id) => set({ activeChannelId: id }),
  openChannel: (id) => set({ isOpen: true, activeChannelId: id }),
  openDirectChat: (friendId, currentUserId) => {
    const dmId = makeDmChannelId(friendId, currentUserId)
    set({ isOpen: true, activeChannelId: dmId })
  },
  setViewing: (id) => set({ viewingChannelId: id }),
}))
