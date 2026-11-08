import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { OnlineNow } from '~/components/live'
import { Hill } from '~/components/scenery/hill'
import { SubscribeForm } from '~/components/subscribe-form'
import type { Resources } from '~/i18n/zh'
import { localePath, useLang } from '~/lib/lang'
import { useSiteName } from '~/lib/site-name'

// The hillside, then on the ground the links: about, more, contact; the feeds and the credits below

interface FooterLink {
  label: `footer.${keyof Resources['footer']}` | `nav.${keyof Resources['nav']}`
  path: string
}

const columns: { key: keyof Resources['footer']; links: FooterLink[] }[] = [
  {
    key: 'about',
    links: [
      { label: 'footer.aboutSite', path: '/about#site' },
      { label: 'footer.aboutMe', path: '/about' },
    ],
  },
  {
    key: 'more',
    links: [
      { label: 'nav.timeline', path: '/timeline' },
      { label: 'nav.friends', path: '/friends' },
      { label: 'nav.projects', path: '/projects' },
    ],
  },
  {
    key: 'contact',
    links: [{ label: 'nav.guestbook', path: '/guestbook' }],
  },
]

export function SiteFooter() {
  const { t } = useTranslation()
  const lang = useLang()
  const siteName = useSiteName()
  return (
    <footer className="mt-24">
      <Hill />
      <div className="bg-ground text-[oklch(0.86_0.02_270)]">
        <div className="mx-auto grid max-w-6xl gap-8 px-4 py-10 sm:grid-cols-3 lg:grid-cols-[1fr_1fr_1fr_1.4fr]">
          {columns.map((column) => (
            <div key={column.key}>
              <h2 className="mb-3 font-heading text-sm font-bold text-[oklch(0.97_0.01_270)]">{t(`footer.${column.key}`)}</h2>
              <ul className="space-y-2 text-sm">
                {column.links.map((link) => (
                  <li key={link.label}>
                    <Link to={localePath(lang, link.path)} className="hover:text-lamp">
                      {t(link.label)}
                    </Link>
                  </li>
                ))}
              </ul>
            </div>
          ))}
          <div className="sm:col-span-3 lg:col-span-1">
            <h2 className="mb-3 font-heading text-sm font-bold text-[oklch(0.97_0.01_270)]">{t('subscribe.title')}</h2>
            <p className="mb-3 text-sm">{t('subscribe.text')}</p>
            <SubscribeForm tone="footer" />
          </div>
        </div>
        <div className="mx-auto flex max-w-6xl flex-wrap items-center gap-x-4 gap-y-2 border-t border-white/10 px-4 py-5 text-xs text-[oklch(0.72_0.02_270)]">
          <span>
            © {new Date().getUTCFullYear()} {siteName}
          </span>
          <Link to={localePath(lang, '/rss')} className="hover:text-lamp">
            RSS
          </Link>
          <Link to={localePath(lang, '/sitemap')} className="hover:text-lamp">
            {t('footer.sitemap')}
          </Link>
          <OnlineNow />
          <span className="ml-auto font-heading">{t('footer.motto')}</span>
          <span>{t('site.footer')}</span>
        </div>
      </div>
    </footer>
  )
}
