import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogFooter, DialogHeader, DialogTitle, DialogTrigger } from '~/components/ui/dialog'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { errorMessage } from '~/i18n'
import { useBanUser } from '~/lib/queries'

export function BanDialog({ userId, name }: { userId: number; name: string }) {
  const { t, i18n } = useTranslation()
  const ban = useBanUser()
  const [open, setOpen] = useState(false)
  const [days, setDays] = useState('7')
  const [reason, setReason] = useState('')

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>
        <Button variant="ghost" size="xs">
          {t('admin.ban')}
        </Button>
      </DialogTrigger>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>{t('admin.banTitle', { name })}</DialogTitle>
        </DialogHeader>
        <div className="grid gap-4">
          <div className="grid gap-1.5">
            <Label htmlFor={`ban-days-${userId}`}>{t('admin.banDays')}</Label>
            <Input
              id={`ban-days-${userId}`}
              inputMode="numeric"
              value={days}
              onChange={(event) => setDays(event.target.value.replace(/\D/g, ''))}
            />
          </div>
          <div className="grid gap-1.5">
            <Label htmlFor={`ban-reason-${userId}`}>{t('admin.banReason')}</Label>
            <Input id={`ban-reason-${userId}`} value={reason} maxLength={255} onChange={(event) => setReason(event.target.value)} />
          </div>
        </div>
        <DialogFooter>
          <Button variant="ghost" onClick={() => setOpen(false)}>
            {t('admin.cancel')}
          </Button>
          <Button
            variant="destructive"
            disabled={ban.isPending}
            onClick={() =>
              ban.mutate(
                { id: userId, days: days ? Number(days) : null, reason },
                { onSuccess: () => setOpen(false), onError: (error) => toast.error(errorMessage(i18n, error)) },
              )
            }
          >
            {t('admin.ban')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
