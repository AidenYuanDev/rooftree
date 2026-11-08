import { DownloadIcon, ExternalLinkIcon } from 'lucide-react'
import { useEffect } from 'react'
import { useTranslation } from 'react-i18next'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { fileKind, fileUrl, formatBytes } from '~/lib/drive'
import { useLang } from '~/lib/lang'
import type { DriveFile } from '~/lib/schemas'
import { sendThumbnail } from '~/lib/thumbnail'

// A file shown in the page when the browser can (pictures, audio, video, PDF), with its download.
// token: a file of that link. A picture of the drive without a small picture (uploaded before there
// were any) gets one, then onThumbnail
export function FilePreview({
  file,
  token,
  owner,
  onClose,
  onThumbnail,
}: {
  file: DriveFile | null
  token?: string
  owner?: number
  onClose: () => void
  onThumbnail?: () => void
}) {
  const { t } = useTranslation()
  const lang = useLang()

  useEffect(() => {
    if (!file || file.thumb || token !== undefined || !onThumbnail || fileKind(file.mime) !== 'image') return
    const controller = new AbortController()
    fetch(fileUrl(file.id, { owner }), { credentials: 'same-origin', signal: controller.signal })
      .then((response) => (response.ok ? response.blob() : null))
      .then((blob) => blob && sendThumbnail(file.id, blob, file.mime, owner))
      .then((sent) => sent && onThumbnail())
      .catch(() => {})
    return () => controller.abort()
  }, [file, token, owner, onThumbnail])

  return (
    <Dialog open={file !== null} onOpenChange={(open) => !open && onClose()}>
      <DialogContent className="sm:max-w-3xl">
        {file && <Preview file={file} token={token} owner={owner} />}
        {file && (
          <DialogFooter className="items-center sm:justify-between">
            <span className="text-sm text-muted-foreground">{formatBytes(file.size, lang)}</span>
            <div className="flex gap-2">
              {fileKind(file.mime) !== 'other' && (
                <Button variant="outline" asChild>
                  <a href={fileUrl(file.id, { token, owner })} target="_blank" rel="noreferrer">
                    <ExternalLinkIcon />
                    {t('drive.openInNewTab')}
                  </a>
                </Button>
              )}
              <Button asChild>
                <a href={fileUrl(file.id, { token, owner, download: true })} download={file.name}>
                  <DownloadIcon />
                  {t('drive.download')}
                </a>
              </Button>
            </div>
          </DialogFooter>
        )}
      </DialogContent>
    </Dialog>
  )
}

function Preview({ file, token, owner }: { file: DriveFile; token?: string; owner?: number }) {
  const { t } = useTranslation()
  const url = fileUrl(file.id, { token, owner })
  const kind = fileKind(file.mime)
  return (
    <>
      <DialogHeader>
        <DialogTitle className="truncate pr-6">{file.name}</DialogTitle>
        <DialogDescription className={kind === 'other' ? undefined : 'sr-only'}>
          {kind === 'other' ? t('drive.noPreview') : file.mime}
        </DialogDescription>
      </DialogHeader>
      {kind === 'image' && <img src={url} alt={file.name} className="mx-auto max-h-[70vh] rounded-md object-contain" />}
      {kind === 'video' && <video src={url} controls autoPlay className="max-h-[70vh] w-full rounded-md bg-black" />}
      {kind === 'audio' && <audio src={url} controls autoPlay className="w-full" />}
      {kind === 'pdf' && <iframe src={url} title={file.name} className="h-[70vh] w-full rounded-md border" />}
    </>
  )
}
