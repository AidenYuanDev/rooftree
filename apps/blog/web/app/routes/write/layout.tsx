import { useTranslation } from 'react-i18next'
import { Outlet } from 'react-router'
import type { Route } from './+types/layout'
import { RequireUser } from '~/components/require-user'
import { privateMeta } from '~/lib/page'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'write.title')

// A writer's pages: its own posts and the ones it was invited to edit (the server shows it no
// others), the same list and editor as the admin's
export default function WriteLayout() {
  const { t } = useTranslation()
  return (
    <RequireUser prompt={t('write.loginRequired')}>
      {(user) => (
        <div className="grid gap-6">
          <div className="border-b pb-3">
            <h1 className="text-2xl font-semibold tracking-tight">{t('write.title')}</h1>
            {!user.write && <p className="mt-1 text-sm text-muted-foreground">{t('write.invitedOnly')}</p>}
          </div>
          <Outlet />
        </div>
      )}
    </RequireUser>
  )
}
