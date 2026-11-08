import { zodResolver } from '@hookform/resolvers/zod'
import { Trash2Icon } from 'lucide-react'
import { useForm } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { z } from 'zod'
import { FormError, FormField } from '~/components/form'
import { useHydrated } from '~/lib/hydrated'
import { applyFieldErrors } from '~/lib/form-errors'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { Skeleton } from '~/components/ui/skeleton'
import { errorMessage } from '~/i18n'
import { slug } from '~/lib/forms'
import { useDeleteTag, useSaveTag, useTags } from '~/lib/queries'
import type { Tag } from '~/lib/schemas'

const tagForm = z.object({
  slug: slug(50),
  nameZh: z.string().trim().min(1, 'required').max(50, 'too_long'),
  nameEn: z.string().trim().min(1, 'required').max(50, 'too_long'),
})

function TagRow({ tag }: { tag: Tag }) {
  const hydrated = useHydrated()
  const { t, i18n } = useTranslation()
  const save = useSaveTag()
  const remove = useDeleteTag()
  const form = useForm<z.infer<typeof tagForm>>({
    resolver: zodResolver(tagForm),
    values: { slug: tag.slug, nameZh: tag.nameZh, nameEn: tag.nameEn },
  })

  return (
    <form
      method="post"
      className="grid grid-cols-[8rem_1fr_1fr_3rem_auto_auto] items-center gap-2 p-2"
      onSubmit={form.handleSubmit((values) =>
        save.mutate(
          { creating: false, tag: values },
          {
            onSuccess: () => toast.success(t('admin.saved')),
            onError: (e) => applyFieldErrors(form, e) || toast.error(errorMessage(i18n, e)),
          },
        ),
      )}
    >
      <code className="truncate text-sm">{tag.slug}</code>
      <Input aria-label={t('admin.tagNameZh')} aria-invalid={!!form.formState.errors.nameZh} {...form.register('nameZh')} />
      <Input aria-label={t('admin.tagNameEn')} aria-invalid={!!form.formState.errors.nameEn} {...form.register('nameEn')} />
      <span className="text-center text-sm text-muted-foreground">{tag.postCount}</span>
      <Button type="submit" size="sm" variant="outline" disabled={!hydrated || !form.formState.isDirty || save.isPending}>
        {t('admin.save')}
      </Button>
      <Button
        type="button"
        size="icon-sm"
        variant="ghost"
        aria-label={t('admin.delete')}
        onClick={() => remove.mutate(tag.slug, { onError: (e) => toast.error(errorMessage(i18n, e)) })}
      >
        <Trash2Icon />
      </Button>
    </form>
  )
}

export default function AdminTags() {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const tags = useTags()
  const create = useSaveTag()
  const form = useForm<z.infer<typeof tagForm>>({ resolver: zodResolver(tagForm), defaultValues: { slug: '', nameZh: '', nameEn: '' } })
  const errors = form.formState.errors

  return (
    <div className="grid gap-6">
      <form
        method="post"
        className="grid gap-4 rounded-lg border p-4 md:grid-cols-[1fr_1fr_1fr_auto] md:items-start"
        noValidate
        onSubmit={form.handleSubmit((values) =>
          create.mutate({ creating: true, tag: values }, { onSuccess: () => form.reset(), onError: (e) => applyFieldErrors(form, e) }),
        )}
      >
        <FormField label={t('admin.tagSlug')} error={errors.slug?.message}>
          {(id, describedBy) => <Input id={id} className="font-mono" aria-describedby={describedBy} {...form.register('slug')} />}
        </FormField>
        <FormField label={t('admin.tagNameZh')} error={errors.nameZh?.message}>
          {(id, describedBy) => <Input id={id} aria-describedby={describedBy} {...form.register('nameZh')} />}
        </FormField>
        <FormField label={t('admin.tagNameEn')} error={errors.nameEn?.message}>
          {(id, describedBy) => <Input id={id} aria-describedby={describedBy} {...form.register('nameEn')} />}
        </FormField>
        <Button type="submit" className="md:mt-6" disabled={!hydrated || create.isPending}>
          {t('admin.addTag')}
        </Button>
      </form>

      <FormError error={tags.error} />
      {tags.isPending && <Skeleton className="h-32 w-full" />}
      {tags.data && tags.data.length > 0 && (
        <div className="divide-y rounded-lg border">
          <div className="grid grid-cols-[8rem_1fr_1fr_3rem_auto_auto] gap-2 p-2 text-xs font-medium text-muted-foreground">
            <span>{t('admin.tagSlug')}</span>
            <span>{t('admin.tagNameZh')}</span>
            <span>{t('admin.tagNameEn')}</span>
            <span className="text-center">{t('admin.tagPosts')}</span>
          </div>
          {tags.data.map((tag) => (
            <TagRow key={tag.slug} tag={tag} />
          ))}
        </div>
      )}
    </div>
  )
}
