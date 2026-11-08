import { PencilIcon } from 'lucide-react'
import { lazy, Suspense, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { data, Link, useNavigate, useSearchParams } from 'react-router'
import type { Route } from './+types/post'
import { NotFoundPage } from './not-found'
import { PostEditorsDialog } from '~/components/admin/post-editors'
import { Article } from '~/components/article/article'
import { LocalDate } from '~/components/local-date'
import { NotInLang } from '~/components/not-in-lang'
import { UserAvatar } from '~/components/user-avatar'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Skeleton } from '~/components/ui/skeleton'
import { fetchPost, fetchSiteCached } from '~/lib/content'
import { localePath } from '~/lib/lang'
import { renderMarkdown } from '~/lib/markdown.server'
import { pageMeta, publicOrigin } from '~/lib/meta'
import { useCurrentUser } from '~/lib/queries'
import { placeholderSlug, requireLang } from '~/lib/route'
import type { Lang, Post } from '~/lib/schemas'
import { readingMinutes } from '~/lib/text'
import { useContentVersion } from '~/stores/live'

// Loaded only when an editor opens the post for editing on its page
const PostInlineEditor = lazy(() =>
  import('~/components/editor/post-inline-editor').then((module) => ({ default: module.PostInlineEditor })),
)

// moved: the slug's new address, then post and html are null; notInLang: the languages of a post not
// in this one (never prerendered: only in the browser)
interface LoaderData {
  siteName?: string
  moved: string | null
  post: Post | null
  html: string | null
  notInLang?: Lang[]
}

// At build time: the post with its highlighted HTML. An old slug is prerendered too, as a pointer to
// the new address (the API followed its 301)
export async function loader({ params }: Route.LoaderArgs): Promise<LoaderData> {
  const lang = requireLang(params.lang)
  if (params.slug === placeholderSlug) return { moved: null, post: null, html: null }
  const post = await fetchPost(lang, params.slug)
  if (!post || 'notInLang' in post) throw data(null, { status: 404 })
  if (post.slug !== params.slug) return { moved: post.slug, post: null, html: null }
  return { moved: null, post, html: await renderMarkdown(post.body), siteName: (await fetchSiteCached(lang)).name }
}

// A post newer than the last build has no prerendered data: fetched from the API and rendered here,
// without the build's code highlighting
export async function clientLoader({ params, serverLoader }: Route.ClientLoaderArgs): Promise<LoaderData> {
  try {
    return await serverLoader()
  } catch {
    const lang = requireLang(params.lang)
    const post = await fetchPost(lang, params.slug)
    if (!post) throw data(null, { status: 404 })
    if ('notInLang' in post) return { moved: null, post: null, html: null, notInLang: post.notInLang }
    if (post.slug !== params.slug) return { moved: post.slug, post: null, html: null }
    return { moved: null, post, html: null }
  }
}
// With hydrate and no HydrateFallback, the prerender renders the page itself and hydration reuses its data
clientLoader.hydrate = true as const

export function meta({ params, loaderData }: Route.MetaArgs) {
  const lang: Lang = params.lang === 'en' ? 'en' : 'zh'
  if (!loaderData) return [{ title: '404' }]
  if (!loaderData.moved && !loaderData.post) return [{ title: '404' }, { name: 'robots', content: 'noindex' }]
  if (loaderData.moved || !loaderData.post) {
    return [
      { title: loaderData.moved ?? '404' },
      { tagName: 'link', rel: 'canonical', href: `${publicOrigin}/${lang}/posts/${loaderData.moved}` },
      { name: 'robots', content: 'noindex' },
      { httpEquiv: 'refresh', content: `0; url=/${lang}/posts/${loaderData.moved}` },
    ]
  }

  const { post } = loaderData
  return pageMeta({
    lang,
    title: post.title,
    description: post.summary || undefined,
    path: `/posts/${post.slug}`,
    languages: post.translations,
    image: post.cover || undefined,
    type: 'article',
  })
}

function Moved({ lang, slug }: { lang: Lang; slug: string }) {
  const { t } = useTranslation()
  const navigate = useNavigate()
  useEffect(() => {
    void navigate(localePath(lang, `/posts/${slug}`), { replace: true })
  }, [navigate, lang, slug])

  return (
    <div className="py-24 text-center">
      <p className="mb-4">{t('posts.moved')}</p>
      <Button asChild>
        <Link to={localePath(lang, `/posts/${slug}`)}>{t('posts.goThere')}</Link>
      </Button>
    </div>
  )
}

export default function PostPage({ loaderData, params }: Route.ComponentProps) {
  const { t } = useTranslation()
  const lang: Lang = params.lang === 'en' ? 'en' : 'zh'
  const version = useContentVersion(loaderData.post ? `post:${loaderData.post.id}` : null)
  const key = `${lang}:${params.slug}`
  const [live, setLive] = useState<{ key: string; data: LoaderData } | null>(null)

  // Republished (or taken back) while open: swapped in from the API, its Markdown rendered before
  // replacing what is on screen so the text doesn't collapse in between
  useEffect(() => {
    if (!version || !params.slug) return
    let active = true
    void (async () => {
      const fresh = await fetchPost(lang, params.slug)
      if (!active) return
      if (!fresh) return setLive({ key, data: { moved: null, post: null, html: null } })
      if ('notInLang' in fresh) return setLive({ key, data: { moved: null, post: null, html: null, notInLang: fresh.notInLang } })
      if (fresh.slug !== params.slug) return setLive({ key, data: { moved: fresh.slug, post: null, html: null } })
      const html =
        fresh.body === loaderData.post?.body && loaderData.html
          ? loaderData.html
          : (await import('~/lib/markdown-html')).markdownHtml(fresh.body)
      if (active) setLive({ key, data: { moved: null, post: fresh, html } })
    })().catch(() => {})
    return () => {
      active = false
    }
  }, [version, key, lang, params.slug, loaderData.post?.body, loaderData.html])

  const { moved, post, html, notInLang } = live?.key === key ? live.data : loaderData
  if (moved) return <Moved lang={lang} slug={moved} />
  if (notInLang) return <NotInLang message={t('posts.notInLang')} path={`/posts/${params.slug}`} translations={notInLang} />
  if (!post) return <NotFoundPage />
  return <PostArticle lang={lang} post={post} html={html} />
}

function PostArticle({ lang, post, html }: { lang: Lang; post: Post; html: string | null }) {
  const { t } = useTranslation()
  const user = useCurrentUser()
  const [searchParams, setSearchParams] = useSearchParams()
  const canEdit = !!user && (user.role === 'admin' || post.people.some((person) => person.id === user.id))
  const editing = canEdit && searchParams.get('edit') === '1'
  const openEdit = () =>
    setSearchParams(
      (params) => {
        params.set('edit', '1')
        return params
      },
      { preventScrollReset: true },
    )
  const closeEdit = () =>
    setSearchParams(
      (params) => {
        params.delete('edit')
        return params
      },
      { replace: true, preventScrollReset: true },
    )

  const header = (
    <>
      <h1 className="font-heading text-3xl leading-tight font-bold md:text-4xl" lang={post.lang}>
        {post.title}
      </h1>
      <div className="mt-4 flex flex-wrap items-center gap-x-4 gap-y-1 text-sm text-muted-foreground">
        <People lang={lang} post={post} />
        <LocalDate iso={post.publishedAt} lang={lang} />
        <span>{t('posts.readingTime', { minutes: readingMinutes(post.body) })}</span>
        {post.tags.map((tag) => (
          <Badge key={tag.slug} variant="secondary" asChild>
            <Link to={localePath(lang, `/tags/${tag.slug}`)}>{tag.name}</Link>
          </Badge>
        ))}
        {canEdit && (
          <span className="ml-auto flex items-center gap-1">
            <Button type="button" variant="outline" size="xs" onClick={openEdit}>
              <PencilIcon />
              {t('admin.edit')}
            </Button>
            <PostEditorsDialog postId={post.id} size="xs" />
          </span>
        )}
      </div>
    </>
  )

  const neighbour = (ref: Post['previous']) => ref && { title: ref.title, to: localePath(lang, `/posts/${ref.slug}`) }
  return (
    <Article
      title={post.title}
      lang={post.lang}
      header={header}
      cover={post.cover || undefined}
      html={html}
      markdown={post.body}
      previous={neighbour(post.previous)}
      next={neighbour(post.next)}
      comments={{ target: `post:${post.id}`, enabled: post.commentsEnabled, passages: true, ownerId: post.people[0]?.id }}
      editor={
        editing ? (
          <Suspense fallback={<Skeleton className="h-96 w-full" />}>
            <PostInlineEditor postId={post.id} lang={lang} onDone={closeEdit} />
          </Suspense>
        ) : undefined
      }
    />
  )
}

// Who wrote it, each a link to their page: its owner first, then its editors
function People({ lang, post }: { lang: Lang; post: Post }) {
  const { t } = useTranslation()
  if (post.people.length === 0) return <span>{t('posts.by', { author: post.author })}</span>
  return (
    <span className="flex items-center gap-2">
      <span className="flex -space-x-1.5">
        {post.people.map((person) => (
          <Link
            key={person.id}
            to={localePath(lang, `/u/${person.username}`)}
            title={person.displayName}
            className="rounded-full ring-2 ring-background"
          >
            <UserAvatar user={person} className="size-6 text-[0.6rem]" />
          </Link>
        ))}
      </span>
      <span>
        {post.people.map((person, index) => (
          <span key={person.id}>
            {index > 0 && (index === post.people.length - 1 ? t('posts.and') : t('posts.comma'))}
            <Link to={localePath(lang, `/u/${person.username}`)} className="hover:text-foreground hover:underline">
              {person.displayName}
            </Link>
          </span>
        ))}
      </span>
    </span>
  )
}
