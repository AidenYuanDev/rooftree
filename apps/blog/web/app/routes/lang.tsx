import { I18nextProvider } from 'react-i18next'
import { Outlet, useParams } from 'react-router'
import { ChatDock } from '~/components/chat/chat-dock'
import { CommandPalette } from '~/components/command-palette'
import { Live } from '~/components/live'
import { PageViews } from '~/components/page-views'
import { SiteFooter } from '~/components/site-footer'
import { SiteHeader } from '~/components/site-header'
import { Painting } from '~/components/scenery/painting'
import { UnverifiedBanner } from '~/components/unverified-banner'
import { getI18n } from '~/i18n'
import { isLang } from '~/lib/schemas'
import { NotFoundPage } from './not-found'

// Every page's frame, in the language of its URL. No loader here: its children are not all
// prerendered (admin), and a parent loader would need data for them at runtime
export default function LangLayout() {
  const { lang } = useParams()
  const i18n = getI18n(isLang(lang) ? lang : 'zh')
  return (
    <I18nextProvider i18n={i18n}>
      <Painting />
      <div className="flex min-h-dvh flex-col">
        <SiteHeader />
        <div className="pt-16">
          <UnverifiedBanner />
        </div>
        <main id="content" className="mx-auto w-full max-w-6xl flex-1 px-4 pt-8 pb-8">
          {isLang(lang) ? <Outlet /> : <NotFoundPage />}
        </main>
        <SiteFooter />
      </div>
      <CommandPalette />
      <Live />
      <PageViews />
      <ChatDock />
    </I18nextProvider>
  )
}
