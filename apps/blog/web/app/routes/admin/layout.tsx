import { useTranslation } from 'react-i18next'
import { Navigate, NavLink, Outlet, useLocation } from 'react-router'
import type { Route } from './+types/layout'
import { RequireUser } from '~/components/require-user'
import { localePath, useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { cn } from '~/lib/utils'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'admin.title')

// moderator: the sections moderators have too, the rest is the admin's
const sections = [
  ['', 'admin.posts', false],
  ['/notes', 'admin.notes', false],
  ['/thinking', 'admin.thinking', false],
  ['/comments', 'admin.comments', true],
  ['/users', 'admin.users', true],
  ['/drives', 'admin.drives', false],
  ['/stats', 'admin.stats', false],
  ['/tags', 'admin.tags', false],
  ['/friends', 'admin.friends', false],
  ['/projects', 'admin.projects', false],
  ['/site', 'admin.site', false],
] as const

export default function AdminLayout() {
  const { t } = useTranslation()
  const lang = useLang()
  const { pathname } = useLocation()
  const link = ({ isActive }: { isActive: boolean }) =>
    cn(
      'rounded-md px-3 py-1.5 text-sm font-medium transition-colors',
      isActive ? 'bg-muted text-foreground' : 'text-muted-foreground hover:text-foreground',
    )

  return (
    <RequireUser moderator>
      {(user) => {
        const admin = user.role === 'admin'
        // A moderator elsewhere than their sections (the list of users, not one user's page)
        const section = pathname.slice(localePath(lang, '/admin').length).replace(/\/$/, '')
        if (!admin && !sections.some(([path, , moderator]) => moderator && path === section))
          return <Navigate to={localePath(lang, '/admin/comments')} replace />

        return (
          <div className="grid gap-6">
            <nav aria-label={t('admin.title')} className="flex flex-wrap items-center gap-1 border-b pb-3">
              <span className="mr-4 font-semibold">{t('admin.title')}</span>
              {sections
                .filter(([, , moderator]) => admin || moderator)
                .map(([path, label]) => (
                  <NavLink key={path} to={localePath(lang, `/admin${path}`)} end={path === ''} className={link}>
                    {t(label)}
                  </NavLink>
                ))}
            </nav>
            <Outlet />
          </div>
        )
      }}
    </RequireUser>
  )
}
