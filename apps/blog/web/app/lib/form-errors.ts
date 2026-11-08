import type { FieldValues, Path, UseFormReturn } from 'react-hook-form'
import { ApiError } from './api'

// A failed request's field errors onto the form's fields. True if any landed, the general message is
// then not needed
export function applyFieldErrors<T extends FieldValues>(form: UseFormReturn<T>, error: unknown): boolean {
  if (!(error instanceof ApiError)) return false

  let applied = false
  for (const [field, code] of Object.entries(error.fields)) {
    form.setError(field as Path<T>, { type: 'server', message: code })
    applied = true
  }
  return applied
}
