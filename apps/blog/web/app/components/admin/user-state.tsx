import { useTranslation } from 'react-i18next'
import { Badge } from '~/components/ui/badge'
import { formatDuration, formatRelative } from '~/i18n'
import { useLang } from '~/lib/lang'
import { useNow } from '~/lib/use-now'
import type { AdminUser } from '~/lib/schemas'

// The admin's own ban check: dates past 2199 are "for good" (the server's PermanentBanTime)
const isForever = (iso: string) => new Date(iso).getUTCFullYear() >= 2199

// Online now, or when last; the time online in all
export function OnlineState({ user }: { user: AdminUser }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const now = useNow()
  if (user.online) {
    const seconds = (now - Date.parse(user.online.since)) / 1000
    return (
      <span className="text-emerald-600 dark:text-emerald-400">
        {t('admin.onlineFor', { duration: formatDuration(i18n, seconds) })}
        {' · '}
        {t('admin.viewing')} <code className="text-xs">{user.online.pages.join(', ')}</code>
      </span>
    )
  }
  return <span>{user.lastSeenAt ? t('admin.lastSeen', { time: formatRelative(lang, user.lastSeenAt, now) }) : t('admin.neverSeen')}</span>
}

export function UserBadges({ user }: { user: AdminUser }) {
  const { t } = useTranslation()
  const lang = useLang()
  return (
    <>
      {user.role !== 'user' && <Badge variant={user.role === 'admin' ? 'default' : 'secondary'}>{t(`admin.roles.${user.role}`)}</Badge>}
      {user.deleted && <Badge variant="outline">{t('admin.deletedAccount')}</Badge>}
      {!user.deleted && !user.emailVerified && <Badge variant="outline">{t('admin.unverified')}</Badge>}
      {user.denied.map((action) => (
        <Badge key={action} variant="outline" className="border-amber-500/50 text-amber-700 dark:text-amber-400">
          {t(`admin.deniedShort.${action}`)}
        </Badge>
      ))}
      {user.bannedUntil && (
        <Badge variant="destructive" title={user.banReason ?? ''}>
          {isForever(user.bannedUntil)
            ? t('admin.bannedForever')
            : t('admin.banned', { date: new Date(user.bannedUntil).toLocaleDateString(lang) })}
        </Badge>
      )}
    </>
  )
}
