import { useInfiniteQuery, useQueryClient } from '@tanstack/react-query'
import { MessageCircleIcon } from 'lucide-react'
import { lazy, Suspense, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import type { Route } from './+types/thinking'
import { CottageMark } from '~/components/scenery/mark'
import { Comments } from '~/components/comments'
import { Reactions, ReactionsProvider } from '~/components/reactions'
import { LocalDate } from '~/components/local-date'
import { PageHeader } from '~/components/page-header'
import { Button } from '~/components/ui/button'
import { errorMessage, getI18n } from '~/i18n'
import { fetchSiteCached, fetchThinkingList } from '~/lib/content'
import { useLang } from '~/lib/lang'
import { renderMarkdown } from '~/lib/markdown.server'
import { pageMeta } from '~/lib/meta'
import { requireLang } from '~/lib/route'
import type { Thinking } from '~/lib/schemas'
import { useContentVersion } from '~/stores/live'

// Thinking newer than the build, and the pages after the first one, are rendered in the browser
const Markdown = lazy(() => import('~/components/markdown').then((module) => ({ default: module.Markdown })))

async function load(lang: string | undefined) {
  const valid = requireLang(lang)
  const [firstPage, site] = await Promise.all([fetchThinkingList(valid), fetchSiteCached(valid)])
  return { firstPage, html: {} as Record<number, string>, siteName: site.name, author: site.author }
}

// At build time, the first page with its HTML. Only here: renderMarkdown is server-only
export async function loader({ params }: Route.LoaderArgs) {
  const page = await load(params.lang)
  for (const item of page.firstPage.thinking) page.html[item.id] = await renderMarkdown(item.body)
  return page
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
  return pageMeta({ lang, title: `${getI18n(lang).t('nav.thinking')} · ${loaderData?.siteName ?? ''}`, path: '/thinking' })
}

function ThinkingItem({ item, html, author }: { item: Thinking; html: string | undefined; author: string }) {
  const { t } = useTranslation()
  const lang = useLang()
  const [open, setOpen] = useState(false)
  return (
    <article id={`thinking-${item.id}`} className="flex scroll-mt-24 gap-3">
      <div className="mt-1 flex size-9 shrink-0 items-center justify-center rounded-full border bg-card">
        <CottageMark className="size-5" />
      </div>
      <div className="min-w-0 flex-1">
        <div className="mb-1 flex items-baseline gap-2 text-sm">
          <span className="font-medium">{author}</span>
          <a href={`#thinking-${item.id}`} className="text-xs text-muted-foreground hover:underline">
            <LocalDate iso={item.publishedAt} lang={lang} />
          </a>
        </div>
        <div className="rounded-2xl rounded-tl-sm border bg-card/70 px-5 py-4 shadow-sm backdrop-blur-md">
          {html ? (
            <div className="prose max-w-none prose-night prose-p:my-1" lang={item.lang} dangerouslySetInnerHTML={{ __html: html }} />
          ) : (
            <Suspense fallback={<p lang={item.lang}>{item.body}</p>}>
              <Markdown source={item.body} className="prose-p:my-1" />
            </Suspense>
          )}
        </div>
        <div className="mt-1 flex flex-wrap items-center gap-1">
          <Reactions target={`thinking:${item.id}`} />
          {item.commentsEnabled && (
            <Button variant="ghost" size="xs" className="text-muted-foreground" aria-expanded={open} onClick={() => setOpen(!open)}>
              <MessageCircleIcon />
              {item.commentCount > 0 ? t('posts.comments', { count: item.commentCount }) : t('thinking.reply')}
            </Button>
          )}
        </div>
        {open && (
          <div className="mt-2 rounded-2xl border bg-paper p-4 backdrop-blur-md">
            <Comments target={`thinking:${item.id}`} />
          </div>
        )}
      </div>
    </article>
  )
}

export default function ThinkingPage({ loaderData }: Route.ComponentProps) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const queryClient = useQueryClient()
  const version = useContentVersion('thinking')
  useEffect(() => {
    if (version) void queryClient.invalidateQueries({ queryKey: ['thinking', lang] })
  }, [version, lang, queryClient])
  const feed = useInfiniteQuery({
    queryKey: ['thinking', lang],
    queryFn: ({ pageParam }) => fetchThinkingList(lang, pageParam),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.nextCursor,
    initialData: { pages: [loaderData.firstPage], pageParams: [null] },
    staleTime: Infinity,
  })
  const items = feed.data.pages.flatMap((page) => page.thinking)
  const initialBodies = new Map(loaderData.firstPage.thinking.map((item) => [item.id, item.body]))

  return (
    <div className="mx-auto max-w-2xl">
      <PageHeader title={t('nav.thinking')} description={t('thinking.description')} />
      {items.length === 0 && <p className="py-12 text-center text-muted-foreground">{t('thinking.empty')}</p>}
      <ReactionsProvider targets={items.map((item) => `thinking:${item.id}`)}>
        <div className="space-y-8">
          {items.map((item, i) => (
            <div key={item.id} className="rise-in" style={{ '--rise-index': Math.min(i, 8) } as React.CSSProperties}>
              <ThinkingItem
                item={item}
                html={initialBodies.get(item.id) === item.body ? loaderData.html[item.id] : undefined}
                author={loaderData.author}
              />
            </div>
          ))}
        </div>
      </ReactionsProvider>
      {feed.hasNextPage && (
        <div className="mt-8 flex justify-center">
          <Button variant="outline" onClick={() => void feed.fetchNextPage()} disabled={feed.isFetchingNextPage}>
            {feed.isFetchingNextPage ? t('posts.loading') : t('posts.loadMore')}
          </Button>
        </div>
      )}
      {feed.isFetchNextPageError && <p className="mt-4 text-center text-sm text-destructive">{errorMessage(i18n, feed.error)}</p>}
    </div>
  )
}
