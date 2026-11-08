import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { errorMessage } from '~/i18n'
import { isValidName } from '~/lib/drive'

// A name to give: a new folder's, or a file's or folder's new one. onSubmit throws the API's error,
// shown under the field
export function NameDialog({
  open,
  onOpenChange,
  title,
  label,
  initial = '',
  submit,
  onSubmit,
}: {
  open: boolean
  onOpenChange: (open: boolean) => void
  title: string
  label: string
  initial?: string
  submit: string
  onSubmit: (name: string) => Promise<unknown>
}) {
  const { t, i18n } = useTranslation()
  const [name, setName] = useState(initial)
  const [error, setError] = useState<string | null>(null)
  const [pending, setPending] = useState(false)

  const save = async (event: React.FormEvent) => {
    event.preventDefault()
    setPending(true)
    try {
      await onSubmit(name.trim())
      onOpenChange(false)
    } catch (caught) {
      setError(errorMessage(i18n, caught))
    } finally {
      setPending(false)
    }
  }

  return (
    <Dialog
      open={open}
      onOpenChange={(next) => {
        if (next) {
          setName(initial)
          setError(null)
        }
        onOpenChange(next)
      }}
    >
      <DialogContent>
        <form onSubmit={(event) => void save(event)} className="grid gap-4">
          <DialogHeader>
            <DialogTitle className="truncate pr-6">{title}</DialogTitle>
          </DialogHeader>
          <div className="grid gap-1.5">
            <Label htmlFor="drive-name">{label}</Label>
            <Input
              id="drive-name"
              value={name}
              maxLength={255}
              autoFocus
              aria-invalid={error ? true : undefined}
              onFocus={(event) => {
                // The name without its extension, for renaming
                const dot = event.target.value.lastIndexOf('.')
                event.target.setSelectionRange(0, dot > 0 ? dot : event.target.value.length)
              }}
              onChange={(event) => {
                setName(event.target.value)
                setError(null)
              }}
            />
            {error && (
              <p className="text-xs text-destructive" role="alert">
                {error}
              </p>
            )}
          </div>
          <DialogFooter>
            <Button type="button" variant="ghost" onClick={() => onOpenChange(false)}>
              {t('drive.cancel')}
            </Button>
            <Button type="submit" disabled={pending || !isValidName(name) || name.trim() === initial}>
              {submit}
            </Button>
          </DialogFooter>
        </form>
      </DialogContent>
    </Dialog>
  )
}
