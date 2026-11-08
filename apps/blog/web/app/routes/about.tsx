import { lazy, Suspense } from 'react'
import { useTranslation } from 'react-i18next'
import type { Route } from './+types/about'
import { getI18n } from '~/i18n'
import { fetchAbout, fetchSiteCached } from '~/lib/content'
import { renderMarkdown } from '~/lib/markdown.server'
import { pageMeta } from '~/lib/meta'
import { requireLang } from '~/lib/route'

const Markdown = lazy(() => import('~/components/markdown').then((module) => ({ default: module.Markdown })))

async function load(lang: string | undefined) {
  const valid = requireLang(lang)
  const [about, site] = await Promise.all([fetchAbout(valid), fetchSiteCached(valid)])
  return { about, siteName: site.name, html: null as { about: string; aboutSite: string } | null }
}

// At build time with the HTML (renderMarkdown is server-only)
export async function loader({ params }: Route.LoaderArgs) {
  const page = await load(params.lang)
  page.html = { about: await renderMarkdown(page.about.about), aboutSite: await renderMarkdown(page.about.aboutSite) }
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
  return pageMeta({ lang, title: `${getI18n(lang).t('nav.about')} · ${loaderData?.siteName ?? ''}`, path: '/about' })
}

function Section({ id, title, html, markdown }: { id?: string; title: string; html: string | undefined; markdown: string }) {
  return (
    <section id={id} className="rise-in scroll-mt-24 rounded-3xl border bg-paper p-6 backdrop-blur-md md:p-10">
      <h2 className="mb-6 font-heading text-2xl font-bold">{title}</h2>
      {html !== undefined ? (
        <div className="prose max-w-none prose-night" dangerouslySetInnerHTML={{ __html: html }} />
      ) : (
        <Suspense>
          <Markdown source={markdown} />
        </Suspense>
      )}
    </section>
  )
}

export default function About({ loaderData }: Route.ComponentProps) {
  const { t } = useTranslation()
  const { about, html } = loaderData
  return (
    <div className="mx-auto grid max-w-3xl gap-8">
      {about.about && <Section title={t('footer.aboutMe')} html={html?.about} markdown={about.about} />}
      {about.aboutSite && <Section id="site" title={t('footer.aboutSite')} html={html?.aboutSite} markdown={about.aboutSite} />}
      {!about.about && !about.aboutSite && <p className="text-center text-muted-foreground">{t('about.empty')}</p>}
    </div>
  )
}
