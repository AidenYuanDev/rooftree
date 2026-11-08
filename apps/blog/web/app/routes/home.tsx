import { ArrowRightIcon } from 'lucide-react'
import { Fragment } from 'react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import type { Route } from './+types/home'
import { LocalDate } from '~/components/local-date'
import { Badge } from '~/components/ui/badge'
import { SocialLinks } from '~/components/social-links'
import { SubscribeForm } from '~/components/subscribe-form'
import { TagList } from '~/components/tag-list'
import { getI18n } from '~/i18n'
import { fetchNoteList, fetchPostList, fetchSiteCached, fetchTags, fetchThinkingList } from '~/lib/content'
import { localePath } from '~/lib/lang'
import { pageMeta } from '~/lib/meta'
import { requireLang } from '~/lib/route'
import type { Lang, Site } from '~/lib/schemas'
import { plainText } from '~/lib/text'

async function load(lang: string | undefined) {
  const valid = requireLang(lang)
  const [firstPage, notes, thinking, tags, site] = await Promise.all([
    fetchPostList(valid),
    fetchNoteList(valid),
    fetchThinkingList(valid),
    fetchTags(valid),
    fetchSiteCached(valid),
  ])
  // Posts and notes together, newest first
  const recent: Recent[] = [
    ...firstPage.posts.map((post) => ({
      kind: 'post' as const,
      key: `post-${post.id}`,
      title: post.title,
      summary: post.summary,
      lang: post.lang,
      path: `/posts/${post.slug}`,
      publishedAt: post.publishedAt,
    })),
    ...notes.notes.map((note) => ({
      kind: 'note' as const,
      key: `note-${note.id}`,
      title: note.title,
      summary: plainText(note.excerpt, 80),
      lang: note.lang,
      path: `/notes/${note.id}`,
      publishedAt: note.publishedAt,
    })),
  ]
    .sort((a, b) => b.publishedAt.localeCompare(a.publishedAt))
    .slice(0, 6)
  return { recent, thinking: thinking.thinking[0] ?? null, tags, site }
}

interface Recent {
  kind: 'post' | 'note'
  key: string
  title: string
  summary: string
  lang: Lang
  path: string
  publishedAt: string
}

// At build time, for the prerendered page
export async function loader({ params }: Route.LoaderArgs) {
  return load(params.lang)
}

// In the browser: the prerendered data, or the API when this page wasn't prerendered
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
  const i18n = getI18n(lang)
  const site = loaderData?.site
  return pageMeta({
    lang,
    title: site?.name ?? i18n.t('site.name'),
    description: site?.bio || i18n.t('site.tagline'),
    path: '',
    image: site?.avatar || undefined,
  })
}

// "Writing **C++** and ...": the starred part highlighted
function Tagline({ text }: { text: string }) {
  return (
    <>
      {text.split(/\*\*(.+?)\*\*/).map((part, i) =>
        i % 2 ? (
          <span key={i} className="mx-1 rounded-md bg-primary/15 px-1.5 py-0.5 font-mono text-[0.9em] text-primary">
            ✦ {part}
          </span>
        ) : (
          <Fragment key={i}>{part}</Fragment>
        ),
      )}
    </>
  )
}

function Hero({ site }: { site: Site }) {
  const { t, i18n } = useTranslation()
  const number = new Intl.NumberFormat(i18n.language)
  const stats = [
    { label: t('home.statPosts'), value: number.format(site.stats.posts) },
    { label: t('home.statNotes'), value: number.format(site.stats.notes) },
    {
      label: t('home.statWords'),
      value: site.stats.words >= 1000 ? `${number.format(Math.round(site.stats.words / 1000))}k` : number.format(site.stats.words),
    },
    { label: t('home.statDays'), value: number.format(site.stats.days) },
  ]

  return (
    // On the right, over the open sky, the cottage and the tree of the painting in view on the left. On
    // glass: the tree reaches under the text, dark by day and lit by the moon at night
    <section className="home-hero grid min-h-[min(78vh,44rem)] items-start md:grid-cols-[1.15fr_1fr] md:items-center">
      <div className="rise-in rounded-3xl glass p-6 md:col-start-2 md:p-10">
        {site.avatar && (
          <img src={site.avatar} alt="" className="mb-6 size-20 rounded-full border-2 border-primary/40 object-cover shadow-lg" />
        )}
        <h1 className="font-heading text-4xl leading-tight font-bold md:text-5xl">
          {/* No-break spaces: a name of two words stays on one line */}
          {t('home.greeting', { name: (site.author || site.name).replaceAll(' ', '\u00a0') })}
        </h1>
        <p className="mt-4 text-lg md:text-xl">
          <Tagline text={site.tagline} />
        </p>
        <p className="mt-4 max-w-xl leading-relaxed text-muted-foreground">{site.bio}</p>
        {site.quote && <blockquote className="mt-6 font-heading text-lg text-muted-foreground">「{site.quote}」</blockquote>}

        <dl className="mt-8 flex gap-8">
          {stats.map((stat) => (
            <div key={stat.label}>
              <dt className="text-xs text-muted-foreground">{stat.label}</dt>
              <dd className="font-heading text-2xl font-bold">{stat.value}</dd>
            </div>
          ))}
        </dl>
        <SocialLinks socials={site.socials} className="mt-8" />
      </div>
    </section>
  )
}

function RecentWriting({ loaderData, lang }: { loaderData: Route.ComponentProps['loaderData']; lang: Lang }) {
  const { t } = useTranslation()
  const { recent, thinking } = loaderData
  return (
    <section className="grid gap-10 md:grid-cols-[1fr_17rem]">
      {/* min-w-0: or the titles' one line would widen it past the screen instead of truncating */}
      <div className="min-w-0 rounded-3xl glass p-6">
        <h2 className="mb-6 font-heading text-2xl font-bold">{t('home.recent')}</h2>
        {recent.length === 0 ? (
          <p className="text-muted-foreground">{t('posts.empty')}</p>
        ) : (
          <ol className="space-y-1">
            {recent.map((item, i) => (
              <li key={item.key} className="rise-in" style={{ '--rise-index': i } as React.CSSProperties}>
                <Link
                  to={localePath(lang, item.path)}
                  className="group flex items-baseline gap-4 rounded-xl px-3 py-3 transition-colors hover:bg-card/70"
                >
                  <span className="font-mono text-sm text-muted-foreground tabular-nums">{String(i + 1).padStart(2, '0')}</span>
                  <span className="min-w-0 flex-1">
                    <span className="flex items-center gap-2">
                      <Badge variant="outline" className="shrink-0">
                        {t(`admin.on.${item.kind}`)}
                      </Badge>
                      <span className="truncate font-medium group-hover:text-primary" lang={item.lang}>
                        {item.title}
                      </span>
                    </span>
                    {item.summary && (
                      <span className="mt-0.5 block truncate text-sm text-muted-foreground" lang={item.lang}>
                        {item.summary}
                      </span>
                    )}
                  </span>
                  <span className="hidden shrink-0 text-xs text-muted-foreground sm:block">
                    <LocalDate iso={item.publishedAt} lang={lang} />
                  </span>
                </Link>
              </li>
            ))}
          </ol>
        )}
        <div className="mt-4 flex gap-4 px-3 text-sm">
          <Link to={localePath(lang, '/posts')} className="inline-flex items-center gap-1 text-primary hover:underline">
            {t('home.allPosts')}
            <ArrowRightIcon className="size-4" />
          </Link>
          <Link to={localePath(lang, '/notes')} className="inline-flex items-center gap-1 text-primary hover:underline">
            {t('home.allNotes')}
            <ArrowRightIcon className="size-4" />
          </Link>
        </div>
      </div>
      <aside className="space-y-8">
        {thinking && (
          <Link
            to={localePath(lang, `/thinking#thinking-${thinking.id}`)}
            className="block rounded-2xl rounded-tl-sm border bg-card/60 p-5 backdrop-blur-md transition hover:border-primary/40"
          >
            <span className="mb-2 block text-xs text-muted-foreground">{t('home.lastThinking')}</span>
            <span className="line-clamp-4 font-heading text-lg" lang={thinking.lang}>
              {plainText(thinking.body, 140)}
            </span>
          </Link>
        )}
        <div className="rounded-3xl glass p-6">
          <h2 className="mb-4 font-heading text-lg font-bold">{t('nav.tags')}</h2>
          <TagList tags={loaderData.tags} />
        </div>
      </aside>
    </section>
  )
}

function Guestbook({ lang }: { lang: Lang }) {
  const { t } = useTranslation()
  return (
    <section className="rise-in flex flex-col items-center justify-center gap-4 rounded-3xl glass px-6 py-10 text-center">
      <h2 className="font-heading text-2xl font-bold">{t('home.leaveMark')}</h2>
      <p className="text-muted-foreground">{t('home.leaveMarkText')}</p>
      <Link
        to={localePath(lang, '/guestbook')}
        className="rounded-full bg-primary px-5 py-2 text-sm text-primary-foreground transition hover:opacity-90"
      >
        {t('nav.guestbook')}
      </Link>
    </section>
  )
}

export default function Home({ loaderData, params }: Route.ComponentProps) {
  const { t } = useTranslation()
  const lang: Lang = params.lang === 'en' ? 'en' : 'zh'
  return (
    <div className="space-y-20">
      <Hero site={loaderData.site} />
      <RecentWriting loaderData={loaderData} lang={lang} />
      <div className="grid grid-cols-1 gap-6 md:grid-cols-2">
        <Guestbook lang={lang} />
        <section className="rise-in flex flex-col items-center justify-center gap-4 rounded-3xl glass px-6 py-10 text-center">
          <h2 className="font-heading text-2xl font-bold">{t('subscribe.title')}</h2>
          <p className="text-muted-foreground">{t('subscribe.homeText')}</p>
          <SubscribeForm className="w-full max-w-sm" />
        </section>
      </div>
    </div>
  )
}
