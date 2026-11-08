import { useSyncExternalStore } from 'react'
import type { Lang } from '~/lib/schemas'

const subscribe = () => () => {}
const localTimeZone = () => Intl.DateTimeFormat().resolvedOptions().timeZone
const buildTimeZone = () => 'UTC'

// A date in the reader's time zone. Prerendered HTML is made at build time, which doesn't know the
// reader's zone: the server snapshot is UTC, hydration starts from it and then switches, so the
// hydrated markup always matches
export function LocalDate({ iso, lang }: { iso: string | null | undefined; lang: Lang }) {
  const timeZone = useSyncExternalStore(subscribe, localTimeZone, buildTimeZone)
  if (!iso) return null

  const text = new Intl.DateTimeFormat(lang === 'zh' ? 'zh-CN' : 'en-US', {
    year: 'numeric',
    month: 'long',
    day: 'numeric',
    timeZone,
  }).format(new Date(iso))
  return <time dateTime={iso}>{text}</time>
}
