import { FolderIcon, HardDriveIcon } from 'lucide-react'
import { useMemo, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { Skeleton } from '~/components/ui/skeleton'
import { folderTree } from '~/lib/drive'
import { useDriveFolders } from '~/lib/queries'
import { cn } from '~/lib/utils'

// Where to move a file or folder (folder: the one being moved). current: where it is now
export function MoveDialog({
  name,
  folder,
  current,
  owner,
  onClose,
  onMove,
}: {
  name: string | null
  folder?: number
  current: number | null
  owner?: number
  onClose: () => void
  onMove: (target: number | null) => void
}) {
  const { t } = useTranslation()
  const folders = useDriveFolders(name !== null, owner)
  const [target, setTarget] = useState<number | null>(current)
  const tree = useMemo(() => folderTree(folders.data ?? [], folder), [folders.data, folder])

  const option = (id: number | null, label: string, depth: number, icon: React.ReactNode) => (
    <button
      key={id ?? 'root'}
      type="button"
      onClick={() => setTarget(id)}
      className={cn(
        'flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left text-sm hover:bg-muted',
        target === id && 'bg-muted font-medium',
      )}
      style={{ paddingLeft: `${0.5 + depth * 1.25}rem` }}
      aria-pressed={target === id}
    >
      {icon}
      <span className="truncate">{label}</span>
    </button>
  )

  return (
    <Dialog
      open={name !== null}
      onOpenChange={(open) => {
        if (open) return
        onClose()
      }}
    >
      <DialogContent onOpenAutoFocus={() => setTarget(current)}>
        <DialogHeader>
          <DialogTitle className="truncate pr-6">{t('drive.moveTitle', { name })}</DialogTitle>
        </DialogHeader>
        <div className="max-h-80 overflow-y-auto rounded-md border p-1">
          {folders.isPending ? (
            <Skeleton className="h-24 w-full" />
          ) : (
            <>
              {option(null, t('drive.root'), 0, <HardDriveIcon className="size-4 shrink-0" />)}
              {tree.map(({ folder: item, depth }) =>
                option(item.id, item.name, depth + 1, <FolderIcon className="size-4 shrink-0 text-amber-600" />),
              )}
            </>
          )}
        </div>
        <DialogFooter>
          <Button variant="ghost" onClick={onClose}>
            {t('drive.cancel')}
          </Button>
          <Button disabled={target === current} onClick={() => onMove(target)}>
            {t('drive.moveHere')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
