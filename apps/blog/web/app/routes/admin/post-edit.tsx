import { zodResolver } from '@hookform/resolvers/zod'
import { CodeIcon, EyeIcon, PlusIcon, XIcon } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { useForm, useWatch } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { Link, useNavigate, useParams } from 'react-router'
import { toast } from 'sonner'
import { z } from 'zod'
import { FormError, FormField } from '~/components/form'
import { UploadButton } from '~/components/admin/upload-button'
import { MarkdownField } from '~/components/editor/markdown-field'
import { PostWorkspace } from '~/components/editor/post-workspace'
import { api } from '~/lib/api'
import { insertAtCursor } from '~/lib/collab'
import { applyFieldErrors } from '~/lib/form-errors'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '~/components/ui/select'
import { Skeleton } from '~/components/ui/skeleton'
import { Switch } from '~/components/ui/switch'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '~/components/ui/tabs'
import { Textarea } from '~/components/ui/textarea'
import { slug } from '~/lib/forms'
import { useHydrated } from '~/lib/hydrated'
import { localePath, useLang, usePostsBase } from '~/lib/lang'
import { type PostInput, useAdminPost, useMe, useSavePost, useTags } from '~/lib/queries'
import { type AdminPost, AdminCommentList, Lang, langs } from '~/lib/schemas'
import { type PostDraft, useDraftStore } from '~/stores/drafts'

const translationForm = z.object({
  title: z.string().trim().min(1, 'required').max(200, 'too_long'),
  summary: z.string().trim().max(500, 'too_long'),
  body: z.string().trim().min(1, 'required').max(200000, 'too_long'),
})

const postForm = z
  .object({
    slug: slug(100),
    status: z.enum(['draft', 'published']),
    originalLang: Lang,
    cover: z
      .string()
      .trim()
      .max(255, 'too_long')
      .refine((v) => v === '' || v.startsWith('/uploads/') || v.startsWith('https://'), 'invalid'),
    commentsEnabled: z.boolean(),
    tags: z.array(z.string()),
    translations: z.partialRecord(Lang, translationForm),
    // datetime-local, the author's own time; '': not scheduled
    publishAt: z.string().optional(),
  })
  .refine((post) => post.translations[post.originalLang], { path: ['originalLang'], message: 'required' })
  .refine((post) => post.status !== 'draft' || !post.publishAt || Date.parse(post.publishAt) > Date.now(), {
    path: ['publishAt'],
    message: 'past',
  })

type PostForm = z.infer<typeof postForm>

const emptyTranslation = { title: '', summary: '', body: '' }

// An ISO time as a datetime-local value, in the reader's zone
function localInput(iso: string | null) {
  if (!iso) return ''
  const date = new Date(iso)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`
}

function fromPost(post: AdminPost): PostForm {
  return {
    slug: post.slug,
    status: post.status,
    originalLang: post.originalLang,
    cover: post.cover,
    commentsEnabled: post.commentsEnabled,
    tags: post.tags,
    translations: post.translations,
    publishAt: localInput(post.publishAt),
  }
}

function newPost(lang: Lang): PostForm {
  return {
    slug: '',
    status: 'draft',
    originalLang: lang,
    cover: '',
    commentsEnabled: true,
    tags: [],
    translations: { [lang]: emptyTranslation },
    publishAt: '',
  }
}

function TranslationEditor({ form, lang }: { form: ReturnType<typeof useForm<PostForm>>; lang: Lang }) {
  const { t } = useTranslation()
  const admin = useMe().data?.user?.role === 'admin'
  const [sourceMode, setSourceMode] = useState(false)
  const bodyHost = useRef<HTMLDivElement>(null)
  const body = useWatch({ control: form.control, name: `translations.${lang}.body` }) ?? ''
  const errors = form.formState.errors.translations?.[lang]

  return (
    <div className="grid gap-4">
      <FormField label={t('admin.title_')} error={errors?.title?.message}>
        {(id, describedBy) => <Input id={id} lang={lang} aria-describedby={describedBy} {...form.register(`translations.${lang}.title`)} />}
      </FormField>
      <FormField label={t('admin.summary')} error={errors?.summary?.message}>
        {(id, describedBy) => (
          <Textarea id={id} lang={lang} rows={2} aria-describedby={describedBy} {...form.register(`translations.${lang}.summary`)} />
        )}
      </FormField>
      <div className="grid gap-1.5">
        <div className="flex items-center gap-2">
          <Label htmlFor={`body-${lang}`} className="mr-auto">
            {t('admin.body')}
          </Label>
          {/* A writer's images go in its post once there is one (components/editor) */}
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
            size="xs"
            aria-pressed={sourceMode}
            title="Ctrl+/"
            onClick={() => setSourceMode((current) => !current)}
          >
            {sourceMode ? <EyeIcon /> : <CodeIcon />}
            {sourceMode ? t('collab.livePreview') : t('collab.source')}
          </Button>
        </div>
        <MarkdownField
          id={`body-${lang}`}
          hostRef={bodyHost}
          lang={lang}
          label={t('admin.body')}
          value={body}
          onChange={(value) => form.setValue(`translations.${lang}.body`, value, { shouldDirty: true, shouldValidate: !!errors?.body })}
          sourceMode={sourceMode}
          onToggleSource={() => setSourceMode((current) => !current)}
          invalid={!!errors?.body}
        />
        {errors?.body?.message && <p className="text-xs text-destructive">{t(`fieldErrors.${errors.body.message as 'required'}`)}</p>}
      </div>
    </div>
  )
}

function Editor({ id, post }: { id: number | null; post?: AdminPost }) {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const save = useSavePost()
  const tags = useTags()
  const base = usePostsBase()
  const admin = useMe().data?.user?.role === 'admin'
  const draftKey = id === null ? 'new' : String(id)
  const { drafts, saveDraft, discardDraft } = useDraftStore()

  const initial = post ? fromPost(post) : newPost(lang)
  const stored = drafts[draftKey]
  // A draft from after the post's last save wins: it holds the newer text
  const restore = stored && (!post || stored.savedAt > Date.parse(post.updatedAt)) ? (stored.draft as PostForm) : null

  const form = useForm<PostForm>({ resolver: zodResolver(postForm), defaultValues: restore ?? initial })
  const [activeLang, setActiveLang] = useState<Lang>((restore ?? initial).originalLang)
  const values = useWatch({ control: form.control })
  const translations = values.translations
  const restoredNotice = useRef(false)

  useEffect(() => {
    if (restore && !restoredNotice.current) {
      restoredNotice.current = true
      toast.info(t('admin.draftRestored'), {
        action: {
          label: t('admin.discardDraft'),
          onClick: () => {
            discardDraft(draftKey)
            form.reset(initial)
          },
        },
      })
    }
  })

  // Every change is kept as a draft, a second after typing stops
  const isDirty = form.formState.isDirty
  useEffect(() => {
    if (!isDirty) return
    const timer = window.setTimeout(() => saveDraft(draftKey, values as PostDraft), 1000)
    return () => window.clearTimeout(timer)
  }, [values, isDirty, draftKey, saveDraft])

  const presentLangs = langs.filter((l) => translations?.[l])
  const missingLangs = langs.filter((l) => !translations?.[l])

  const onSubmit = form.handleSubmit((values) => {
    // Only a draft waits for its time
    const publishAt = values.status === 'draft' && values.publishAt ? new Date(values.publishAt).toISOString() : null
    const input: PostInput = { ...values, translations: { ...values.translations }, publishAt }
    // Languages the post had and the form doesn't anymore are removed
    for (const l of langs) if (post?.translations[l] && !values.translations[l]) input.translations[l] = null

    save.mutate(
      { id, input },
      {
        onSuccess: (saved) => {
          discardDraft(draftKey)
          toast.success(t('admin.saved'))
          form.reset(fromPost(saved))
          // Comments on passages whose text the post no longer has: the admin's to look at
          if (saved.status === 'published')
            void api('/api/admin/comments?passage=orphaned', AdminCommentList).then((list) => {
              const count = list.comments.filter((comment) => comment.target === `post:${saved.id}`).length
              if (count)
                toast.warning(t('admin.orphanedAfterSave', { count }), {
                  duration: 15_000,
                  action: {
                    label: t('admin.reviewOrphaned'),
                    onClick: () => void navigate(`${localePath(lang, '/admin/comments')}?passage=orphaned`),
                  },
                })
            })
          if (id === null) void navigate(localePath(lang, `${base}/${saved.id}`), { replace: true })
        },
        onError: (error) => applyFieldErrors(form, error),
      },
    )
  })

  const errors = form.formState.errors
  return (
    <form method="post" onSubmit={onSubmit} className="grid gap-6" noValidate>
      <div className="flex flex-wrap items-center gap-3">
        <h1 className="mr-auto text-2xl font-semibold tracking-tight">
          {id === null ? t('admin.newPost') : (post?.translations[post.originalLang]?.title ?? post?.slug)}
        </h1>
        {post?.status === 'published' && (
          <Button type="button" variant="ghost" asChild>
            <Link to={localePath(lang, `/posts/${post.slug}`)}>{t('admin.view')}</Link>
          </Button>
        )}
        <Button type="submit" disabled={!hydrated || save.isPending}>
          {save.isPending ? t('admin.saving') : t('admin.save')}
        </Button>
      </div>

      <FormError error={save.error} />

      <div className="grid gap-4 rounded-lg border p-4 md:grid-cols-2">
        <FormField label={t('admin.slug')} hint={t('admin.slugHint')} error={errors.slug?.message}>
          {(fieldId, describedBy) => <Input id={fieldId} className="font-mono" aria-describedby={describedBy} {...form.register('slug')} />}
        </FormField>
        <FormField label={t('admin.cover')} hint={t('admin.coverHint')} error={errors.cover?.message}>
          {(fieldId, describedBy) => (
            <div className="flex gap-2">
              <Input id={fieldId} aria-describedby={describedBy} {...form.register('cover')} />
              {admin && (
                <UploadButton
                  label={t('admin.uploadCover')}
                  onUploaded={(url) => {
                    form.setValue('cover', url, { shouldDirty: true, shouldValidate: true })
                    toast.success(t('admin.coverUploaded'))
                  }}
                />
              )}
            </div>
          )}
        </FormField>
        <FormField label={t('admin.status')}>
          {(fieldId) => (
            <Select
              value={values.status}
              onValueChange={(value) => form.setValue('status', value as PostForm['status'], { shouldDirty: true })}
            >
              <SelectTrigger id={fieldId} className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="draft">{t('admin.statusDraft')}</SelectItem>
                <SelectItem value="published">{t('admin.statusPublished')}</SelectItem>
              </SelectContent>
            </Select>
          )}
        </FormField>
        {values.status === 'draft' && (
          <FormField label={t('admin.publishAt')} hint={t('admin.publishAtHint')} error={errors.publishAt?.message}>
            {(fieldId, describedBy) => (
              <Input id={fieldId} type="datetime-local" aria-describedby={describedBy} {...form.register('publishAt')} />
            )}
          </FormField>
        )}
        <FormField label={t('admin.originalLang')} error={errors.originalLang?.message}>
          {(fieldId) => (
            <Select
              value={values.originalLang}
              onValueChange={(value) => form.setValue('originalLang', value as Lang, { shouldDirty: true, shouldValidate: true })}
            >
              <SelectTrigger id={fieldId} className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {langs.map((l) => (
                  <SelectItem key={l} value={l}>
                    {t(`langs.${l}`)}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          )}
        </FormField>
        <div className="grid gap-2 md:col-span-2">
          <Label>{t('admin.tags')}</Label>
          <div className="flex flex-wrap gap-2">
            {tags.data?.map((tag) => {
              const selected = (values.tags ?? []).includes(tag.slug)
              return (
                <Button
                  key={tag.slug}
                  type="button"
                  size="xs"
                  variant={selected ? 'default' : 'outline'}
                  aria-pressed={selected}
                  onClick={() => {
                    const current = form.getValues('tags')
                    form.setValue('tags', selected ? current.filter((s) => s !== tag.slug) : [...current, tag.slug], { shouldDirty: true })
                  }}
                >
                  {lang === 'en' ? tag.nameEn : tag.nameZh}
                </Button>
              )
            })}
            {tags.data?.length === 0 && (
              <Link to={localePath(lang, '/admin/tags')} className="text-sm text-muted-foreground underline">
                {t('admin.addTag')}
              </Link>
            )}
          </div>
          {errors.tags?.message && <p className="text-xs text-destructive">{t(`fieldErrors.${errors.tags.message as 'unknown_tag'}`)}</p>}
        </div>
        <div className="flex items-center gap-2 md:col-span-2">
          <Switch
            id="comments-enabled"
            checked={values.commentsEnabled}
            onCheckedChange={(checked) => form.setValue('commentsEnabled', checked, { shouldDirty: true })}
          />
          <Label htmlFor="comments-enabled">{t('admin.commentsEnabled')}</Label>
        </div>
      </div>

      <Tabs value={activeLang} onValueChange={(value) => setActiveLang(value as Lang)}>
        <div className="flex flex-wrap items-center gap-2">
          <TabsList>
            {presentLangs.map((l) => (
              <TabsTrigger key={l} value={l}>
                {t(`langs.${l}`)}
              </TabsTrigger>
            ))}
          </TabsList>
          {missingLangs.map((l) => (
            <Button
              key={l}
              type="button"
              variant="outline"
              size="sm"
              onClick={() => {
                form.setValue(`translations.${l}`, emptyTranslation, { shouldDirty: true })
                setActiveLang(l)
              }}
            >
              <PlusIcon />
              {t('admin.addTranslation', { lang: t(`langs.${l}`) })}
            </Button>
          ))}
          {presentLangs.length > 1 && activeLang !== values.originalLang && (
            <Button
              type="button"
              variant="ghost"
              size="sm"
              className="ml-auto"
              onClick={() => {
                const next = { ...form.getValues('translations') }
                delete next[activeLang]
                form.setValue('translations', next, { shouldDirty: true })
                setActiveLang(form.getValues('originalLang'))
              }}
            >
              <XIcon />
              {t('admin.removeTranslation', { lang: t(`langs.${activeLang}`) })}
            </Button>
          )}
        </div>
        {presentLangs.map((l) => (
          <TabsContent key={l} value={l} className="mt-4">
            <TranslationEditor form={form} lang={l} />
          </TabsContent>
        ))}
      </Tabs>
    </form>
  )
}

export default function PostEdit() {
  const { id } = useParams()
  const postId = id === undefined ? null : Number(id)
  const post = useAdminPost(postId)

  if (postId === null) return <Editor id={null} />
  if (post.isPending) return <Skeleton className="h-96 w-full" />
  if (post.error || !post.data) return <FormError error={post.error} />

  // Its text written together, live; keyed by the post, so switching posts starts afresh
  return <PostWorkspace key={post.data.id} post={post.data} />
}
