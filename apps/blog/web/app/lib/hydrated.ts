import { useSyncExternalStore } from 'react'

const subscribe = () => () => {}

// False in the prerendered HTML and until hydration, true after. Submit buttons wait for it: a form
// submitted before React handles it would be sent by the browser itself
export function useHydrated(): boolean {
  return useSyncExternalStore(
    subscribe,
    () => true,
    () => false,
  )
}
