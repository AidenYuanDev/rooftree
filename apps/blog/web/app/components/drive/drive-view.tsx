import { DownloadIcon, EllipsisIcon, FolderPlusIcon, UploadIcon, XIcon } from 'lucide-react'
import { useCallback, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useSearchParams } from 'react-router'
import { toast } from 'sonner'
import { Breadcrumbs, FileList } from '~/components/drive/file-list'
import { FilePreview } from '~/components/drive/file-preview'
import { MoveDialog } from '~/components/drive/move-dialog'
import { NameDialog } from '~/components/drive/name-dialog'
import { ShareDialog } from '~/components/drive/share-dialog'
import { ShareList } from '~/components/drive/share-list'
import { TrashList } from '~/components/drive/trash-list'
import { UploadPanel } from '~/components/drive/uploads'
import { FormError } from '~/components/form'
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
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from '~/components/ui/dropdown-menu'
import { Skeleton } from '~/components/ui/skeleton'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '~/components/ui/tabs'
import { errorMessage } from '~/i18n'
import { fileUrl, formatBytes, zipUrl } from '~/lib/drive'
import { localePath, useLang } from '~/lib/lang'
import { useDrive, useDriveChanges } from '~/lib/queries'
import type { DriveFile, DriveFolder } from '~/lib/schemas'
import { thumbUrl } from '~/lib/thumbnail'
import { useUploads } from '~/lib/use-uploads'

type Item = { kind: 'file'; item: DriveFile } | { kind: 'folder'; item: DriveFolder }

function Usage({ used, quota, maxFileSize }: { used: number; quota: number; maxFileSize: number }) {
  const { t } = useTranslation()
  const lang = useLang()
  const percent = quota ? Math.min(100, (used / quota) * 100) : 100
  return (
    <div className="grid gap-1 text-xs text-muted-foreground">
      <div className="h-1.5 w-48 overflow-hidden rounded-full bg-muted">
        <div className={`h-full ${percent > 90 ? 'bg-destructive' : 'bg-primary'}`} style={{ width: `${percent}%` }} />
      </div>
      <span>
        {t('drive.usage', { used: formatBytes(used, lang), quota: formatBytes(quota, lang) })} ·{' '}
        {t('drive.maxFileSize', { size: formatBytes(maxFileSize, lang) })}
      </span>
    </div>
  )
}

function Drive({ owner, basePath }: { owner?: number; basePath: string }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const [params, setParams] = useSearchParams()
  const folder = Number(params.get('folder')) || null
  const listing = useDrive(folder, owner)
  const changes = useDriveChanges(owner)
  const refresh = changes.refresh
  const onUploaded = useCallback(() => void refresh(), [refresh])
  const uploads = useUploads(listing.data?.usage, onUploaded, owner)
  const input = useRef<HTMLInputElement>(null)
  const [dragging, setDragging] = useState(false)

  const [preview, setPreview] = useState<DriveFile | null>(null)
  const [creating, setCreating] = useState(false)
  const [renaming, setRenaming] = useState<Item | null>(null)
  const [moving, setMoving] = useState<Item | null>(null)
  const [deleting, setDeleting] = useState<Item | null>(null)
  const [sharing, setSharing] = useState<{ kind: 'file' | 'folder'; id: number; name: string } | null>(null)
  // What is chosen, in the folder it was chosen in
  const [selection, setSelection] = useState<{ folder: number | null; keys: Set<string> }>({ folder, keys: new Set() })
  const [deletingChosen, setDeletingChosen] = useState(false)

  const href = (id: number | null) => `${localePath(lang, basePath)}${id ? `?folder=${id}` : ''}`
  const onError = (error: unknown) => toast.error(errorMessage(i18n, error))

  const actions = (entry: Item) => (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button size="icon-sm" variant="ghost" aria-label={t('drive.actions')}>
          <EllipsisIcon />
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end">
        {entry.kind === 'file' ? (
          <>
            <DropdownMenuItem onSelect={() => setPreview(entry.item)}>{t('drive.open')}</DropdownMenuItem>
            <DropdownMenuItem asChild>
              <a href={fileUrl(entry.item.id, { owner, download: true })} download={entry.item.name}>
                {t('drive.download')}
              </a>
            </DropdownMenuItem>
          </>
        ) : (
          <>
            <DropdownMenuItem onSelect={() => setParams({ folder: String(entry.item.id) })}>{t('drive.open')}</DropdownMenuItem>
            <DropdownMenuItem asChild>
              <a href={zipUrl({ folder: entry.item.id, owner })}>{t('drive.downloadZip')}</a>
            </DropdownMenuItem>
          </>
        )}
        <DropdownMenuItem onSelect={() => setSharing({ kind: entry.kind, id: entry.item.id, name: entry.item.name })}>
          {t('drive.share')}
        </DropdownMenuItem>
        <DropdownMenuSeparator />
        <DropdownMenuItem onSelect={() => setRenaming(entry)}>{t('drive.rename')}</DropdownMenuItem>
        <DropdownMenuItem onSelect={() => setMoving(entry)}>{t('drive.move')}</DropdownMenuItem>
        <DropdownMenuItem variant="destructive" onSelect={() => setDeleting(entry)}>
          {t('drive.delete')}
        </DropdownMenuItem>
      </DropdownMenuContent>
    </DropdownMenu>
  )

  if (listing.isError) return <FormError error={listing.error} />
  const data = listing.data
  const trashDays = data?.usage.trashDays ?? 0
  // Only what is still shown here
  const shown = new Set([...(data?.folders ?? []).map((item) => `d${item.id}`), ...(data?.files ?? []).map((item) => `f${item.id}`)])
  const chosen = new Set(selection.folder === folder ? [...selection.keys].filter((key) => shown.has(key)) : [])
  const chosenIds = (prefix: string) => [...chosen].filter((key) => key.startsWith(prefix)).map((key) => Number(key.slice(1)))
  const choose = (keys: Set<string>) => setSelection({ folder, keys })

  return (
    <div
      className="relative grid gap-4"
      onDragOver={(event) => {
        if (!event.dataTransfer.types.includes('Files')) return
        event.preventDefault()
        setDragging(true)
      }}
      onDragLeave={(event) => {
        if (!event.currentTarget.contains(event.relatedTarget as Node | null)) setDragging(false)
      }}
      onDrop={(event) => {
        event.preventDefault()
        setDragging(false)
        // Folders dropped have no bytes of their own: only files
        const files = [...event.dataTransfer.files].filter((file) => file.type !== '' || file.size > 0)
        if (files.length) uploads.add(files, folder)
      }}
    >
      <div className="flex flex-wrap items-center justify-between gap-3">
        <Breadcrumbs root={t('drive.root')} rootHref={href(null)} path={data?.path ?? []} href={href} />
        <div className="flex flex-wrap gap-2">
          {data && (data.folders.length > 0 || data.files.length > 0) && (
            <Button variant="outline" size="sm" asChild>
              <a href={zipUrl({ folder: folder ?? undefined, owner })}>
                <DownloadIcon />
                {t('drive.downloadAll')}
              </a>
            </Button>
          )}
          <Button variant="outline" size="sm" onClick={() => setCreating(true)} disabled={!data}>
            <FolderPlusIcon />
            {t('drive.newFolder')}
          </Button>
          <Button size="sm" onClick={() => input.current?.click()} disabled={!data}>
            <UploadIcon />
            {t('drive.upload')}
          </Button>
          <input
            ref={input}
            type="file"
            multiple
            hidden
            data-testid="drive-upload"
            onChange={(event) => {
              uploads.add([...(event.target.files ?? [])], folder)
              event.target.value = ''
            }}
          />
        </div>
      </div>

      <UploadPanel uploads={uploads.uploads} onCancel={uploads.cancel} onRetry={uploads.retry} onClear={uploads.clear} />

      {chosen.size > 0 && (
        <div className="flex flex-wrap items-center gap-2 rounded-xl border bg-paper px-3 py-2 text-sm backdrop-blur-md">
          <span className="mr-auto font-medium">{t('drive.chosen', { count: chosen.size })}</span>
          <Button size="sm" variant="outline" asChild>
            <a href={zipUrl({ files: chosenIds('f'), folders: chosenIds('d'), name: data?.folder?.name, owner })}>
              <DownloadIcon />
              {t('drive.download')}
            </a>
          </Button>
          <Button size="sm" variant="outline" className="text-destructive" onClick={() => setDeletingChosen(true)}>
            {t('drive.delete')}
          </Button>
          <Button size="icon-sm" variant="ghost" aria-label={t('drive.clearChoice')} onClick={() => choose(new Set())}>
            <XIcon />
          </Button>
        </div>
      )}

      {!data ? (
        <Skeleton className="h-48 w-full" />
      ) : data.folders.length === 0 && data.files.length === 0 ? (
        <p className="rounded-2xl border border-dashed bg-paper px-4 py-16 text-center text-muted-foreground backdrop-blur-md">
          {t('drive.empty')}
        </p>
      ) : (
        <FileList
          folders={data.folders}
          files={data.files}
          folderHref={(item) => href(item.id)}
          onOpenFile={setPreview}
          folderActions={(item) => actions({ kind: 'folder', item })}
          fileActions={(item) => actions({ kind: 'file', item })}
          selection={{ selected: chosen, onChange: choose }}
          thumb={(item) => thumbUrl(item.id, { owner })}
        />
      )}
      {data && <Usage {...data.usage} />}
      {data && data.usage.quota > 0 && data.usage.used >= data.usage.quota * 0.9 && (
        <p role="alert" className="rounded-xl border border-destructive/40 bg-destructive/10 px-3 py-2 text-sm text-destructive">
          {data.usage.used >= data.usage.quota
            ? t(owner === undefined ? 'drive.full' : 'drive.fullOther')
            : t(owner === undefined ? 'drive.almostFull' : 'drive.almostFullOther', {
                left: formatBytes(data.usage.quota - data.usage.used, lang),
              })}
        </p>
      )}
      {owner === undefined && data?.usage.quota === 0 && <p className="text-sm text-muted-foreground">{t('drive.noSpace')}</p>}

      {dragging && (
        <div className="pointer-events-none absolute inset-0 grid place-items-center rounded-2xl border-2 border-dashed border-primary bg-background/80 text-lg font-medium">
          {t('drive.dropHere')}
        </div>
      )}

      <FilePreview file={preview} owner={owner} onClose={() => setPreview(null)} onThumbnail={onUploaded} />
      <NameDialog
        open={creating}
        onOpenChange={setCreating}
        title={t('drive.newFolder')}
        label={t('drive.folderName')}
        submit={t('drive.create')}
        onSubmit={(name) => changes.createFolder.mutateAsync({ parentId: folder, name })}
      />
      <NameDialog
        open={renaming !== null}
        onOpenChange={(open) => !open && setRenaming(null)}
        title={t('drive.rename')}
        label={t('drive.newName')}
        initial={renaming?.item.name}
        submit={t('drive.save')}
        onSubmit={async (name) => {
          if (!renaming) return
          if (renaming.kind === 'file') await changes.updateFile.mutateAsync({ id: renaming.item.id, name })
          else await changes.updateFolder.mutateAsync({ id: renaming.item.id, name })
          toast.success(t('drive.renamed'))
        }}
      />
      <MoveDialog
        name={moving?.item.name ?? null}
        folder={moving?.kind === 'folder' ? moving.item.id : undefined}
        current={folder}
        owner={owner}
        onClose={() => setMoving(null)}
        onMove={(target) => {
          if (!moving) return
          const done = { onSuccess: () => toast.success(t('drive.moved')), onError }
          if (moving.kind === 'file') changes.updateFile.mutate({ id: moving.item.id, folderId: target }, done)
          else changes.updateFolder.mutate({ id: moving.item.id, parentId: target }, done)
          setMoving(null)
        }}
      />
      <ShareDialog target={sharing} owner={owner} onClose={() => setSharing(null)} />
      <AlertDialog open={deletingChosen} onOpenChange={setDeletingChosen}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>{t('drive.deleteChosenTitle', { count: chosen.size })}</AlertDialogTitle>
            <AlertDialogDescription>
              {trashDays > 0 ? t('drive.trashChosenHint', { count: trashDays }) : t('drive.deleteChosenHint')}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('drive.cancel')}</AlertDialogCancel>
            <AlertDialogAction
              variant="destructive"
              onClick={() => {
                const work = [
                  ...chosenIds('d').map((id) => changes.deleteFolder.mutateAsync(id)),
                  ...chosenIds('f').map((id) => changes.deleteFile.mutateAsync(id)),
                ]
                choose(new Set())
                Promise.all(work).then(() => toast.success(t(trashDays > 0 ? 'drive.trashed' : 'drive.deleted')), onError)
              }}
            >
              {t('drive.delete')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
      <AlertDialog open={deleting !== null} onOpenChange={(open) => !open && setDeleting(null)}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle className="break-all">
              {deleting?.kind === 'folder'
                ? t('drive.deleteFolderTitle', { name: deleting.item.name })
                : t('drive.deleteFileTitle', { name: deleting?.item.name })}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {trashDays > 0
                ? t(deleting?.kind === 'folder' ? 'drive.trashFolderHint' : 'drive.trashFileHint', { count: trashDays })
                : t(deleting?.kind === 'folder' ? 'drive.deleteFolderHint' : 'drive.deleteFileHint')}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('drive.cancel')}</AlertDialogCancel>
            <AlertDialogAction
              variant="destructive"
              onClick={() => {
                if (!deleting) return
                const done = {
                  onSuccess: ({ trashed }: { trashed: boolean }) => toast.success(t(trashed ? 'drive.trashed' : 'drive.deleted')),
                  onError,
                }
                if (deleting.kind === 'file') changes.deleteFile.mutate(deleting.item.id, done)
                else changes.deleteFolder.mutate(deleting.item.id, done)
              }}
            >
              {t('drive.delete')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  )
}

const tabs = ['files', 'shares', 'trash'] as const

// A drive's files, links and trash, at basePath (?folder=, ?tab=shares|trash): the account's own, or
// for the admin another account's (owner)
export function DriveTabs({ owner, basePath }: { owner?: number; basePath: string }) {
  const { t } = useTranslation()
  const [params, setParams] = useSearchParams()
  const tab = tabs.find((name) => name === params.get('tab')) ?? 'files'
  return (
    <Tabs
      value={tab}
      onValueChange={(value) =>
        // The folder stays: back on the files, the same one is shown
        setParams((current) => {
          const next = new URLSearchParams(current)
          if (value === 'files') next.delete('tab')
          else next.set('tab', value)
          return next
        })
      }
    >
      <TabsList className="mb-4">
        <TabsTrigger value="files">{t('drive.filesTab')}</TabsTrigger>
        <TabsTrigger value="shares">{t('drive.sharesTab')}</TabsTrigger>
        <TabsTrigger value="trash">{t('drive.trashTab')}</TabsTrigger>
      </TabsList>
      {/* Kept mounted: uploads go on while the links are shown */}
      <TabsContent value="files" forceMount className="data-[state=inactive]:hidden">
        <Drive owner={owner} basePath={basePath} />
      </TabsContent>
      <TabsContent value="shares">
        <ShareList owner={owner} />
      </TabsContent>
      <TabsContent value="trash">
        <TrashList owner={owner} />
      </TabsContent>
    </Tabs>
  )
}
