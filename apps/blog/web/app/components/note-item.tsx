import { useSyncExternalStore } from 'react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { Badge } from '~/components/ui/badge'
import { localePath, useLang } from '~/lib/lang'
import type { NoteSummary } from '~/lib/schemas'
import { plainText } from '~/lib/text'

const subscribe = () => () => {}

// The day of a note as a diary shows it: the day large, month and year small. In the reader's time
// zone once hydrated, UTC in the prerendered page (like LocalDate)
function DiaryDate({ iso }: { iso: string }) {
  const lang = useLang()
  const timeZone = useSyncExternalStore(
    subscribe,
    () => Intl.DateTimeFormat().resolvedOptions().timeZone,
    () => 'UTC',
  )
  const date = new Date(iso)
  const locale = lang === 'zh' ? 'zh-CN' : 'en-US'
  const day = new Intl.DateTimeFormat(locale, { day: 'numeric', timeZone }).format(date).replace(/\D/g, '')
  const month = new Intl.DateTimeFormat('en-US', { year: 'numeric', month: '2-digit', timeZone })
    .formatToParts(date)
    .reduce((parts, part) => ({ ...parts, [part.type]: part.value }), {} as Record<string, string>)
  return (
    <time dateTime={iso} className="flex w-16 shrink-0 flex-col items-center">
      <span className="font-heading text-3xl leading-none font-bold">{day}</span>
      <span className="mt-1 font-mono text-xs text-muted-foreground">
        {month.year}.{month.month}
      </span>
    </time>
  )
}

export function NoteItem({ note }: { note: NoteSummary }) {
  const { t } = useTranslation()
  const lang = useLang()
  return (
    <Link
      to={localePath(lang, `/notes/${note.id}`)}
      className="group flex gap-4 rounded-2xl border bg-card/60 p-5 shadow-sm backdrop-blur-md transition hover:-translate-y-0.5 hover:border-primary/40"
    >
      <DiaryDate iso={note.publishedAt} />
      <div className="min-w-0 flex-1 border-l pl-4">
        <div className="flex flex-wrap items-center gap-2">
          <h2 className="font-heading text-lg font-bold group-hover:text-primary" lang={note.lang}>
            {note.title}
          </h2>
          <span className="text-xs text-muted-foreground">#{note.id}</span>
        </div>
        <div className="mt-1 flex flex-wrap gap-1.5 text-xs">
          {note.mood && <Badge variant="secondary">{t('notes.mood', { mood: note.mood })}</Badge>}
          {note.weather && <Badge variant="secondary">{t('notes.weather', { weather: note.weather })}</Badge>}
          {note.fallback && <Badge variant="outline">{t(`langs.${note.lang}`)}</Badge>}
          {note.commentCount > 0 && <span className="text-muted-foreground">{t('posts.comments', { count: note.commentCount })}</span>}
        </div>
        <p className="mt-2 line-clamp-2 text-sm text-muted-foreground" lang={note.lang}>
          {plainText(note.excerpt)}
        </p>
      </div>
    </Link>
  )
}
