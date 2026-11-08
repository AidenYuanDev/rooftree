import { useTranslation } from 'react-i18next'
import type { Route } from './+types/guestbook'
import { CottageMark } from '~/components/scenery/mark'
import { Comments } from '~/components/comments'
import { PageHeader } from '~/components/page-header'
import { getI18n } from '~/i18n'
import { fetchSiteCached } from '~/lib/content'
import { pageMeta } from '~/lib/meta'
import { requireLang } from '~/lib/route'

async function load(lang: string | undefined) {
  return { siteName: (await fetchSiteCached(requireLang(lang))).name }
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
  return pageMeta({ lang, title: `${getI18n(lang).t('nav.guestbook')} · ${loaderData?.siteName ?? ''}`, path: '/guestbook' })
}

// Comments on nothing in particular (target "guestbook")
export default function Guestbook() {
  const { t } = useTranslation()
  return (
    <div className="mx-auto max-w-3xl">
      <div className="flex items-end gap-6">
        <PageHeader title={t('nav.guestbook')} description={t('guestbook.description')} className="flex-1" />
        <CottageMark className="mb-10 hidden size-16 sm:block" />
      </div>
      <div className="rounded-3xl border bg-paper p-6 backdrop-blur-md md:p-10">
        <Comments target="guestbook" />
      </div>
    </div>
  )
}
