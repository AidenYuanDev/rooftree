import { ImageUpIcon } from 'lucide-react'
import { useRef } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { Button } from '~/components/ui/button'
import { errorMessage } from '~/i18n'
import { ApiError } from '~/lib/api'
import { useUpload } from '~/lib/queries'

// Picks an image and uploads it; the server checks the type by content and names the file
// postId: into that post, in its owner's drive (any of its editors may)
export function UploadButton({ label, postId, onUploaded }: { label: string; postId?: number; onUploaded: (url: string) => void }) {
  const { t, i18n } = useTranslation()
  const input = useRef<HTMLInputElement>(null)
  const upload = useUpload(postId)

  return (
    <>
      <input
        ref={input}
        type="file"
        accept="image/jpeg,image/png,image/webp,image/gif"
        className="hidden"
        aria-label={label}
        onChange={(event) => {
          const file = event.target.files?.[0]
          event.target.value = ''
          if (!file) return
          upload.mutate(file, {
            onSuccess: ({ url }) => onUploaded(url),
            onError: (error) => {
              const field = error instanceof ApiError ? error.fields.file : undefined
              toast.error(field ? t(`fieldErrors.${field as 'invalid'}`) : errorMessage(i18n, error))
            },
          })
        }}
      />
      <Button type="button" variant="outline" size="xs" disabled={upload.isPending} onClick={() => input.current?.click()}>
        <ImageUpIcon />
        {upload.isPending ? t('admin.uploading') : label}
      </Button>
    </>
  )
}
