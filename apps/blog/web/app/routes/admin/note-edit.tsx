import { zodResolver } from '@hookform/resolvers/zod'
import { CodeIcon, EyeIcon, PlusIcon, XIcon } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { useForm, useWatch } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { Link, useNavigate, useParams } from 'react-router'
import { toast } from 'sonner'
import { z } from 'zod'
import { UploadButton } from '~/components/admin/upload-button'
import { MarkdownField } from '~/components/editor/markdown-field'
import { FormError, FormField } from '~/components/form'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '~/components/ui/select'
import { Skeleton } from '~/components/ui/skeleton'
import { Switch } from '~/components/ui/switch'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '~/components/ui/tabs'
import { insertAtCursor } from '~/lib/collab'
import { applyFieldErrors } from '~/lib/form-errors'
import { useHydrated } from '~/lib/hydrated'
import { localePath, useLang } from '~/lib/lang'
import { type NoteInput, useAdminNote, useSaveNote } from '~/lib/queries'
import { type AdminNote, Lang, langs } from '~/lib/schemas'
import { useDraftStore } from '~/stores/drafts'

// Like the post editor (post-edit.tsx), for a note: no slug, tags or summary; a mood and the weather

const translationForm = z.object({
  title: z.string().trim().min(1, 'required').max(200, 'too_long'),
  body: z.string().trim().min(1, 'required').max(200000, 'too_long'),
})

const noteForm = z
  .object({
    status: z.enum(['draft', 'published']),
    originalLang: Lang,
    mood: z.string().trim().max(20, 'too_long'),
    weather: z.string().trim().max(20, 'too_long'),
    cover: z
      .string()
      .trim()
      .max(255, 'too_long')
      .refine((v) => v === '' || v.startsWith('/uploads/') || v.startsWith('https://'), 'invalid'),
    commentsEnabled: z.boolean(),
    translations: z.partialRecord(Lang, translationForm),
  })
  .refine((note) => note.translations[note.originalLang], { path: ['originalLang'], message: 'required' })

type NoteForm = z.infer<typeof noteForm>

const emptyTranslation = { title: '', body: '' }

function fromNote(note: AdminNote): NoteForm {
  return {
    status: note.status,
    originalLang: note.originalLang,
    mood: note.mood,
    weather: note.weather,
    cover: note.cover,
    commentsEnabled: note.commentsEnabled,
    translations: note.translations,
  }
}

function newNote(lang: Lang): NoteForm {
  return {
    status: 'draft',
    originalLang: lang,
    mood: '',
    weather: '',
    cover: '',
    commentsEnabled: true,
    translations: { [lang]: emptyTranslation },
  }
}

function TranslationEditor({ form, lang }: { form: ReturnType<typeof useForm<NoteForm>>; lang: Lang }) {
  const { t } = useTranslation()
  const [sourceMode, setSourceMode] = useState(false)
  const bodyHost = useRef<HTMLDivElement>(null)
  const body = useWatch({ control: form.control, name: `translations.${lang}.body` }) ?? ''
  const errors = form.formState.errors.translations?.[lang]

  return (
    <div className="grid gap-4">
      <FormField label={t('admin.title_')} error={errors?.title?.message}>
        {(id, describedBy) => <Input id={id} lang={lang} aria-describedby={describedBy} {...form.register(`translations.${lang}.title`)} />}
      </FormField>
      <div className="grid gap-1.5">
        <div className="flex items-center gap-2">
          <Label htmlFor={`note-body-${lang}`} className="mr-auto">
            {t('admin.body')}
          </Label>
          <UploadButton
            label={t('admin.uploadImage')}
            onUploaded={(url) => {
              if (insertAtCursor(bodyHost.current, `![](${url})`)) toast.success(t('admin.uploaded'))
            }}
          />
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
          id={`note-body-${lang}`}
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

function Editor({ id, note }: { id: number | null; note?: AdminNote }) {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const save = useSaveNote()
  const draftKey = `note:${id ?? 'new'}`
  const { drafts, saveDraft, discardDraft } = useDraftStore()

  const initial = note ? fromNote(note) : newNote(lang)
  const stored = drafts[draftKey]
  const restore = stored && (!note || stored.savedAt > Date.parse(note.updatedAt)) ? (stored.draft as NoteForm) : null

  const form = useForm<NoteForm>({ resolver: zodResolver(noteForm), defaultValues: restore ?? initial })
  const [activeLang, setActiveLang] = useState<Lang>((restore ?? initial).originalLang)
  // Not part of the note: asked at each save, until it was mailed
  const [notify, setNotify] = useState(false)
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

  const isDirty = form.formState.isDirty
  useEffect(() => {
    if (!isDirty) return
    const timer = window.setTimeout(() => saveDraft(draftKey, values), 1000)
    return () => window.clearTimeout(timer)
  }, [values, isDirty, draftKey, saveDraft])

  const presentLangs = langs.filter((l) => translations?.[l])
  const missingLangs = langs.filter((l) => !translations?.[l])

  const onSubmit = form.handleSubmit((values) => {
    const input: NoteInput = { ...values, notify, translations: { ...values.translations } }
    for (const l of langs) if (note?.translations[l] && !values.translations[l]) input.translations[l] = null

    save.mutate(
      { id, input },
      {
        onSuccess: (saved) => {
          discardDraft(draftKey)
          toast.success(t('admin.saved'))
          form.reset(fromNote(saved))
          if (id === null) void navigate(localePath(lang, `/admin/notes/${saved.id}`), { replace: true })
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
          {id === null ? t('admin.newNote') : `#${id} ${note?.translations[note.originalLang]?.title ?? ''}`}
        </h1>
        {note?.status === 'published' && (
          <Button type="button" variant="ghost" asChild>
            <Link to={localePath(lang, `/notes/${note.id}`)}>{t('admin.view')}</Link>
          </Button>
        )}
        <Button type="submit" disabled={!hydrated || save.isPending}>
          {save.isPending ? t('admin.saving') : t('admin.save')}
        </Button>
      </div>

      <FormError error={save.error} />

      <div className="grid gap-4 rounded-lg border p-4 md:grid-cols-2">
        <FormField label={t('admin.mood')} hint={t('admin.moodHint')} error={errors.mood?.message}>
          {(fieldId, describedBy) => <Input id={fieldId} aria-describedby={describedBy} {...form.register('mood')} />}
        </FormField>
        <FormField label={t('admin.weather')} hint={t('admin.weatherHint')} error={errors.weather?.message}>
          {(fieldId, describedBy) => <Input id={fieldId} aria-describedby={describedBy} {...form.register('weather')} />}
        </FormField>
        <FormField label={t('admin.status')}>
          {(fieldId) => (
            <Select
              value={values.status}
              onValueChange={(value) => form.setValue('status', value as NoteForm['status'], { shouldDirty: true })}
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
        <FormField label={t('admin.cover')} hint={t('admin.coverHint')} error={errors.cover?.message}>
          {(fieldId, describedBy) => (
            <div className="flex gap-2">
              <Input id={fieldId} aria-describedby={describedBy} {...form.register('cover')} />
              <UploadButton
                label={t('admin.uploadCover')}
                onUploaded={(url) => {
                  form.setValue('cover', url, { shouldDirty: true, shouldValidate: true })
                  toast.success(t('admin.coverUploaded'))
                }}
              />
            </div>
          )}
        </FormField>
        <div className="flex items-center gap-2 md:pt-6">
          <Switch
            id="note-comments-enabled"
            checked={values.commentsEnabled}
            onCheckedChange={(checked) => form.setValue('commentsEnabled', checked, { shouldDirty: true })}
          />
          <Label htmlFor="note-comments-enabled">{t('admin.commentsEnabled')}</Label>
        </div>
        <div className="flex items-center gap-2 md:col-span-2">
          <Switch id="note-notify" checked={notify} onCheckedChange={setNotify} />
          <Label htmlFor="note-notify">{t('admin.notifySubscribers')}</Label>
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

export default function NoteEdit() {
  const { id } = useParams()
  const noteId = id === undefined ? null : Number(id)
  const note = useAdminNote(noteId)

  if (noteId === null) return <Editor id={null} />
  if (note.isPending) return <Skeleton className="h-96 w-full" />
  if (note.error || !note.data) return <FormError error={note.error} />

  return <Editor key={note.data.id} id={noteId} note={note.data} />
}
