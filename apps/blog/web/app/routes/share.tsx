import { DownloadIcon, LockIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useParams, useSearchParams } from 'react-router'
import type { Route } from './+types/share'
import { Breadcrumbs, FileList, FileTypeIcon } from '~/components/drive/file-list'
import { FilePreview } from '~/components/drive/file-preview'
import { FormError } from '~/components/form'
import { PageHeader } from '~/components/page-header'
import { RequireUser } from '~/components/require-user'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Skeleton } from '~/components/ui/skeleton'
import { errorMessage } from '~/i18n'
import { ApiError } from '~/lib/api'
import { fileKind, fileUrl, formatBytes, formatDate, shareZipUrl } from '~/lib/drive'
import { localePath, useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { useShare, useUnlockShare } from '~/lib/queries'
import type { DriveFile } from '~/lib/schemas'
import { thumbUrl } from '~/lib/thumbnail'

// What someone shared from their drive, for logged in readers: a file, or a folder to look through,
// read only. A link with a password asks for it once. Not prerendered (the SPA fallback serves it)

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'share.title')

function Unlock({ token }: { token: string }) {
  const { t, i18n } = useTranslation()
  const unlock = useUnlockShare(token)
  const [password, setPassword] = useState('')
  return (
    <form
      className="mx-auto grid max-w-sm gap-4 rounded-2xl border bg-paper p-6 backdrop-blur-md"
      onSubmit={(event) => {
        event.preventDefault()
        unlock.mutate(password)
      }}
    >
      <p className="flex items-center gap-2 font-medium">
        <LockIcon className="size-4" />
        {t('share.locked')}
      </p>
      <div className="grid gap-1.5">
        <Label htmlFor="share-password">{t('share.password')}</Label>
        <Input id="share-password" type="password" value={password} autoFocus onChange={(event) => setPassword(event.target.value)} />
        {unlock.error && (
          <p className="text-xs text-destructive" role="alert">
            {errorMessage(i18n, unlock.error)}
          </p>
        )}
      </div>
      <Button type="submit" disabled={!password || unlock.isPending}>
        {t('share.unlock')}
      </Button>
    </form>
  )
}

function SharedFile({ file, token }: { file: DriveFile; token: string }) {
  const { t } = useTranslation()
  const lang = useLang()
  const url = fileUrl(file.id, { token })
  const kind = fileKind(file.mime)
  return (
    <div className="grid gap-4 rounded-2xl border bg-paper p-4 backdrop-blur-md">
      <div className="flex flex-wrap items-center gap-3">
        <FileTypeIcon file={file} />
        <div className="min-w-0 flex-1">
          <p className="truncate font-medium">{file.name}</p>
          <p className="text-xs text-muted-foreground">{formatBytes(file.size, lang)}</p>
        </div>
        <Button asChild>
          <a href={fileUrl(file.id, { token, download: true })} download={file.name}>
            <DownloadIcon />
            {t('drive.download')}
          </a>
        </Button>
      </div>
      {kind === 'image' && <img src={url} alt={file.name} className="mx-auto max-h-[75vh] rounded-md object-contain" />}
      {kind === 'video' && <video src={url} controls className="max-h-[75vh] w-full rounded-md bg-black" />}
      {kind === 'audio' && <audio src={url} controls className="w-full" />}
      {kind === 'pdf' && <iframe src={url} title={file.name} className="h-[75vh] w-full rounded-md border" />}
      {kind === 'other' && <p className="text-sm text-muted-foreground">{t('drive.noPreview')}</p>}
    </div>
  )
}

function Share({ token }: { token: string }) {
  const { t } = useTranslation()
  const lang = useLang()
  const [params] = useSearchParams()
  const folder = Number(params.get('folder')) || null
  const share = useShare(token, folder, true)
  const [preview, setPreview] = useState<DriveFile | null>(null)

  if (share.isPending) return <Skeleton className="h-48 w-full" />
  if (share.error) {
    const gone = share.error instanceof ApiError && (share.error.status === 404 || share.error.status === 410)
    if (!gone) return <FormError error={share.error} />
    return (
      <p className="py-16 text-center text-muted-foreground">
        {share.error instanceof ApiError && share.error.status === 410 ? t('share.expired') : t('share.notFound')}
      </p>
    )
  }

  const { share: info, file, path = [], folders = [], files = [] } = share.data
  const href = (id: number | null) => `${localePath(lang, `/s/${token}`)}${id ? `?folder=${id}` : ''}`

  return (
    <div className="grid gap-4">
      <p className="text-sm text-muted-foreground">
        {t('share.from', { owner: info.owner })}
        {info.expiresAt && ` · ${t('share.until', { date: formatDate(info.expiresAt, lang) })}`}
      </p>
      {info.isOwner && <p className="text-sm text-muted-foreground">{t('share.isOwner')}</p>}
      {info.locked ? (
        <Unlock token={token} />
      ) : file ? (
        <SharedFile file={file} token={token} />
      ) : (
        <>
          <div className="flex flex-wrap items-center justify-between gap-3">
            {/* The link's folder is the top: its own name, no drive above it */}
            <Breadcrumbs root={path[0]?.name ?? ''} rootHref={href(null)} path={path.slice(1)} href={href} />
            {(folders.length > 0 || files.length > 0) && (
              <Button variant="outline" size="sm" asChild>
                <a href={shareZipUrl(token, folder)}>
                  <DownloadIcon />
                  {t('drive.downloadAll')}
                </a>
              </Button>
            )}
          </div>
          {folders.length === 0 && files.length === 0 ? (
            <p className="py-10 text-center text-muted-foreground">{t('share.emptyFolder')}</p>
          ) : (
            <FileList
              folders={folders}
              files={files}
              folderHref={(item) => href(item.id)}
              onOpenFile={setPreview}
              thumb={(item) => thumbUrl(item.id, { token })}
              fileActions={(item) => (
                <Button size="icon-sm" variant="ghost" asChild aria-label={t('drive.download')}>
                  <a href={fileUrl(item.id, { token, download: true })} download={item.name}>
                    <DownloadIcon />
                  </a>
                </Button>
              )}
            />
          )}
          <FilePreview file={preview} token={token} onClose={() => setPreview(null)} />
        </>
      )}
    </div>
  )
}

export default function SharePage() {
  const { t } = useTranslation()
  const token = useParams().token ?? ''
  return (
    <div className="mx-auto max-w-5xl">
      <PageHeader title={t('share.title')} />
      <RequireUser prompt={t('share.loginRequired')}>{() => <Share key={token} token={token} />}</RequireUser>
    </div>
  )
}
