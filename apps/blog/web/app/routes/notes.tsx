import { useInfiniteQuery } from '@tanstack/react-query'
import { useTranslation } from 'react-i18next'
import type { Route } from './+types/notes'
import { NoteItem } from '~/components/note-item'
import { PageHeader } from '~/components/page-header'
import { Button } from '~/components/ui/button'
import { errorMessage, getI18n } from '~/i18n'
import { fetchNoteList, fetchSiteCached } from '~/lib/content'
import { useLang } from '~/lib/lang'
import { pageMeta } from '~/lib/meta'
import { requireLang } from '~/lib/route'

async function load(lang: string | undefined) {
  const valid = requireLang(lang)
  const [firstPage, site] = await Promise.all([fetchNoteList(valid), fetchSiteCached(valid)])
  return { firstPage, siteName: site.name }
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
  return pageMeta({ lang, title: `${getI18n(lang).t('nav.notes')} · ${loaderData?.siteName ?? ''}`, path: '/notes' })
}

export default function Notes({ loaderData }: Route.ComponentProps) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const feed = useInfiniteQuery({
    queryKey: ['notes', lang],
    queryFn: ({ pageParam }) => fetchNoteList(lang, pageParam),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.nextCursor,
    initialData: { pages: [loaderData.firstPage], pageParams: [null] },
    staleTime: Infinity,
  })
  const notes = feed.data.pages.flatMap((page) => page.notes)

  return (
    <div className="mx-auto max-w-3xl">
      <PageHeader title={t('nav.notes')} description={t('notes.description')} />
      {notes.length === 0 ? (
        <p className="py-12 text-center text-muted-foreground">{t('notes.empty')}</p>
      ) : (
        <ol className="space-y-4">
          {notes.map((note, i) => (
            <li key={note.id} className="rise-in" style={{ '--rise-index': Math.min(i, 8) } as React.CSSProperties}>
              <NoteItem note={note} />
            </li>
          ))}
        </ol>
      )}
      {feed.hasNextPage && (
        <div className="mt-6 flex justify-center">
          <Button variant="outline" onClick={() => void feed.fetchNextPage()} disabled={feed.isFetchingNextPage}>
            {feed.isFetchingNextPage ? t('posts.loading') : t('posts.loadMore')}
          </Button>
        </div>
      )}
      {feed.isFetchNextPageError && <p className="mt-4 text-center text-sm text-destructive">{errorMessage(i18n, feed.error)}</p>}
    </div>
  )
}
