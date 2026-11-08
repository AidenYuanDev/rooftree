import { zodResolver } from '@hookform/resolvers/zod'
import { PencilIcon, PlusIcon } from 'lucide-react'
import { useState } from 'react'
import { useForm } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import type { z } from 'zod'
import { FormField } from '~/components/form'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { Input } from '~/components/ui/input'
import { errorMessage } from '~/i18n'
import { applyFieldErrors } from '~/lib/form-errors'
import { tagForm } from '~/lib/forms'
import { useLang } from '~/lib/lang'
import { useDeleteTag, useMe, useSaveTag, useTags } from '~/lib/queries'
import type { Tag } from '~/lib/schemas'

type TagForm = z.infer<typeof tagForm>

// What was searched for and not found, as the new tag's fields: only an ASCII search can be its slug
function fromSearch(search: string): TagForm {
  const text = search.trim()
  const ascii = /^[\x20-\x7e]*$/.test(text)
  const slug = /^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(text.toLowerCase()) ? text.toLowerCase() : ''
  return { slug, nameZh: text, nameEn: ascii ? text : '' }
}

function TagDialog({
  tag,
  search,
  onClose,
  onCreated,
  onDeleted,
}: {
  tag: Tag | null
  search: string
  onClose: () => void
  onCreated: (slug: string) => void
  onDeleted: (slug: string) => void
}) {
  const { t, i18n } = useTranslation()
  const save = useSaveTag()
  const remove = useDeleteTag()
  const [confirming, setConfirming] = useState(false)
  const form = useForm<TagForm>({
    resolver: zodResolver(tagForm),
    defaultValues: tag ? { slug: tag.slug, nameZh: tag.nameZh, nameEn: tag.nameEn } : fromSearch(search),
  })
  const errors = form.formState.errors

  return (
    <Dialog open onOpenChange={(open) => !open && onClose()}>
      <DialogContent>
        {/* Events bubble through the portal: without stopPropagation this would submit the post's form too */}
        <form
          method="post"
          className="grid gap-4"
          noValidate
          onSubmit={(event) => {
            event.stopPropagation()
            void form.handleSubmit((values) =>
              save.mutate(
                { creating: !tag, tag: values },
                {
                  onSuccess: () => {
                    if (!tag) onCreated(values.slug)
                    onClose()
                  },
                  onError: (e) => applyFieldErrors(form, e) || toast.error(errorMessage(i18n, e)),
                },
              ),
            )(event)
          }}
        >
          <DialogHeader>
            <DialogTitle>{t(tag ? 'admin.editTag' : 'admin.newTag')}</DialogTitle>
          </DialogHeader>
          <FormField label={t('admin.tagSlug')} error={errors.slug?.message}>
            {(id, describedBy) => (
              <Input id={id} className="font-mono" readOnly={!!tag} aria-describedby={describedBy} {...form.register('slug')} />
            )}
          </FormField>
          <FormField label={t('admin.tagNameZh')} error={errors.nameZh?.message}>
            {(id, describedBy) => <Input id={id} aria-describedby={describedBy} {...form.register('nameZh')} />}
          </FormField>
          <FormField label={t('admin.tagNameEn')} error={errors.nameEn?.message}>
            {(id, describedBy) => <Input id={id} aria-describedby={describedBy} {...form.register('nameEn')} />}
          </FormField>
          {confirming && tag && (
            <p role="alert" className="text-sm text-destructive">
              {t('admin.tagDeleteWarning', { posts: tag.postCount })}
            </p>
          )}
          <DialogFooter>
            {tag && (
              <Button
                type="button"
                variant="destructive"
                className="sm:mr-auto"
                disabled={remove.isPending}
                onClick={() => {
                  if (!confirming) return setConfirming(true)
                  remove.mutate(tag.slug, {
                    onSuccess: () => {
                      onDeleted(tag.slug)
                      onClose()
                    },
                    onError: (e) => toast.error(errorMessage(i18n, e)),
                  })
                }}
              >
                {t(confirming ? 'admin.tagDeleteConfirm' : 'admin.delete')}
              </Button>
            )}
            <Button type="button" variant="outline" onClick={onClose}>
              {t('admin.cancel')}
            </Button>
            <Button type="submit" disabled={save.isPending}>
              {t(tag ? 'admin.save' : 'admin.addTag')}
            </Button>
          </DialogFooter>
        </form>
      </DialogContent>
    </Dialog>
  )
}

// The tags of a post being written: anyone who may edit it finds and picks them; creating, renaming
// and deleting them is the admin's (/api/admin/tags), here as on the tags page
export function TagPicker({ value, onChange, disabled }: { value: string[]; onChange: (slugs: string[]) => void; disabled?: boolean }) {
  const { t } = useTranslation()
  const lang = useLang()
  const tags = useTags()
  const canManage = useMe().data?.user?.role === 'admin' && !disabled
  const [search, setSearch] = useState('')
  const [managing, setManaging] = useState(false)
  const [dialog, setDialog] = useState<Tag | 'new' | null>(null)

  const all = tags.data ?? []
  if (all.length === 0 && !canManage) return null

  const needle = search.trim().toLowerCase()
  const shown = all.filter((tag) => [tag.slug, tag.nameZh, tag.nameEn].some((text) => text.toLowerCase().includes(needle)))
  const toggle = (slug: string) => onChange(value.includes(slug) ? value.filter((s) => s !== slug) : [...value, slug])

  return (
    <div className="grid gap-2">
      <div className="flex items-center gap-2">
        <Input
          aria-label={t('admin.tagSearch')}
          placeholder={t('admin.tagSearch')}
          className="h-7 max-w-56 text-sm"
          value={search}
          disabled={disabled}
          onChange={(event) => setSearch(event.target.value)}
          onKeyDown={(event) => {
            if (event.key !== 'Enter') return
            // The post's form must not submit from here
            event.preventDefault()
            const [only, ...others] = shown
            if (only && others.length === 0 && !managing) {
              toggle(only.slug)
              setSearch('')
            }
          }}
        />
        {canManage && (
          <>
            <Button type="button" size="xs" variant="outline" onClick={() => setDialog('new')}>
              <PlusIcon />
              {t('admin.newTag')}
            </Button>
            <Button
              type="button"
              size="xs"
              variant={managing ? 'default' : 'ghost'}
              aria-pressed={managing}
              onClick={() => setManaging(!managing)}
            >
              <PencilIcon />
              {t('admin.manageTags')}
            </Button>
          </>
        )}
      </div>
      <div className="flex flex-wrap items-center gap-1.5">
        {shown.map((tag) => {
          const selected = value.includes(tag.slug)
          return (
            <Button
              key={tag.slug}
              type="button"
              size="xs"
              variant={selected ? 'default' : 'outline'}
              aria-pressed={managing ? undefined : selected}
              disabled={disabled}
              onClick={() => (managing ? setDialog(tag) : toggle(tag.slug))}
            >
              {lang === 'en' ? tag.nameEn : tag.nameZh}
            </Button>
          )
        })}
        {shown.length === 0 && needle && <span className="text-sm text-muted-foreground">{t('admin.noTagFound')}</span>}
      </div>
      {dialog && (
        <TagDialog
          tag={dialog === 'new' ? null : dialog}
          search={search}
          onClose={() => setDialog(null)}
          onCreated={(slug) => {
            onChange([...value, slug])
            setSearch('')
          }}
          onDeleted={(slug) => onChange(value.filter((s) => s !== slug))}
        />
      )}
    </div>
  )
}
