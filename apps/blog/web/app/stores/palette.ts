import { create } from 'zustand'

// Whether the command palette (components/command-palette.tsx) is open
export const usePaletteStore = create<{ open: boolean; setOpen: (open: boolean) => void }>()((set) => ({
  open: false,
  setOpen: (open) => set({ open }),
}))
