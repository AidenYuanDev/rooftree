import { CheckIcon, CopyIcon, RssIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import type { Route } from './+types/rss'
import { PageHeader } from '~/components/page-header'
import { SubscribeForm } from '~/components/subscribe-form'
import { Button } from '~/components/ui/button'
import { getI18n } from '~/i18n'
import { fetchSiteCached } from '~/lib/content'
import { useLang } from '~/lib/lang'
import { pageMeta, publicOrigin } from '~/lib/meta'
import { requireLang } from '~/lib/route'
import type { Lang } from '~/lib/schemas'

// What the footer's RSS opens: the feed itself is XML for the readers' apps, this says what to do with it

async function load(lang: string | undefined) {
  const site = await fetchSiteCached(requireLang(lang))
  return { siteName: site.name }
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
  return pageMeta({ lang, title: `${getI18n(lang).t('rss.title')} · ${loaderData?.siteName ?? ''}`, path: '/rss' })
}

const feedUrl = (lang: Lang) => `${publicOrigin}/${lang}/feed.xml`

function Feed({ lang }: { lang: Lang }) {
  const { t } = useTranslation()
  const [copied, setCopied] = useState(false)
  const url = feedUrl(lang)
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(url)
      setCopied(true)
      setTimeout(() => setCopied(false), 2000)
    } catch {
      // No clipboard (http, an old browser): the address is there to select
    }
  }

  return (
    <div className="grid gap-2">
      <p className="text-sm font-medium">{t(`rss.feed.${lang}`)}</p>
      <div className="flex gap-2">
        {/* No width of its own: on a phone the address wraps instead of widening the card */}
        <code className="w-0 flex-1 rounded-lg border bg-background/60 px-3 py-2 text-sm break-all">{url}</code>
        <Button type="button" variant="outline" onClick={copy} aria-label={t('rss.copy')}>
          {copied ? <CheckIcon /> : <CopyIcon />}
          <span className="hidden sm:inline">{copied ? t('rss.copied') : t('rss.copy')}</span>
        </Button>
      </div>
      <p className="flex flex-wrap gap-x-4 gap-y-1 text-sm">
        <a
          className="text-primary underline-offset-4 hover:underline"
          href={`https://feedly.com/i/subscription/feed/${encodeURIComponent(url)}`}
          target="_blank"
          rel="noreferrer"
        >
          {t('rss.openIn', { app: 'Feedly' })}
        </a>
        <a
          className="text-primary underline-offset-4 hover:underline"
          href={`https://www.inoreader.com/?add_feed=${encodeURIComponent(url)}`}
          target="_blank"
          rel="noreferrer"
        >
          {t('rss.openIn', { app: 'Inoreader' })}
        </a>
        <a className="text-muted-foreground underline-offset-4 hover:underline" href={`/${lang}/feed.xml`}>
          {t('rss.raw')}
        </a>
      </p>
    </div>
  )
}

export default function Rss() {
  const { t } = useTranslation()
  const lang = useLang()
  const other: Lang = lang === 'zh' ? 'en' : 'zh'
  return (
    <div className="mx-auto max-w-3xl">
      <PageHeader title={t('rss.title')} description={t('rss.description')} />
      <section className="rise-in grid gap-6 rounded-3xl border bg-paper p-6 backdrop-blur-md md:p-10">
        <div className="flex gap-3 text-sm text-muted-foreground">
          <RssIcon className="mt-0.5 size-4 shrink-0 text-lamp" />
          <p>{t('rss.howTo')}</p>
        </div>
        <Feed lang={lang} />
        <Feed lang={other} />
      </section>
      <section className="rise-in mt-8 rounded-3xl border bg-paper p-6 backdrop-blur-md md:p-10">
        <h2 className="mb-2 font-heading text-xl font-bold">{t('rss.mailTitle')}</h2>
        <p className="mb-4 text-sm text-muted-foreground">{t('rss.mailText')}</p>
        <SubscribeForm />
      </section>
    </div>
  )
}
