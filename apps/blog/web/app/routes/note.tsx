import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { data } from 'react-router'
import type { Route } from './+types/note'
import { NotFoundPage } from './not-found'
import { Article } from '~/components/article/article'
import { LocalDate } from '~/components/local-date'
import { NotInLang } from '~/components/not-in-lang'
import { Badge } from '~/components/ui/badge'
import { fetchNote, fetchSiteCached } from '~/lib/content'
import { localePath } from '~/lib/lang'
import { renderMarkdown } from '~/lib/markdown.server'
import { pageMeta } from '~/lib/meta'
import { placeholderSlug, requireLang } from '~/lib/route'
import type { Lang, Note } from '~/lib/schemas'
import { readingMinutes } from '~/lib/text'
import { useContentVersion } from '~/stores/live'

// notInLang: the languages of a note not in this one (never prerendered: only in the browser)
interface LoaderData {
  siteName?: string
  note: Note | null
  html: string | null
  notInLang?: Lang[]
}

// At build time: the note with its highlighted HTML
export async function loader({ params }: Route.LoaderArgs): Promise<LoaderData> {
  const lang = requireLang(params.lang)
  if (params.id === placeholderSlug) return { note: null, html: null }
  const note = await fetchNote(lang, params.id)
  if (!note || 'notInLang' in note) throw data(null, { status: 404 })
  return { note, html: await renderMarkdown(note.body), siteName: (await fetchSiteCached(lang)).name }
}

// A note newer than the last build: from the API, rendered here
export async function clientLoader({ params, serverLoader }: Route.ClientLoaderArgs): Promise<LoaderData> {
  try {
    return await serverLoader()
  } catch {
    const note = await fetchNote(requireLang(params.lang), params.id)
    if (!note) throw data(null, { status: 404 })
    if ('notInLang' in note) return { note: null, html: null, notInLang: note.notInLang }
    return { note, html: null }
  }
}
// With hydrate and no HydrateFallback, the prerender renders the page itself and hydration reuses its data
clientLoader.hydrate = true as const

export function meta({ params, loaderData }: Route.MetaArgs) {
  const lang: Lang = params.lang === 'en' ? 'en' : 'zh'
  const note = loaderData?.note
  if (!note) return [{ title: '404' }, { name: 'robots', content: 'noindex' }]
  return pageMeta({
    lang,
    title: note.title,
    path: `/notes/${note.id}`,
    languages: note.translations,
    image: note.cover || undefined,
    type: 'article',
  })
}

export default function NotePage({ loaderData, params }: Route.ComponentProps) {
  const { t } = useTranslation()
  const lang: Lang = params.lang === 'en' ? 'en' : 'zh'
  const version = useContentVersion(loaderData.note ? `note:${loaderData.note.id}` : null)
  const key = `${lang}:${params.id}`
  const [live, setLive] = useState<{ key: string; data: LoaderData } | null>(null)

  useEffect(() => {
    if (!version || !params.id) return
    let active = true
    void (async () => {
      const fresh = await fetchNote(lang, params.id)
      if (!active) return
      if (!fresh) return setLive({ key, data: { note: null, html: null } })
      if ('notInLang' in fresh) return setLive({ key, data: { note: null, html: null, notInLang: fresh.notInLang } })
      const html =
        fresh.body === loaderData.note?.body && loaderData.html
          ? loaderData.html
          : (await import('~/lib/markdown-html')).markdownHtml(fresh.body)
      if (active) setLive({ key, data: { note: fresh, html } })
    })().catch(() => {})
    return () => {
      active = false
    }
  }, [version, key, lang, params.id, loaderData.note?.body, loaderData.html])

  const { note, html, notInLang } = live?.key === key ? live.data : loaderData
  if (notInLang) return <NotInLang message={t('notes.notInLang')} path={`/notes/${params.id}`} translations={notInLang} />
  if (!note) return <NotFoundPage />

  const header = (
    <>
      <p className="mb-2 font-mono text-sm text-muted-foreground">
        {t('nav.notes')} #{note.id}
      </p>
      <h1 className="font-heading text-3xl leading-tight font-bold md:text-4xl" lang={note.lang}>
        {note.title}
      </h1>
      <div className="mt-4 flex flex-wrap items-center gap-x-4 gap-y-2 text-sm text-muted-foreground">
        <LocalDate iso={note.publishedAt} lang={lang} />
        {note.mood && <Badge variant="secondary">{t('notes.mood', { mood: note.mood })}</Badge>}
        {note.weather && <Badge variant="secondary">{t('notes.weather', { weather: note.weather })}</Badge>}
        <span>{t('notes.words', { count: note.wordCount })}</span>
        <span>{t('posts.readingTime', { minutes: readingMinutes(note.body) })}</span>
      </div>
    </>
  )

  const neighbour = (ref: Note['previous']) => ref && { title: ref.title, to: localePath(lang, `/notes/${ref.id}`) }
  return (
    <Article
      title={note.title}
      lang={note.lang}
      header={header}
      cover={note.cover || undefined}
      html={html}
      markdown={note.body}
      previous={neighbour(note.previous)}
      next={neighbour(note.next)}
      comments={{ target: `note:${note.id}`, enabled: note.commentsEnabled }}
    />
  )
}
