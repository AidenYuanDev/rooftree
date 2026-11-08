import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { FormError } from '~/components/form'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '~/components/ui/card'
import { Skeleton } from '~/components/ui/skeleton'
import { formatBytes } from '~/lib/drive'
import { localePath, useLang } from '~/lib/lang'
import { useAdminDrives } from '~/lib/queries'

function Bar({ used, of }: { used: number; of: number }) {
  const percent = of ? Math.min(100, (used / of) * 100) : used ? 100 : 0
  return (
    <div className="h-1.5 w-full overflow-hidden rounded-full bg-muted">
      <div className={`h-full ${percent >= 90 ? 'bg-destructive' : 'bg-primary'}`} style={{ width: `${percent}%` }} />
    </div>
  )
}

// Every drive at a glance: how full each is, what is given out, and the disk under them
export default function AdminDrives() {
  const { t } = useTranslation()
  const lang = useLang()
  const drives = useAdminDrives()

  if (drives.isPending) return <Skeleton className="h-96 w-full" />
  if (!drives.data) return <FormError error={drives.error} />
  const { disk, quotas, used, accounts } = drives.data
  const size = (bytes: number) => formatBytes(bytes, lang)
  // What the drives may still take, against what the disk can
  const overcommitted = quotas - used > disk.free - disk.minFree

  return (
    <div className="grid gap-6">
      <div>
        <h1 className="text-xl font-semibold">{t('admin.drives')}</h1>
        <p className="text-sm text-muted-foreground">{t('admin.drivesHint')}</p>
      </div>

      <div className="grid gap-4 md:grid-cols-2">
        <Card>
          <CardHeader>
            <CardTitle>{t('admin.drivesDisk')}</CardTitle>
            <CardDescription>
              {t('admin.drivesDiskUsage', { free: size(disk.free), total: size(disk.total), minFree: size(disk.minFree) })}
            </CardDescription>
          </CardHeader>
          <CardContent>
            <Bar used={disk.total - disk.free} of={disk.total} />
          </CardContent>
        </Card>
        <Card>
          <CardHeader>
            <CardTitle>{t('admin.drivesQuotas')}</CardTitle>
            <CardDescription>{t('admin.drivesQuotasHint', { quotas: size(quotas), used: size(used) })}</CardDescription>
          </CardHeader>
          <CardContent className="grid gap-2">
            <Bar used={used} of={quotas} />
            {overcommitted && <p className="text-sm text-destructive">{t('admin.drivesOvercommitted')}</p>}
          </CardContent>
        </Card>
      </div>

      {accounts.length === 0 ? (
        <p className="py-10 text-center text-muted-foreground">{t('admin.drivesNone')}</p>
      ) : (
        <div className="overflow-x-auto rounded-2xl border bg-paper backdrop-blur-md">
          <table className="w-full min-w-[40rem] text-sm">
            <thead className="text-left text-xs text-muted-foreground">
              <tr className="border-b">
                <th className="px-3 py-2 font-normal">{t('admin.drivesAccount')}</th>
                <th className="w-56 px-3 py-2 font-normal">{t('admin.drivesUsed')}</th>
                <th className="px-3 py-2 text-right font-normal">{t('admin.drivesFiles')}</th>
                <th className="px-3 py-2 font-normal">{t('admin.drivesTrash')}</th>
                <th className="px-3 py-2">
                  <span className="sr-only">{t('drive.actions')}</span>
                </th>
              </tr>
            </thead>
            <tbody className="divide-y">
              {accounts.map((account) => (
                <tr key={account.id}>
                  <td className="px-3 py-2">
                    <Link to={localePath(lang, `/admin/users/${account.id}`)} className="font-medium hover:underline">
                      {account.displayName}
                    </Link>{' '}
                    <span className="text-muted-foreground">@{account.username}</span>{' '}
                    {!account.enabled && <Badge variant="outline">{t('admin.drivesOff')}</Badge>}
                  </td>
                  <td className="grid gap-1 px-3 py-2">
                    <span className="tabular-nums">
                      {size(account.used)} / {size(account.quota)}
                    </span>
                    <Bar used={account.used} of={account.quota} />
                  </td>
                  <td className="px-3 py-2 text-right tabular-nums">{account.files}</td>
                  <td className="px-3 py-2 text-muted-foreground">
                    {t('admin.drivesTrashOf', { size: size(account.trashed), days: account.trashDays })}
                  </td>
                  <td className="px-3 py-2 text-right">
                    <Button size="sm" variant="ghost" asChild>
                      <Link to={localePath(lang, `/admin/users/${account.id}/drive`)}>{t('admin.drivesOpen')}</Link>
                    </Button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}
