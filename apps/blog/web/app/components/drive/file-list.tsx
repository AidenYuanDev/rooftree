import { FileArchiveIcon, FileAudioIcon, FileIcon, FileImageIcon, FileTextIcon, FileVideoIcon, FolderIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { LocalDate } from '~/components/local-date'
import { fileKind, formatBytes } from '~/lib/drive'
import { useLang } from '~/lib/lang'
import type { DriveFile, DriveFolder } from '~/lib/schemas'

export function FileTypeIcon({ file }: { file: DriveFile }) {
  const className = 'size-5 shrink-0 text-muted-foreground'
  switch (fileKind(file.mime)) {
    case 'image':
      return <FileImageIcon className={className} />
    case 'video':
      return <FileVideoIcon className={className} />
    case 'audio':
      return <FileAudioIcon className={className} />
    case 'pdf':
      return <FileTextIcon className={className} />
    default:
      if (/zip|tar|gzip|7z/.test(file.mime)) return <FileArchiveIcon className={className} />
      if (file.mime.startsWith('text/')) return <FileTextIcon className={className} />
      return <FileIcon className={className} />
  }
}

// What is chosen in a list, by key: d<id> a folder, f<id> a file
export type Selection = { selected: ReadonlySet<string>; onChange: (selected: Set<string>) => void }

const checkbox = 'size-4 accent-primary'

// A folder's folders, then its files: a folder opens by its link, a file by onOpenFile (a preview).
// actions: a row's menu, none on a link's read only pages. selection: a box on each row
export function FileList({
  folders,
  files,
  folderHref,
  onOpenFile,
  folderActions,
  fileActions,
  selection,
  thumb,
}: {
  folders: DriveFolder[]
  files: DriveFile[]
  folderHref: (folder: DriveFolder) => string
  onOpenFile: (file: DriveFile) => void
  folderActions?: (folder: DriveFolder) => React.ReactNode
  fileActions?: (file: DriveFile) => React.ReactNode
  selection?: Selection
  // The address of a file's small picture
  thumb?: (file: DriveFile) => string
}) {
  const { t } = useTranslation()
  const lang = useLang()
  const row = selection
    ? 'grid grid-cols-[auto_1fr_auto] items-center gap-3 px-3 py-2 md:grid-cols-[auto_1fr_6rem_10rem_2rem]'
    : 'grid grid-cols-[1fr_auto] items-center gap-3 px-3 py-2 md:grid-cols-[1fr_6rem_10rem_2rem]'
  const keys = [...folders.map((folder) => `d${folder.id}`), ...files.map((file) => `f${file.id}`)]
  const allChosen = selection !== undefined && keys.length > 0 && keys.every((key) => selection.selected.has(key))
  const box = (key: string, name: string) =>
    selection && (
      <input
        type="checkbox"
        className={checkbox}
        aria-label={t('drive.select', { name })}
        checked={selection.selected.has(key)}
        onChange={(event) => {
          const next = new Set(selection.selected)
          if (event.target.checked) next.add(key)
          else next.delete(key)
          selection.onChange(next)
        }}
      />
    )

  return (
    <div className="overflow-hidden rounded-2xl border bg-paper backdrop-blur-md">
      <div className={`${row} border-b text-xs text-muted-foreground max-md:hidden`}>
        {selection && (
          <input
            type="checkbox"
            className={checkbox}
            aria-label={t('drive.selectAll')}
            checked={allChosen}
            onChange={(event) => selection.onChange(new Set(event.target.checked ? keys : []))}
          />
        )}
        <span>{t('drive.name')}</span>
        <span className="text-right">{t('drive.size')}</span>
        <span>{t('drive.modified')}</span>
        <span className="sr-only">{t('drive.actions')}</span>
      </div>
      <ul className="divide-y">
        {folders.map((folder) => (
          <li key={`d${folder.id}`} className={`${row} hover:bg-muted/40`}>
            {box(`d${folder.id}`, folder.name)}
            <Link to={folderHref(folder)} className="flex min-w-0 items-center gap-2 font-medium">
              <FolderIcon className="size-5 shrink-0 fill-amber-300/60 text-amber-600 dark:text-amber-400" />
              <span className="truncate">{folder.name}</span>
            </Link>
            <span className="text-right text-sm text-muted-foreground max-md:hidden">—</span>
            <span className="text-sm text-muted-foreground max-md:hidden">
              <LocalDate iso={folder.createdAt} lang={lang} />
            </span>
            <span className="flex justify-end">{folderActions?.(folder)}</span>
          </li>
        ))}
        {files.map((file) => (
          <li key={`f${file.id}`} className={`${row} hover:bg-muted/40`}>
            {box(`f${file.id}`, file.name)}
            <button type="button" className="flex min-w-0 items-center gap-2 text-left" onClick={() => onOpenFile(file)}>
              {file.thumb && thumb ? (
                <img src={thumb(file)} alt="" loading="lazy" className="size-10 shrink-0 rounded-md bg-muted object-cover" />
              ) : (
                <FileTypeIcon file={file} />
              )}
              <span className="min-w-0">
                <span className="block truncate">{file.name}</span>
                <span className="block text-xs text-muted-foreground md:hidden">{formatBytes(file.size, lang)}</span>
              </span>
            </button>
            <span className="text-right text-sm text-muted-foreground tabular-nums max-md:hidden">{formatBytes(file.size, lang)}</span>
            <span className="text-sm text-muted-foreground max-md:hidden">
              <LocalDate iso={file.updatedAt} lang={lang} />
            </span>
            <span className="flex justify-end">{fileActions?.(file)}</span>
          </li>
        ))}
      </ul>
    </div>
  )
}

// The way from the top (the drive's root or a link's folder) down to the folder shown
export function Breadcrumbs({
  root,
  rootHref,
  path,
  href,
}: {
  root: string
  rootHref: string
  path: { id: number; name: string }[]
  href: (id: number) => string
}) {
  return (
    <nav className="flex min-w-0 flex-wrap items-center gap-1 text-sm" aria-label="breadcrumb">
      {path.length === 0 ? (
        <span className="font-medium">{root}</span>
      ) : (
        <Link to={rootHref} className="text-muted-foreground hover:text-foreground">
          {root}
        </Link>
      )}
      {path.map((step, index) => (
        <span key={step.id} className="flex min-w-0 items-center gap-1">
          <span className="text-muted-foreground">/</span>
          {index === path.length - 1 ? (
            <span className="truncate font-medium">{step.name}</span>
          ) : (
            <Link to={href(step.id)} className="truncate text-muted-foreground hover:text-foreground">
              {step.name}
            </Link>
          )}
        </span>
      ))}
    </nav>
  )
}
