import { useTranslation } from 'react-i18next'
import type { Route } from './+types/drive'
import { DriveTabs } from '~/components/drive/drive-view'
import { PageHeader } from '~/components/page-header'
import { RequireUser } from '~/components/require-user'
import { privateMeta } from '~/lib/page'

// The account's drive: its folders (?folder=), uploads dropped or chosen, and the links it gave. The
// page is a prerendered shell; who reads it, and whether they have a drive, is known in the browser

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'drive.title')

export default function DrivePage() {
  const { t } = useTranslation()
  return (
    <div className="mx-auto max-w-5xl">
      <PageHeader title={t('drive.title')} description={t('drive.description')} />
      <RequireUser prompt={t('drive.loginRequired')}>
        {(user) =>
          user.drive ? <DriveTabs basePath="/drive" /> : <p className="py-16 text-center text-muted-foreground">{t('drive.noDrive')}</p>
        }
      </RequireUser>
    </div>
  )
}
