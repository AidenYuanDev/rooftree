import { useId } from 'react'
import { useTranslation } from 'react-i18next'
import { Alert, AlertDescription } from '~/components/ui/alert'
import { Label } from '~/components/ui/label'
import { errorMessage, fieldErrorMessage } from '~/i18n'

// Zod messages are error codes ("too_short"), like the server's fields; both are shown translated
export function FormField({
  label,
  error,
  hint,
  children,
}: {
  label: string
  error?: string
  hint?: string
  children: (id: string, describedBy?: string) => React.ReactNode
}) {
  const { i18n } = useTranslation()
  const id = useId()
  const hintId = `${id}-hint`
  const errorId = `${id}-error`
  const describedBy = [hint ? hintId : null, error ? errorId : null].filter(Boolean).join(' ') || undefined
  return (
    <div className="grid gap-1.5">
      <Label htmlFor={id}>{label}</Label>
      {children(id, describedBy)}
      {hint && !error && (
        <p id={hintId} className="text-xs text-muted-foreground">
          {hint}
        </p>
      )}
      {error && (
        <p id={errorId} className="text-xs text-destructive" role="alert">
          {fieldErrorMessage(i18n, error)}
        </p>
      )}
    </div>
  )
}

export function FormError({ error }: { error: unknown }) {
  const { i18n } = useTranslation()
  if (!error) return null
  return (
    <Alert variant="destructive">
      <AlertDescription>{errorMessage(i18n, error)}</AlertDescription>
    </Alert>
  )
}
