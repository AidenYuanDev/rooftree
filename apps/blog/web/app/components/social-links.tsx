import { MailIcon, MusicIcon, RssIcon } from 'lucide-react'
import {
  siBilibili,
  siBluesky,
  siDiscord,
  siGithub,
  siMastodon,
  siSinaweibo,
  siSteam,
  siTelegram,
  siX,
  siYoutube,
  siZhihu,
  type SimpleIcon,
} from 'simple-icons'
import { useTranslation } from 'react-i18next'
import type { Site, SocialKind } from '~/lib/schemas'
import { cn } from '~/lib/utils'

const brands: Partial<Record<SocialKind, SimpleIcon>> = {
  github: siGithub,
  x: siX,
  bilibili: siBilibili,
  telegram: siTelegram,
  mastodon: siMastodon,
  bluesky: siBluesky,
  zhihu: siZhihu,
  weibo: siSinaweibo,
  steam: siSteam,
  discord: siDiscord,
  youtube: siYoutube,
}

function Icon({ kind }: { kind: SocialKind }) {
  const brand = brands[kind]
  if (brand) {
    return (
      <svg viewBox="0 0 24 24" className="size-4" fill="currentColor" aria-hidden>
        <path d={brand.path} />
      </svg>
    )
  }
  if (kind === 'email') return <MailIcon className="size-4" />
  if (kind === 'rss') return <RssIcon className="size-4" />
  return <MusicIcon className="size-4" />
}

export function SocialLinks({ socials, className }: { socials: Site['socials']; className?: string }) {
  const { t } = useTranslation()
  return (
    <ul className={cn('flex flex-wrap gap-2', className)}>
      {socials.map((social) => (
        <li key={social.kind + social.url}>
          <a
            href={social.url}
            target={social.url.startsWith('http') ? '_blank' : undefined}
            rel="noopener me"
            aria-label={t(`social.${social.kind}`)}
            title={t(`social.${social.kind}`)}
            className="flex size-9 items-center justify-center rounded-full border bg-card/60 text-muted-foreground backdrop-blur transition hover:-translate-y-0.5 hover:border-primary/50 hover:text-primary"
          >
            <Icon kind={social.kind} />
          </a>
        </li>
      ))}
    </ul>
  )
}
