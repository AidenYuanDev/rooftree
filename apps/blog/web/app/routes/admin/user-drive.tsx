import { ArrowLeftIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link, useParams } from 'react-router'
import { DriveTabs } from '~/components/drive/drive-view'
import { FormError } from '~/components/form'
import { Skeleton } from '~/components/ui/skeleton'
import { localePath, useLang } from '~/lib/lang'
import { useAdminUser } from '~/lib/queries'

// An account's drive as the admin sees it: its files and links, to add to and delete from
export default function AdminUserDrive() {
  const { t } = useTranslation()
  const lang = useLang()
  const id = Number(useParams().id)
  const user = useAdminUser(id)

  if (user.isPending) return <Skeleton className="h-96 w-full" />
  if (!user.data) return <FormError error={user.error} />

  return (
    <div className="grid gap-6">
      <Link
        to={localePath(lang, `/admin/users/${id}`)}
        className="flex w-fit items-center gap-1 text-sm text-muted-foreground hover:text-foreground"
      >
        <ArrowLeftIcon className="size-4" />
        {t('admin.backToUser', { name: user.data.displayName })}
      </Link>
      <div>
        <h1 className="text-xl font-semibold">{t('admin.userDrive', { name: user.data.displayName })}</h1>
        <p className="text-sm text-muted-foreground">{t('admin.userDriveHint')}</p>
      </div>
      <DriveTabs owner={id} basePath={`/admin/users/${id}/drive`} />
    </div>
  )
}
