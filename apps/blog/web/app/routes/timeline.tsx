import { FileTextIcon, MessageSquareQuoteIcon, NotebookPenIcon } from 'lucide-react'
import { useSyncExternalStore } from 'react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import type { Route } from './+types/timeline'
import { PageHeader } from '~/components/page-header'
import { getI18n } from '~/i18n'
import { fetchSiteCached, fetchTimeline } from '~/lib/content'
import { localePath, useLang } from '~/lib/lang'
import { pageMeta } from '~/lib/meta'
import { requireLang } from '~/lib/route'
import type { Lang, TimelineItem } from '~/lib/schemas'
import { plainText } from '~/lib/text'

async function load(lang: string | undefined) {
  const valid = requireLang(lang)
  const [items, site] = await Promise.all([fetchTimeline(valid), fetchSiteCached(valid)])
  return { items, siteName: site.name }
}

export async function loader({ params }: Route.LoaderArgs) {
  return load(params.lang)
}

export async function clientLoader({ params, serverLoader }: Route.ClientLoaderArgs) {
  try {
    return await serverLoader()
  } catch {
    return load(params.lang)
  }
}
// With hydrate and no HydrateFallback, the prerender renders the page itself and hydration reuses its data
clientLoader.hydrate = true as const

export function meta({ params, loaderData }: Route.MetaArgs) {
  const lang = params.lang === 'en' ? 'en' : 'zh'
  return pageMeta({ lang, title: `${getI18n(lang).t('nav.timeline')} · ${loaderData?.siteName ?? ''}`, path: '/timeline' })
}

const icons = { post: FileTextIcon, note: NotebookPenIcon, thinking: MessageSquareQuoteIcon }

function itemPath(lang: Lang, item: TimelineItem) {
  if (item.kind === 'post') return localePath(lang, `/posts/${item.slug}`)
  if (item.kind === 'note') return localePath(lang, `/notes/${item.id}`)
  return localePath(lang, `/thinking#thinking-${item.id}`)
}

const subscribe = () => () => {}

// Year groups and "10-20" dates in the reader's time zone once hydrated, UTC in the prerendered page
function useTimeZone() {
  return useSyncExternalStore(
    subscribe,
    () => Intl.DateTimeFormat().resolvedOptions().timeZone,
    () => 'UTC',
  )
}

export default function Timeline({ loaderData }: Route.ComponentProps) {
  const { t } = useTranslation()
  const lang = useLang()
  const timeZone = useTimeZone()
  const parts = (iso: string) => {
    const date = new Date(iso)
    const get = (option: Intl.DateTimeFormatOptions) => new Intl.DateTimeFormat('en-US', { ...option, timeZone }).format(date)
    return { year: get({ year: 'numeric' }), day: `${get({ month: '2-digit' })}-${get({ day: '2-digit' })}` }
  }

  const years = new Map<string, TimelineItem[]>()
  for (const item of loaderData.items) {
    const { year } = parts(item.publishedAt)
    years.set(year, [...(years.get(year) ?? []), item])
  }

  return (
    <div className="mx-auto max-w-3xl">
      <PageHeader title={t('nav.timeline')} description={t('timeline.description', { count: loaderData.items.length })} />
      {[...years].map(([year, items]) => (
        <section key={year} className="rise-in mb-10">
          <h2 className="mb-4 flex items-baseline gap-3 font-heading text-2xl font-bold">
            {year}
            <span className="text-sm font-normal text-muted-foreground">{t('timeline.count', { count: items.length })}</span>
          </h2>
          <ol className="relative ml-2 space-y-1 border-l pl-6">
            {items.map((item) => {
              const Icon = icons[item.kind]
              return (
                <li key={`${item.kind}-${item.id}`} className="relative">
                  <span className="absolute top-3 -left-[1.95rem] size-2.5 rounded-full border-2 border-background bg-primary" />
                  <Link
                    to={itemPath(lang, item)}
                    className="group flex items-center gap-3 rounded-lg px-2 py-2 transition-colors hover:bg-card/70"
                  >
                    <time dateTime={item.publishedAt} className="w-12 shrink-0 font-mono text-xs text-muted-foreground tabular-nums">
                      {parts(item.publishedAt).day}
                    </time>
                    <Icon className="size-4 shrink-0 text-muted-foreground" aria-label={t(`admin.on.${item.kind}`)} />
                    <span className="truncate group-hover:text-primary">
                      {item.kind === 'thinking' ? plainText(item.title, 60) : item.title}
                    </span>
                  </Link>
                </li>
              )
            })}
          </ol>
        </section>
      ))}
      {loaderData.items.length === 0 && <p className="py-12 text-center text-muted-foreground">{t('posts.empty')}</p>}
    </div>
  )
}
