import { create } from 'zustand'
import { persist } from 'zustand/middleware'

export type Theme = 'light' | 'dark' | 'system'

// localStorage key, also read by the inline script in root.tsx before the first paint
export const themeStorageKey = 'blog-theme'

interface ThemeState {
  theme: Theme
  setTheme: (theme: Theme) => void
}

export const useThemeStore = create<ThemeState>()(
  persist((set) => ({ theme: 'system', setTheme: (theme) => set({ theme }) }), { name: themeStorageKey }),
)

export function resolveTheme(theme: Theme): 'light' | 'dark' {
  if (theme !== 'system') return theme
  return typeof window !== 'undefined' && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

export function applyTheme(theme: Theme) {
  document.documentElement.classList.toggle('dark', resolveTheme(theme) === 'dark')
}

// Runs in <head> before React: no flash of the wrong theme on prerendered pages
export const themeScript = `(function(){try{var t=JSON.parse(localStorage.getItem('${themeStorageKey}')||'{}').state;t=t&&t.theme||'system';
if(t==='dark'||(t==='system'&&matchMedia('(prefers-color-scheme: dark)').matches))document.documentElement.classList.add('dark')}catch(e){}})()`

// Switches with the new theme growing as a circle from (x, y), the button clicked (app.css,
// ::view-transition-new). Plain switch where View Transitions aren't supported or motion is reduced
export function switchTheme(theme: Theme, x: number, y: number) {
  const { setTheme } = useThemeStore.getState()
  const changes = resolveTheme(theme) !== resolveTheme(useThemeStore.getState().theme)
  if (!changes || !document.startViewTransition || window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
    setTheme(theme)
    return
  }

  const style = document.documentElement.style
  style.setProperty('--reveal-x', `${x}px`)
  style.setProperty('--reveal-y', `${y}px`)
  style.setProperty('--reveal-r', `${Math.hypot(Math.max(x, innerWidth - x), Math.max(y, innerHeight - y))}px`)
  document.startViewTransition(() => {
    setTheme(theme)
    applyTheme(theme)
  })
}
