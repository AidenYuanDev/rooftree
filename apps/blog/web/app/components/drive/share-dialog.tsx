import { CopyIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '~/components/ui/select'
import { errorMessage } from '~/i18n'
import { copyLink, shareUrl } from '~/lib/drive'
import { useLang } from '~/lib/lang'
import { useCreateShare } from '~/lib/queries'
import { type ShareExpiry, shareExpiries } from '~/lib/schemas'

export function LinkField({ url }: { url: string }) {
  const { t } = useTranslation()
  return (
    <div className="flex gap-2">
      <Input readOnly value={url} aria-label={t('drive.copy')} onFocus={(event) => event.target.select()} />
      <Button type="button" variant="outline" onClick={() => void copyLink(url, t('drive.copied'))}>
        <CopyIcon />
        {t('drive.copy')}
      </Button>
    </div>
  )
}

// A link to a file or folder: how long, with a password or not; then the link to copy
export function ShareDialog({
  target,
  owner,
  onClose,
}: {
  owner?: number

  target: { kind: 'file' | 'folder'; id: number; name: string } | null
  onClose: () => void
}) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const create = useCreateShare(owner)
  const [expires, setExpires] = useState<ShareExpiry>('7d')
  const [password, setPassword] = useState('')
  const [url, setUrl] = useState<string | null>(null)

  const reset = () => {
    setExpires('7d')
    setPassword('')
    setUrl(null)
    create.reset()
  }

  return (
    <Dialog
      open={target !== null}
      onOpenChange={(open) => {
        if (open) return
        onClose()
        reset()
      }}
    >
      <DialogContent>
        <DialogHeader>
          <DialogTitle className="truncate pr-6">{t('drive.shareTitle', { name: target?.name })}</DialogTitle>
          <DialogDescription>{t('drive.shareHint')}</DialogDescription>
        </DialogHeader>
        {url ? (
          <LinkField url={url} />
        ) : (
          <div className="grid gap-4">
            <div className="grid gap-1.5">
              <Label htmlFor="share-expires">{t('drive.expires')}</Label>
              <Select value={expires} onValueChange={(value) => setExpires(value as ShareExpiry)}>
                <SelectTrigger id="share-expires" className="w-40">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  {shareExpiries.map((expiry) => (
                    <SelectItem key={expiry} value={expiry}>
                      {t(`drive.expiry.${expiry}`)}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
            <div className="grid gap-1.5">
              <Label htmlFor="share-password">{t('drive.password')}</Label>
              <Input
                id="share-password"
                value={password}
                maxLength={128}
                autoComplete="off"
                onChange={(event) => setPassword(event.target.value)}
              />
              <p className="text-xs text-muted-foreground">{t('drive.passwordHint')}</p>
            </div>
          </div>
        )}
        <DialogFooter>
          {url ? (
            <Button
              onClick={() => {
                onClose()
                reset()
              }}
            >
              {t('common.close')}
            </Button>
          ) : (
            <Button
              disabled={create.isPending || !target}
              onClick={() => {
                if (!target) return
                create.mutate(
                  { [target.kind === 'file' ? 'fileId' : 'folderId']: target.id, expires, ...(password ? { password } : {}) },
                  {
                    onSuccess: (share) => {
                      const link = shareUrl(lang, share.token)
                      setUrl(link)
                      void copyLink(link, t('drive.copied'))
                    },
                    onError: (error) => toast.error(errorMessage(i18n, error)),
                  },
                )
              }}
            >
              {t('drive.createLink')}
            </Button>
          )}
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
