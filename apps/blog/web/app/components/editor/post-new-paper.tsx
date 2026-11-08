import { zodResolver } from '@hookform/resolvers/zod'
import { ArrowLeftIcon, CodeIcon, EyeIcon, SendIcon } from 'lucide-react'
import { useRef, useState } from 'react'
import { useForm, useWatch } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { useNavigate } from 'react-router'
import { toast } from 'sonner'
import { z } from 'zod'
import { UploadButton } from '~/components/admin/upload-button'
import { MarkdownField } from '~/components/editor/markdown-field'
import { FormError, FormField } from '~/components/form'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { insertAtCursor } from '~/lib/collab'
import { applyFieldErrors } from '~/lib/form-errors'
import { slug } from '~/lib/forms'
import { useHydrated } from '~/lib/hydrated'
import { localePath } from '~/lib/lang'
import { useMe, useSavePost, useTags } from '~/lib/queries'
import type { Lang } from '~/lib/schemas'

const newPostForm = z.object({
  slug: slug(100),
  title: z.string().trim().min(1, 'required').max(200, 'too_long'),
  summary: z.string().trim().max(500, 'too_long'),
  body: z.string().trim().min(1, 'required').max(200000, 'too_long'),
  tags: z.array(z.string()),
})

type NewPostForm = z.infer<typeof newPostForm>

// Writing a new post directly on the /posts page: a single-column paper card with line-by-line
// live Markdown preview. Saving as a draft switches into collaborative editing (with editor
// invitations); publishing goes straight to /posts/<slug>
export function PostNewPaper({
  lang,
  onCancel,
  onDraftCreated,
}: {
  lang: Lang
  onCancel: () => void
  onDraftCreated: (id: number) => void
}) {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const navigate = useNavigate()
  const admin = useMe().data?.user?.role === 'admin'
  const save = useSavePost()
  const tags = useTags()
  const [sourceMode, setSourceMode] = useState(false)
  const bodyHost = useRef<HTMLDivElement>(null)

  const form = useForm<NewPostForm>({
    resolver: zodResolver(newPostForm),
    defaultValues: { slug: '', title: '', summary: '', body: '', tags: [] },
  })
  const selectedTags = useWatch({ control: form.control, name: 'tags' }) ?? []
  const body = useWatch({ control: form.control, name: 'body' }) ?? ''
  const errors = form.formState.errors

  const submit = (status: 'draft' | 'published') =>
    void form.handleSubmit((values) => {
      save.mutate(
        {
          id: null,
          input: {
            slug: values.slug,
            status,
            originalLang: lang,
            cover: '',
            commentsEnabled: true,
            tags: values.tags,
            publishAt: null,
            translations: { [lang]: { title: values.title, summary: values.summary, body: values.body } },
          },
        },
        {
          onSuccess: (saved) => {
            if (status === 'published' && saved.status === 'published') {
              toast.success(t('collab.published'))
              void navigate(localePath(lang, `/posts/${saved.slug}`))
              return
            }
            toast.success(saved.reviewRequestedAt ? t('collab.reviewAsked') : t('collab.draftSaved'))
            onDraftCreated(saved.id)
          },
          onError: (error) => {
            // Server validation errors for translations.<lang>.title/body map back to title/body here
            applyFieldErrors(form, error)
            const fields = (error as { fields?: Record<string, string> })?.fields
            if (fields?.[`translations.${lang}.title`]) form.setError('title', { message: fields[`translations.${lang}.title`] })
            if (fields?.[`translations.${lang}.body`]) form.setError('body', { message: fields[`translations.${lang}.body`] })
          },
        },
      )
    })()

  return (
    <div className="grid gap-4">
      <div className="flex flex-wrap items-center gap-2 border-b pb-4">
        <Button type="button" variant="ghost" size="sm" onClick={onCancel}>
          <ArrowLeftIcon />
          {t('collab.backToPosts')}
        </Button>

        <div className="ml-auto flex flex-wrap items-center gap-1.5">
          {admin && (
            <UploadButton
              label={t('admin.uploadImage')}
              onUploaded={(url) => {
                if (insertAtCursor(bodyHost.current, `![](${url})`)) toast.success(t('admin.uploaded'))
              }}
            />
          )}
          <Button
            type="button"
            variant="ghost"
            size="sm"
            aria-pressed={sourceMode}
            title="Ctrl+/"
            onClick={() => setSourceMode((current) => !current)}
          >
            {sourceMode ? <EyeIcon /> : <CodeIcon />}
            {sourceMode ? t('collab.livePreview') : t('collab.source')}
          </Button>
          <Button type="button" variant="outline" size="sm" disabled={!hydrated || save.isPending} onClick={() => submit('draft')}>
            {t('collab.saveDraft')}
          </Button>
          <Button type="button" size="sm" disabled={!hydrated || save.isPending} onClick={() => submit('published')}>
            <SendIcon />
            {t('collab.publish')}
          </Button>
        </div>
      </div>

      <FormError error={save.error} />

      <div className="grid gap-3 rounded-xl border bg-background/40 p-3">
        <FormField label={t('admin.slug')} hint={t('admin.slugHint')} error={errors.slug?.message}>
          {(id, describedBy) => (
            <Input id={id} className="font-mono" placeholder="hello-world" aria-describedby={describedBy} {...form.register('slug')} />
          )}
        </FormField>
        {tags.data && tags.data.length > 0 && (
          <div className="flex flex-wrap items-center gap-1.5">
            {tags.data.map((tag) => {
              const selected = selectedTags.includes(tag.slug)
              return (
                <Button
                  key={tag.slug}
                  type="button"
                  size="xs"
                  variant={selected ? 'default' : 'outline'}
                  aria-pressed={selected}
                  onClick={() =>
                    form.setValue('tags', selected ? selectedTags.filter((s) => s !== tag.slug) : [...selectedTags, tag.slug], {
                      shouldDirty: true,
                    })
                  }
                >
                  {lang === 'en' ? tag.nameEn : tag.nameZh}
                </Button>
              )
            })}
          </div>
        )}
      </div>

      <div className="grid gap-1.5">
        <Input
          lang={lang}
          aria-label={t('admin.title_')}
          placeholder={t('admin.title_')}
          aria-invalid={!!errors.title}
          className="h-auto py-2 font-heading text-2xl font-bold md:text-3xl"
          {...form.register('title')}
        />
        {errors.title?.message && <p className="text-xs text-destructive">{t(`fieldErrors.${errors.title.message as 'required'}`)}</p>}
      </div>

      <div className="grid gap-1.5">
        <Input
          lang={lang}
          aria-label={t('admin.summary')}
          placeholder={t('collab.summaryOptional')}
          aria-invalid={!!errors.summary}
          className="text-muted-foreground"
          {...form.register('summary')}
        />
        {errors.summary?.message && <p className="text-xs text-destructive">{t(`fieldErrors.${errors.summary.message as 'too_long'}`)}</p>}
      </div>

      <div className="grid gap-1.5">
        <MarkdownField
          hostRef={bodyHost}
          lang={lang}
          label={t('admin.body')}
          placeholder={t('admin.body')}
          value={body}
          onChange={(value) => form.setValue('body', value, { shouldDirty: true, shouldValidate: !!errors.body })}
          sourceMode={sourceMode}
          onToggleSource={() => setSourceMode((current) => !current)}
          invalid={!!errors.body}
        />
        {errors.body?.message && <p className="text-xs text-destructive">{t(`fieldErrors.${errors.body.message as 'required'}`)}</p>}
      </div>
    </div>
  )
}
