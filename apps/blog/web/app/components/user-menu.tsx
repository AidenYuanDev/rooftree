import { CloudIcon, HardDriveIcon, LogOutIcon, PenLineIcon, SettingsIcon, ShieldIcon, UserIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link, useNavigate } from 'react-router'
import { toast } from 'sonner'
import { Button } from '~/components/ui/button'
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from '~/components/ui/dropdown-menu'
import { Skeleton } from '~/components/ui/skeleton'
import { UserAvatar } from '~/components/user-avatar'
import { localePath, useLang } from '~/lib/lang'
import { useLogout, useMe, useUserFriendRequests } from '~/lib/queries'
import { canModerate } from '~/lib/schemas'

export function UserMenu() {
  const { t } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const me = useMe()
  const logout = useLogout()
  const user = me.data?.user
  const friendRequests = useUserFriendRequests(!!user)
  const incomingCount = friendRequests.data?.incoming.length ?? 0

  // Prerendered pages don't know who reads them: a placeholder until the session is known
  if (me.isPending) return <Skeleton className="h-8 w-20" />
  if (!user) {
    return (
      <div className="flex items-center gap-1">
        <Button variant="ghost" size="sm" asChild>
          <Link to={localePath(lang, '/login')}>{t('nav.login')}</Link>
        </Button>
        {/* The narrowest screens have no room for it; the login page links to it */}
        <Button size="sm" className="max-[359px]:hidden" asChild>
          <Link to={localePath(lang, '/register')}>{t('nav.register')}</Link>
        </Button>
      </div>
    )
  }

  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button variant="ghost" size="sm" className="relative max-w-40">
          <div className="relative">
            <UserAvatar user={user} className="size-6 text-xs" />
            {incomingCount > 0 && <span className="absolute -top-1 -right-1 size-2 animate-pulse rounded-full bg-red-500" />}
          </div>
          <span className="truncate">{user.displayName}</span>
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end" className="min-w-44">
        <DropdownMenuLabel className="truncate">@{user.username}</DropdownMenuLabel>
        <DropdownMenuSeparator />
        <DropdownMenuItem asChild>
          <Link to={localePath(lang, `/u/${encodeURIComponent(user.username)}`)}>
            <UserIcon />
            {t('person.title')}
          </Link>
        </DropdownMenuItem>
        {canModerate(user) && (
          <DropdownMenuItem asChild>
            <Link to={localePath(lang, user.role === 'admin' ? '/admin' : '/admin/comments')}>
              <ShieldIcon />
              {t('nav.admin')}
            </Link>
          </DropdownMenuItem>
        )}
        {/* A writer's posts, or the ones it was invited to edit (the admin has them in its pages) */}
        {user.role !== 'admin' && (user.write || user.editing) && (
          <DropdownMenuItem asChild>
            <Link to={localePath(lang, '/write')}>
              <PenLineIcon />
              {t('nav.write')}
            </Link>
          </DropdownMenuItem>
        )}
        {user.drive && (
          <DropdownMenuItem asChild>
            <Link to={localePath(lang, '/drive')}>
              <HardDriveIcon />
              {t('nav.drive')}
            </Link>
          </DropdownMenuItem>
        )}
        {/* The server's numbers, the admin's only */}
        {user.role === 'admin' && (
          <DropdownMenuItem asChild>
            <Link to={localePath(lang, '/status')}>
              <CloudIcon />
              {t('nav.status')}
            </Link>
          </DropdownMenuItem>
        )}
        <DropdownMenuItem asChild>
          <Link to={localePath(lang, '/account')} className="flex items-center justify-between">
            <span className="flex items-center gap-2">
              <SettingsIcon />
              {t('nav.account')}
            </span>
            {incomingCount > 0 && (
              <span className="py-0.2 rounded-full bg-red-500 px-1.5 text-[0.65rem] font-bold text-white">{incomingCount}</span>
            )}
          </Link>
        </DropdownMenuItem>
        <DropdownMenuSeparator />
        <DropdownMenuItem
          onSelect={() =>
            logout.mutate(undefined, {
              onSuccess: () => {
                toast.success(t('auth.loggedOut'))
                void navigate(localePath(lang))
              },
            })
          }
        >
          <LogOutIcon />
          {t('nav.logout')}
        </DropdownMenuItem>
      </DropdownMenuContent>
    </DropdownMenu>
  )
}
