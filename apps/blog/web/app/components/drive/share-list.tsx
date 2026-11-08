import { FileIcon, FolderIcon, LockIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogTrigger,
} from '~/components/ui/alert-dialog'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { Skeleton } from '~/components/ui/skeleton'
import { errorMessage, formatRelative } from '~/i18n'
import { copyLink, formatDate, shareUrl } from '~/lib/drive'
import { useLang } from '~/lib/lang'
import { useDeleteShare, useDriveShares, useShareAccess } from '~/lib/queries'
import type { DriveShare } from '~/lib/schemas'
import { useNow } from '~/lib/use-now'

function Access({ share, owner, onClose }: { share: DriveShare; owner?: number; onClose: () => void }) {
  const { t } = useTranslation()
  const lang = useLang()
  const access = useShareAccess(share.id, owner)
  const now = useNow()
  return (
    <Dialog open onOpenChange={(open) => !open && onClose()}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle className="truncate pr-6">
            {t('drive.access')} · {share.name}
          </DialogTitle>
        </DialogHeader>
        {access.isPending ? (
          <Skeleton className="h-24 w-full" />
        ) : !access.data?.length ? (
          <p className="text-sm text-muted-foreground">{t('drive.noAccess')}</p>
        ) : (
          <table className="w-full text-sm">
            <thead className="text-left text-xs text-muted-foreground">
              <tr>
                <th className="py-1 font-normal">{t('drive.name')}</th>
                <th className="py-1 text-right font-normal">{t('drive.views')}</th>
                <th className="py-1 text-right font-normal">{t('drive.downloads')}</th>
                <th className="py-1 text-right font-normal">{t('drive.lastAt')}</th>
              </tr>
            </thead>
            <tbody>
              {access.data.map((visitor) => (
                <tr key={visitor.userId} className="border-t">
                  <td className="py-1.5">
                    {visitor.displayName} <span className="text-muted-foreground">@{visitor.username}</span>
                  </td>
                  <td className="py-1.5 text-right tabular-nums">{visitor.views}</td>
                  <td className="py-1.5 text-right tabular-nums">{visitor.downloads}</td>
                  <td className="py-1.5 text-right text-muted-foreground">{formatRelative(lang, visitor.lastAt, now)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </DialogContent>
    </Dialog>
  )
}

// The account's links: what each shows, until when, who opened it; copied, looked into, revoked
export function ShareList({ owner }: { owner?: number }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const shares = useDriveShares(owner)
  const revoke = useDeleteShare(owner)
  const [access, setAccess] = useState<DriveShare | null>(null)

  if (shares.isPending) return <Skeleton className="h-32 w-full" />
  if (!shares.data?.length) return <p className="py-10 text-center text-muted-foreground">{t('drive.sharesEmpty')}</p>

  return (
    <>
      <ul className="divide-y overflow-hidden rounded-2xl border bg-paper backdrop-blur-md">
        {shares.data.map((share) => (
          <li key={share.id} className="flex flex-wrap items-center gap-x-4 gap-y-2 px-3 py-3">
            <div className="grid min-w-0 flex-1 gap-1">
              <span className="flex min-w-0 items-center gap-2 font-medium">
                {share.kind === 'folder' ? (
                  <FolderIcon className="size-4 shrink-0 text-amber-600" />
                ) : (
                  <FileIcon className="size-4 shrink-0 text-muted-foreground" />
                )}
                <span className="truncate">{share.name}</span>
                {share.hasPassword && (
                  <Badge variant="secondary">
                    <LockIcon />
                    {t('drive.protected')}
                  </Badge>
                )}
                {share.expired && <Badge variant="destructive">{t('drive.expired')}</Badge>}
                {share.trashed && <Badge variant="outline">{t('drive.inTrash')}</Badge>}
              </span>
              <span className="text-xs text-muted-foreground">
                {share.expiresAt ? t('drive.expiresAt', { date: formatDate(share.expiresAt, lang) }) : t('drive.neverExpires')}
                {' · '}
                {t('drive.stats', { visitors: share.visitors, views: share.views, downloads: share.downloads })}
              </span>
            </div>
            <div className="flex gap-1">
              <Button size="sm" variant="ghost" onClick={() => void copyLink(shareUrl(lang, share.token), t('drive.copied'))}>
                {t('drive.copy')}
              </Button>
              <Button size="sm" variant="ghost" onClick={() => setAccess(share)}>
                {t('drive.access')}
              </Button>
              <AlertDialog>
                <AlertDialogTrigger asChild>
                  <Button size="sm" variant="ghost" className="text-destructive">
                    {t('drive.revoke')}
                  </Button>
                </AlertDialogTrigger>
                <AlertDialogContent>
                  <AlertDialogHeader>
                    <AlertDialogTitle>{t('drive.revokeTitle')}</AlertDialogTitle>
                    <AlertDialogDescription>{t('drive.revokeHint')}</AlertDialogDescription>
                  </AlertDialogHeader>
                  <AlertDialogFooter>
                    <AlertDialogCancel>{t('drive.cancel')}</AlertDialogCancel>
                    <AlertDialogAction
                      variant="destructive"
                      onClick={() =>
                        revoke.mutate(share.id, {
                          onSuccess: () => toast.success(t('drive.revoked')),
                          onError: (error) => toast.error(errorMessage(i18n, error)),
                        })
                      }
                    >
                      {t('drive.revoke')}
                    </AlertDialogAction>
                  </AlertDialogFooter>
                </AlertDialogContent>
              </AlertDialog>
            </div>
          </li>
        ))}
      </ul>
      {access && <Access share={access} owner={owner} onClose={() => setAccess(null)} />}
    </>
  )
}
