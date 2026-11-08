import { zodResolver } from '@hookform/resolvers/zod'
import { PencilIcon } from 'lucide-react'
import { useState } from 'react'
import { useForm } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { z } from 'zod'
import { DeleteButton } from '~/components/admin/delete-button'
import { UploadButton } from '~/components/admin/upload-button'
import { FormError, FormField } from '~/components/form'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { Skeleton } from '~/components/ui/skeleton'
import { Textarea } from '~/components/ui/textarea'
import { errorMessage } from '~/i18n'
import { applyFieldErrors } from '~/lib/form-errors'
import { useHydrated } from '~/lib/hydrated'
import { useDeleteProject, useProjects, useSaveProject } from '~/lib/queries'
import type { Project } from '~/lib/schemas'

const siteUrl = z
  .string()
  .trim()
  .max(255, 'too_long')
  .refine((v) => v === '' || /^https?:\/\/[^\s@]{4,}$/.test(v), 'invalid')

const projectForm = z.object({
  name: z.string().trim().min(1, 'required').max(80, 'too_long'),
  url: siteUrl,
  repoUrl: siteUrl,
  icon: z
    .string()
    .trim()
    .max(255, 'too_long')
    .refine((v) => v === '' || v.startsWith('https://') || v.startsWith('/uploads/'), 'invalid'),
  descriptionZh: z.string().trim().max(300, 'too_long'),
  descriptionEn: z.string().trim().max(300, 'too_long'),
  sort: z.number('invalid').int('invalid'),
})
type ProjectForm = z.infer<typeof projectForm>

function ProjectFields({ project, onDone }: { project?: Project; onDone?: () => void }) {
  const { t } = useTranslation()
  const hydrated = useHydrated()
  const save = useSaveProject()
  const form = useForm<ProjectForm>({
    resolver: zodResolver(projectForm),
    defaultValues: project ?? { name: '', url: '', repoUrl: '', icon: '', descriptionZh: '', descriptionEn: '', sort: 0 },
  })
  const errors = form.formState.errors
  const onSubmit = form.handleSubmit((input) =>
    save.mutate(
      { id: project?.id ?? null, input },
      {
        onSuccess: () => {
          toast.success(t('admin.saved'))
          if (!project) form.reset()
          onDone?.()
        },
        onError: (error) => applyFieldErrors(form, error),
      },
    ),
  )

  return (
    <form method="post" onSubmit={onSubmit} className="grid gap-3 rounded-lg border p-4 md:grid-cols-2" noValidate>
      <FormField label={t('projects.name')} error={errors.name?.message}>
        {(id) => <Input id={id} {...form.register('name')} />}
      </FormField>
      <FormField label={t('projects.icon')} error={errors.icon?.message}>
        {(id) => (
          <div className="flex gap-2">
            <Input id={id} {...form.register('icon')} />
            <UploadButton
              label={t('admin.uploadImage')}
              onUploaded={(url) => form.setValue('icon', url, { shouldDirty: true, shouldValidate: true })}
            />
          </div>
        )}
      </FormField>
      <FormField label={t('projects.url')} error={errors.url?.message}>
        {(id) => <Input id={id} type="url" {...form.register('url')} />}
      </FormField>
      <FormField label={t('projects.repoUrl')} error={errors.repoUrl?.message}>
        {(id) => <Input id={id} type="url" {...form.register('repoUrl')} />}
      </FormField>
      <FormField label={t('projects.descriptionZh')} error={errors.descriptionZh?.message}>
        {(id) => <Textarea id={id} rows={2} lang="zh" {...form.register('descriptionZh')} />}
      </FormField>
      <FormField label={t('projects.descriptionEn')} error={errors.descriptionEn?.message}>
        {(id) => <Textarea id={id} rows={2} lang="en" {...form.register('descriptionEn')} />}
      </FormField>
      <FormField label={t('admin.sort')} error={errors.sort?.message}>
        {(id) => <Input id={id} type="number" className="w-28" {...form.register('sort', { valueAsNumber: true })} />}
      </FormField>
      <div className="flex items-end justify-end gap-2">
        {onDone && (
          <Button type="button" variant="ghost" onClick={onDone}>
            {t('admin.cancel')}
          </Button>
        )}
        <Button type="submit" disabled={!hydrated || save.isPending}>
          {project ? t('admin.save') : t('admin.add')}
        </Button>
      </div>
      <div className="md:col-span-2">
        <FormError error={save.error} />
      </div>
    </form>
  )
}

function Row({ project }: { project: Project }) {
  const { t, i18n } = useTranslation()
  const remove = useDeleteProject()
  const [editing, setEditing] = useState(false)
  if (editing) return <ProjectFields project={project} onDone={() => setEditing(false)} />
  return (
    <div className="flex items-center gap-3 p-3">
      {project.icon ? (
        <img src={project.icon} alt="" className="size-9 rounded-md object-cover" />
      ) : (
        <span className="size-9 rounded-md bg-muted" />
      )}
      <div className="min-w-0 flex-1">
        <p className="font-medium">{project.name}</p>
        <p className="truncate text-xs text-muted-foreground">{project.descriptionZh || project.descriptionEn}</p>
      </div>
      <span className="text-xs text-muted-foreground">
        {t('admin.sort')} {project.sort}
      </span>
      <Button variant="ghost" size="icon-sm" aria-label={t('admin.edit')} onClick={() => setEditing(true)}>
        <PencilIcon />
      </Button>
      <DeleteButton
        title={project.name}
        onConfirm={() => remove.mutate(project.id, { onError: (error) => toast.error(errorMessage(i18n, error)) })}
      />
    </div>
  )
}

export default function AdminProjects() {
  const { t } = useTranslation()
  const projects = useProjects()
  return (
    <div className="grid gap-6">
      <ProjectFields />
      <FormError error={projects.error} />
      {projects.isPending && <Skeleton className="h-32 w-full" />}
      {projects.data?.length === 0 && <p className="py-12 text-center text-muted-foreground">{t('projects.empty')}</p>}
      {projects.data && projects.data.length > 0 && (
        <div className="divide-y rounded-lg border">
          {projects.data.map((project) => (
            <Row key={project.id} project={project} />
          ))}
        </div>
      )}
    </div>
  )
}
