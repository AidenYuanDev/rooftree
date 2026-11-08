import { FileIcon, FolderIcon } from 'lucide-react'
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
} from '~/components/ui/alert-dialog'
import { Button } from '~/components/ui/button'
import { Skeleton } from '~/components/ui/skeleton'
import { errorMessage, formatRelative } from '~/i18n'
import { formatBytes } from '~/lib/drive'
import { useLang } from '~/lib/lang'
import { useDriveChanges, useDriveTrash } from '~/lib/queries'
import type { DriveTrashItem } from '~/lib/schemas'
import { useNow } from '~/lib/use-now'

// What the drive's trash holds: where each was, when it goes for good; restored, or deleted for good
export function TrashList({ owner }: { owner?: number }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const trash = useDriveTrash(owner)
  const changes = useDriveChanges(owner)
  const now = useNow()
  const [purging, setPurging] = useState<DriveTrashItem | null>(null)
  const [emptying, setEmptying] = useState(false)
  const onError = (error: unknown) => toast.error(errorMessage(i18n, error))

  if (trash.isPending) return <Skeleton className="h-32 w-full" />
  if (trash.isError) return null
  const { trash: items, days } = trash.data

  return (
    <div className="grid gap-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <p className="text-sm text-muted-foreground">{days > 0 ? t('drive.trashHint', { count: days }) : t('drive.trashOff')}</p>
        {items.length > 0 && (
          <Button size="sm" variant="outline" className="text-destructive" onClick={() => setEmptying(true)}>
            {t('drive.emptyTrash')}
          </Button>
        )}
      </div>

      {items.length === 0 ? (
        <p className="py-10 text-center text-muted-foreground">{t('drive.trashEmpty')}</p>
      ) : (
        <ul className="divide-y overflow-hidden rounded-2xl border bg-paper backdrop-blur-md">
          {items.map((item) => (
            <li key={`${item.kind}-${item.id}`} className="flex flex-wrap items-center gap-x-4 gap-y-2 px-3 py-3">
              <div className="grid min-w-0 flex-1 gap-1">
                <span className="flex min-w-0 items-center gap-2 font-medium">
                  {item.kind === 'folder' ? (
                    <FolderIcon className="size-4 shrink-0 text-amber-600" />
                  ) : (
                    <FileIcon className="size-4 shrink-0 text-muted-foreground" />
                  )}
                  <span className="truncate">{item.name}</span>
                </span>
                <span className="text-xs text-muted-foreground">
                  {t('drive.trashFrom', { path: [t('drive.root'), ...item.from].join(' / ') })} · {formatBytes(item.size, lang)} ·{' '}
                  {t('drive.trashPurgeAt', { when: formatRelative(lang, item.purgeAt, now) })}
                </span>
              </div>
              <div className="flex gap-1">
                <Button
                  size="sm"
                  variant="ghost"
                  disabled={changes.restore.isPending}
                  onClick={() =>
                    changes.restore.mutate(
                      { kind: item.kind, id: item.id },
                      { onSuccess: (restored) => toast.success(t('drive.restored', { name: restored.name })), onError },
                    )
                  }
                >
                  {t('drive.restore')}
                </Button>
                <Button size="sm" variant="ghost" className="text-destructive" onClick={() => setPurging(item)}>
                  {t('drive.purge')}
                </Button>
              </div>
            </li>
          ))}
        </ul>
      )}

      <AlertDialog open={purging !== null} onOpenChange={(open) => !open && setPurging(null)}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle className="break-all">{t('drive.purgeTitle', { name: purging?.name })}</AlertDialogTitle>
            <AlertDialogDescription>{t('drive.purgeHint')}</AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('drive.cancel')}</AlertDialogCancel>
            <AlertDialogAction
              variant="destructive"
              onClick={() => {
                if (purging)
                  changes.purge.mutate(
                    { kind: purging.kind, id: purging.id },
                    { onSuccess: () => toast.success(t('drive.purged')), onError },
                  )
              }}
            >
              {t('drive.purge')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
      <AlertDialog open={emptying} onOpenChange={setEmptying}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>{t('drive.emptyTrashTitle')}</AlertDialogTitle>
            <AlertDialogDescription>{t('drive.emptyTrashHint')}</AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('drive.cancel')}</AlertDialogCancel>
            <AlertDialogAction
              variant="destructive"
              onClick={() => changes.emptyTrash.mutate(undefined, { onSuccess: () => toast.success(t('drive.trashEmptied')), onError })}
            >
              {t('drive.emptyTrash')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  )
}
