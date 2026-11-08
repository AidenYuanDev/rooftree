import { useTranslation } from 'react-i18next'
import { Link, useLocation } from 'react-router'
import { Button } from '~/components/ui/button'
import { Skeleton } from '~/components/ui/skeleton'
import { localePath, useLang } from '~/lib/lang'
import { useMe } from '~/lib/queries'
import { canModerate, type User } from '~/lib/schemas'

// Renders children with the logged in user; a login link without one, a notice for others than the
// admin (admin set) or than the admin and moderators (moderator set). The API checks the same again,
// this is only what the page shows. prompt: what the login link says it is for
export function RequireUser({
  admin = false,
  moderator = false,
  prompt,
  children,
}: {
  admin?: boolean
  moderator?: boolean
  prompt?: string
  children: (user: User) => React.ReactNode
}) {
  const { t } = useTranslation()
  const lang = useLang()
  const location = useLocation()
  const me = useMe()

  if (me.isPending) return <Skeleton className="h-48 w-full" />

  const user = me.data?.user
  if (!user) {
    return (
      <div className="py-16 text-center">
        <p className="mb-4">{prompt ?? t('account.loginRequired')}</p>
        <Button asChild>
          <Link to={`${localePath(lang, '/login')}?next=${encodeURIComponent(location.pathname + location.search)}`}>{t('nav.login')}</Link>
        </Button>
      </div>
    )
  }

  if ((admin && user.role !== 'admin') || (moderator && !canModerate(user)))
    return <p className="py-16 text-center text-muted-foreground">{t('admin.forbidden')}</p>

  return <>{children(user)}</>
}
