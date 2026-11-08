import { RefreshCwIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { FormField } from '~/components/form'
import { Input } from '~/components/ui/input'
import type { useCaptcha } from '~/lib/queries'
import { cn } from '~/lib/utils'

// The picture of useCaptcha, as tall as an input next to it; clicking it asks for a new one
export function CaptchaImage({ captcha, className }: { captcha: ReturnType<typeof useCaptcha>; className?: string }) {
  const { t } = useTranslation()
  const { image } = captcha
  return (
    <button
      type="button"
      onClick={captcha.refresh}
      title={t('captcha.image')}
      aria-label={t('captcha.image')}
      className={cn(
        'relative flex h-8 w-[92px] shrink-0 items-center justify-center overflow-hidden rounded-lg border border-input bg-white text-xs text-neutral-500 outline-none focus-visible:ring-3 focus-visible:ring-ring/50',
        className,
      )}
    >
      {image ? (
        <img src={image.image} width={image.width} height={image.height} alt="" draggable={false} className="h-full w-full object-cover" />
      ) : captcha.isError ? (
        <span className="px-1 leading-tight">{t('captcha.failed')}</span>
      ) : (
        <span>{t('captcha.loading')}</span>
      )}
      {image && captcha.isLoading && (
        <span className="absolute inset-0 flex items-center justify-center bg-white/70">
          <RefreshCwIcon className="size-4 animate-spin" />
        </span>
      )}
    </button>
  )
}

// A form's captcha field (label, input, picture, hint, error); nothing when the server has it off.
// input: what react-hook-form's register('captcha') returns
export function CaptchaField({
  captcha,
  error,
  input,
}: {
  captcha: ReturnType<typeof useCaptcha>
  error?: string
  input: React.ComponentProps<'input'>
}) {
  const { t } = useTranslation()
  if (!captcha.enabled) return null
  return (
    <FormField label={t('captcha.label')} hint={t('captcha.hint')} error={error}>
      {(id, describedBy) => (
        <div className="flex gap-2">
          <Input
            id={id}
            autoComplete="off"
            autoCapitalize="characters"
            spellCheck={false}
            maxLength={16}
            placeholder={t('captcha.placeholder')}
            aria-describedby={describedBy}
            aria-invalid={!!error}
            {...input}
          />
          <CaptchaImage captcha={captcha} />
        </div>
      )}
    </FormField>
  )
}
