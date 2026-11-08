import { CheckIcon, RotateCwIcon, XIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Button } from '~/components/ui/button'
import { formatBytes } from '~/lib/drive'
import { useLang } from '~/lib/lang'
import type { Upload } from '~/lib/use-uploads'

export function UploadPanel({
  uploads,
  onCancel,
  onRetry,
  onClear,
}: {
  uploads: Upload[]
  onCancel: (key: number) => void
  onRetry: (key: number) => void
  onClear: () => void
}) {
  const { t } = useTranslation()
  const lang = useLang()
  if (uploads.length === 0) return null

  return (
    <section className="rounded-2xl border bg-paper p-3 backdrop-blur-md" aria-label={t('drive.uploads')}>
      <div className="mb-2 flex items-center justify-between">
        <h2 className="text-sm font-medium">{t('drive.uploads')}</h2>
        {uploads.some((upload) => upload.state !== 'waiting' && upload.state !== 'uploading') && (
          <Button size="xs" variant="ghost" onClick={onClear}>
            {t('drive.clearDone')}
          </Button>
        )}
      </div>
      <ul className="grid max-h-60 gap-2 overflow-y-auto">
        {uploads.map((upload) => {
          const percent = upload.file.size ? Math.floor((upload.sent / upload.file.size) * 100) : 100
          return (
            <li key={upload.key} className="grid gap-1">
              <div className="flex items-center gap-2 text-sm">
                <span className="min-w-0 flex-1 truncate">{upload.file.name}</span>
                <span className="shrink-0 text-xs text-muted-foreground tabular-nums">
                  {upload.state === 'uploading' && t('drive.uploading', { percent })}
                  {upload.state === 'waiting' && t('drive.waiting')}
                  {upload.state === 'cancelled' && t('drive.uploadCancelled')}
                  {upload.state === 'done' && <CheckIcon className="size-4 text-emerald-600" aria-label={t('drive.uploaded')} />}
                  {upload.state !== 'done' && upload.state !== 'failed' && ` · ${formatBytes(upload.file.size, lang)}`}
                </span>
                {(upload.state === 'waiting' || upload.state === 'uploading') && (
                  <Button size="icon-xs" variant="ghost" aria-label={t('drive.cancelUpload')} onClick={() => onCancel(upload.key)}>
                    <XIcon />
                  </Button>
                )}
                {(upload.state === 'failed' || upload.state === 'cancelled') && (
                  <Button size="icon-xs" variant="ghost" aria-label={t('drive.retry')} onClick={() => onRetry(upload.key)}>
                    <RotateCwIcon />
                  </Button>
                )}
              </div>
              {upload.state === 'uploading' && (
                <div className="h-1 overflow-hidden rounded-full bg-muted">
                  <div className="h-full bg-primary transition-[width]" style={{ width: `${percent}%` }} />
                </div>
              )}
              {upload.state === 'failed' && (
                <p className="text-xs text-destructive" role="alert">
                  {t('drive.uploadFailed', { error: upload.error })}
                </p>
              )}
            </li>
          )
        })}
      </ul>
    </section>
  )
}
