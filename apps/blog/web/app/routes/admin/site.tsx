import { zodResolver } from '@hookform/resolvers/zod'
import { PlusIcon, XIcon } from 'lucide-react'
import { useFieldArray, useForm, useWatch } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { z } from 'zod'
import { UploadButton } from '~/components/admin/upload-button'
import { FormError, FormField } from '~/components/form'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '~/components/ui/select'
import { Skeleton } from '~/components/ui/skeleton'
import { Switch } from '~/components/ui/switch'
import { Textarea } from '~/components/ui/textarea'
import { applyFieldErrors } from '~/lib/form-errors'
import { useHydrated } from '~/lib/hydrated'
import { useAdminSite, useSaveSite } from '~/lib/queries'
import { type AdminSite, langs, SocialKind, socialKinds } from '~/lib/schemas'

// The profile: the home page's introduction, the about pages, the links. Texts in both languages; a
// reader of a language without one gets the other

const localized = (max: number) => z.object({ zh: z.string().trim().max(max, 'too_long'), en: z.string().trim().max(max, 'too_long') })

const siteForm = z.object({
  name: localized(60),
  author: z.string().trim().max(40, 'too_long'),
  avatar: z
    .string()
    .trim()
    .max(255, 'too_long')
    .refine((v) => v === '' || v.startsWith('/uploads/') || v.startsWith('https://'), 'invalid'),
  tagline: localized(120),
  bio: localized(500),
  quote: localized(200),
  since: z.string().refine((v) => v === '' || /^\d{4}-\d{2}-\d{2}$/.test(v), 'invalid'),
  reviewWriters: z.boolean(),
  socials: z.array(
    z.object({
      kind: SocialKind,
      url: z
        .string()
        .trim()
        .min(1, 'required')
        .max(255, 'too_long')
        .refine((v) => /^(https?:\/\/|mailto:|\/(?!\/))/.test(v), 'invalid'),
    }),
  ),
  about: localized(50000),
  aboutSite: localized(50000),
})
type SiteForm = z.infer<typeof siteForm>

const empty = { zh: '', en: '' }

function fromSite(site: Partial<AdminSite>): SiteForm {
  return {
    name: site.name ?? empty,
    author: site.author ?? '',
    avatar: site.avatar ?? '',
    tagline: site.tagline ?? empty,
    bio: site.bio ?? empty,
    quote: site.quote ?? empty,
    since: site.since ?? '',
    socials: site.socials ?? [],
    about: site.about ?? empty,
    aboutSite: site.aboutSite ?? empty,
    reviewWriters: site.reviewWriters ?? false,
  }
}

type LocalizedKey = 'name' | 'tagline' | 'bio' | 'quote' | 'about' | 'aboutSite'

function Localized({
  form,
  name,
  label,
  rows,
}: {
  form: ReturnType<typeof useForm<SiteForm>>
  name: LocalizedKey
  label: string
  rows?: number
}) {
  const { t } = useTranslation()
  const errors = form.formState.errors[name]
  return (
    <div className="grid gap-3 md:grid-cols-2">
      {langs.map((lang) => (
        <FormField key={lang} label={`${label} · ${t(`langs.${lang}`)}`} error={errors?.[lang]?.message}>
          {(id) =>
            rows ? (
              <Textarea
                id={id}
                rows={rows}
                lang={lang}
                className={rows > 4 ? 'font-mono text-sm' : ''}
                {...form.register(`${name}.${lang}`)}
              />
            ) : (
              <Input id={id} lang={lang} {...form.register(`${name}.${lang}`)} />
            )
          }
        </FormField>
      ))}
    </div>
  )
}

function Editor({ site }: { site: Partial<AdminSite> }) {
  const { t } = useTranslation()
  const hydrated = useHydrated()
  const save = useSaveSite()
  const form = useForm<SiteForm>({ resolver: zodResolver(siteForm), defaultValues: fromSite(site) })
  const socials = useFieldArray({ control: form.control, name: 'socials' })
  const kinds = useWatch({ control: form.control, name: 'socials' })
  const reviewWriters = useWatch({ control: form.control, name: 'reviewWriters' })
  const errors = form.formState.errors

  const onSubmit = form.handleSubmit((values) =>
    save.mutate(values, {
      onSuccess: (saved) => {
        toast.success(t('admin.siteSaved'))
        form.reset(fromSite(saved))
      },
      onError: (error) => applyFieldErrors(form, error),
    }),
  )

  return (
    <form method="post" onSubmit={onSubmit} className="grid gap-8" noValidate>
      <div className="flex items-center">
        <h1 className="mr-auto text-2xl font-semibold tracking-tight">{t('admin.site')}</h1>
        <Button type="submit" disabled={!hydrated || save.isPending}>
          {save.isPending ? t('admin.saving') : t('admin.save')}
        </Button>
      </div>
      <FormError error={save.error} />

      <section className="flex items-start gap-3 rounded-lg border p-4">
        <Switch
          id="review-writers"
          checked={reviewWriters}
          onCheckedChange={(checked) => form.setValue('reviewWriters', checked, { shouldDirty: true })}
        />
        <div className="grid gap-1">
          <Label htmlFor="review-writers">{t('admin.reviewWriters')}</Label>
          <p className="text-sm text-muted-foreground">{t('admin.reviewWritersHint')}</p>
        </div>
      </section>

      <section className="grid gap-4 rounded-lg border p-4">
        <Localized form={form} name="name" label={t('admin.siteName')} />
        <div className="grid gap-3 md:grid-cols-3">
          <FormField label={t('admin.author')} error={errors.author?.message}>
            {(id) => <Input id={id} {...form.register('author')} />}
          </FormField>
          <FormField label={t('admin.avatar')} error={errors.avatar?.message}>
            {(id) => (
              <div className="flex gap-2">
                <Input id={id} {...form.register('avatar')} />
                <UploadButton
                  label={t('admin.uploadImage')}
                  onUploaded={(url) => form.setValue('avatar', url, { shouldDirty: true, shouldValidate: true })}
                />
              </div>
            )}
          </FormField>
          <FormField label={t('admin.since')} hint={t('admin.sinceHint')} error={errors.since?.message}>
            {(id) => <Input id={id} type="date" {...form.register('since')} />}
          </FormField>
        </div>
      </section>

      <section className="grid gap-4 rounded-lg border p-4">
        <Localized form={form} name="tagline" label={t('admin.tagline')} />
        <p className="-mt-2 text-xs text-muted-foreground">{t('admin.taglineHint')}</p>
        <Localized form={form} name="bio" label={t('admin.bio')} rows={3} />
        <Localized form={form} name="quote" label={t('admin.quote')} />
      </section>

      <section className="grid gap-3 rounded-lg border p-4">
        <Label>{t('admin.socials')}</Label>
        {socials.fields.map((field, index) => (
          <div key={field.id} className="flex gap-2">
            <Select
              value={kinds[index]?.kind}
              onValueChange={(value) =>
                form.setValue(`socials.${index}.kind`, value as SiteForm['socials'][number]['kind'], { shouldDirty: true })
              }
            >
              <SelectTrigger className="w-40" aria-label={t('admin.socialKind')}>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {socialKinds.map((kind) => (
                  <SelectItem key={kind} value={kind}>
                    {t(`social.${kind}`)}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
            <Input
              aria-label={t('admin.socialUrl')}
              aria-invalid={!!errors.socials?.[index]?.url}
              placeholder="https:// · mailto: · /zh/feed.xml"
              {...form.register(`socials.${index}.url`)}
            />
            <Button type="button" variant="ghost" size="icon" aria-label={t('admin.delete')} onClick={() => socials.remove(index)}>
              <XIcon />
            </Button>
          </div>
        ))}
        <Button
          type="button"
          variant="outline"
          size="sm"
          className="justify-self-start"
          onClick={() => socials.append({ kind: 'github', url: '' })}
        >
          <PlusIcon />
          {t('admin.addSocial')}
        </Button>
      </section>

      <section className="grid gap-4 rounded-lg border p-4">
        <Localized form={form} name="about" label={t('footer.aboutMe')} rows={12} />
        <Localized form={form} name="aboutSite" label={t('footer.aboutSite')} rows={8} />
        <p className="text-xs text-muted-foreground">{t('admin.markdownHint')}</p>
      </section>
    </form>
  )
}

export default function AdminSitePage() {
  const site = useAdminSite()
  if (site.isPending) return <Skeleton className="h-96 w-full" />
  if (site.error || !site.data) return <FormError error={site.error} />
  return <Editor site={site.data} />
}
