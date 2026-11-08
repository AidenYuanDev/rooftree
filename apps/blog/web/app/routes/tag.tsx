import { useTranslation } from 'react-i18next'
import { data } from 'react-router'
import type { Route } from './+types/tag'
import { NotFoundPage } from './not-found'
import { PostFeed } from '~/components/post-feed'
import { TagList } from '~/components/tag-list'
import { getI18n } from '~/i18n'
import { fetchPostList, fetchSiteCached, fetchTags } from '~/lib/content'
import { pageMeta } from '~/lib/meta'
import { placeholderSlug, requireLang } from '~/lib/route'

async function load(lang: string | undefined, slug: string) {
  const valid = requireLang(lang)
  if (slug === placeholderSlug) return { firstPage: { posts: [], nextCursor: null }, tags: [], tag: null }
  const [firstPage, tags, site] = await Promise.all([fetchPostList(valid, { tag: slug }), fetchTags(valid), fetchSiteCached(valid)])
  const tag = tags.find((t) => t.slug === slug)
  if (!tag) throw data(null, { status: 404 })
  return { firstPage, tags, tag, siteName: site.name }
}

export async function loader({ params }: Route.LoaderArgs) {
  return load(params.lang, params.tag)
}

export async function clientLoader({ params, serverLoader }: Route.ClientLoaderArgs) {
  try {
    return await serverLoader()
  } catch {
    return load(params.lang, params.tag)
  }
}
// With hydrate and no HydrateFallback, the prerender renders the page itself and hydration reuses its data
clientLoader.hydrate = true as const

export function meta({ params, loaderData }: Route.MetaArgs) {
  const lang = params.lang === 'en' ? 'en' : 'zh'
  if (!loaderData?.tag) return [{ title: '404' }, { name: 'robots', content: 'noindex' }]
  const name = lang === 'en' ? loaderData.tag.nameEn : loaderData.tag.nameZh
  const title = `${getI18n(lang).t('posts.taggedWith', { tag: name })} · ${loaderData.siteName}`
  return pageMeta({ lang, title, path: `/tags/${params.tag}` })
}

export default function TagPage({ loaderData, params }: Route.ComponentProps) {
  const { t, i18n } = useTranslation()
  if (!loaderData.tag) return <NotFoundPage />
  const name = i18n.language === 'en' ? loaderData.tag.nameEn : loaderData.tag.nameZh
  return (
    <div className="grid gap-10 md:grid-cols-[1fr_14rem]">
      <section>
        <h1 className="mb-6 text-2xl font-semibold tracking-tight">{t('posts.taggedWith', { tag: name })}</h1>
        <PostFeed key={params.tag} firstPage={loaderData.firstPage} tag={params.tag} />
      </section>
      <aside className="md:pt-12">
        <h2 className="mb-3 text-sm font-medium text-muted-foreground">{t('posts.allTags')}</h2>
        <TagList tags={loaderData.tags} current={params.tag} />
      </aside>
    </div>
  )
}
