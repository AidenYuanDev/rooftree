import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import type { Route } from './+types/sitemap'
import { PageHeader } from '~/components/page-header'
import { TagList } from '~/components/tag-list'
import { getI18n } from '~/i18n'
import { fetchSiteCached, fetchTags, fetchTimeline } from '~/lib/content'
import { localePath, useLang } from '~/lib/lang'
import { pageMeta } from '~/lib/meta'
import { requireLang } from '~/lib/route'

// Every page for readers, by section; /sitemap.xml is the crawlers' one

async function load(lang: string | undefined) {
  const valid = requireLang(lang)
  const [items, tags, site] = await Promise.all([fetchTimeline(valid), fetchTags(valid), fetchSiteCached(valid)])
  return { items, tags, siteName: site.name }
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
  return pageMeta({ lang, title: `${getI18n(lang).t('footer.sitemap')} · ${loaderData?.siteName ?? ''}`, path: '/sitemap' })
}

const pages = [
  ['', 'nav.home'],
  ['/posts', 'nav.posts'],
  ['/timeline', 'nav.timeline'],
  ['/guestbook', 'nav.guestbook'],
  ['/friends', 'nav.friends'],
  ['/projects', 'nav.projects'],
  ['/about', 'footer.aboutMe'],
  ['/about#site', 'footer.aboutSite'],
  ['/search', 'search.title'],
  ['/rss', 'rss.title'],
] as const

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <section className="rise-in rounded-3xl border bg-paper p-6 backdrop-blur-md md:p-8">
      <h2 className="mb-4 font-heading text-xl font-bold">{title}</h2>
      {children}
    </section>
  )
}

function Links({ links }: { links: { to: string; label: string }[] }) {
  return (
    <ul className="grid gap-x-6 gap-y-2 sm:grid-cols-2">
      {links.map(({ to, label }) => (
        <li key={to}>
          <Link to={to} className="underline-offset-4 hover:text-primary hover:underline">
            {label}
          </Link>
        </li>
      ))}
    </ul>
  )
}

export default function Sitemap({ loaderData }: Route.ComponentProps) {
  const { t } = useTranslation()
  const lang = useLang()
  const posts = loaderData.items.filter((item) => item.kind === 'post')
  const tags = loaderData.tags.filter((tag) => tag.postCount > 0)

  return (
    <div className="mx-auto max-w-3xl">
      <PageHeader title={t('footer.sitemap')} description={t('sitemap.description')} />
      <div className="grid gap-6">
        <Section title={t('sitemap.pages')}>
          <Links links={pages.map(([path, label]) => ({ to: localePath(lang, path), label: t(label) }))} />
        </Section>
        <Section title={`${t('nav.posts')} · ${posts.length}`}>
          {posts.length ? (
            <Links links={posts.map((post) => ({ to: localePath(lang, `/posts/${post.slug}`), label: post.title }))} />
          ) : (
            <p className="text-sm text-muted-foreground">{t('sitemap.none')}</p>
          )}
        </Section>
        {tags.length > 0 && (
          <Section title={t('nav.tags')}>
            <TagList tags={tags} />
          </Section>
        )}
      </div>
    </div>
  )
}
