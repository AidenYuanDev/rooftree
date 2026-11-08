import { PencilIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { DeleteButton } from '~/components/admin/delete-button'
import { FormError } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Label } from '~/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '~/components/ui/select'
import { Skeleton } from '~/components/ui/skeleton'
import { Textarea } from '~/components/ui/textarea'
import { errorMessage, fieldErrorMessage } from '~/i18n'
import { ApiError, api } from '~/lib/api'
import { useHydrated } from '~/lib/hydrated'
import { useLang } from '~/lib/lang'
import { type ThinkingInput, useAdminThinking, useDeleteThinking, useSaveThinking } from '~/lib/queries'
import { AdminThinking, type Lang, langs } from '~/lib/schemas'

const maxLength = 2000

// The text in both languages; the original one required, the other one optional (empty: none)
function Composer({
  id,
  initial,
  onDone,
}: {
  id: number | null
  initial?: { originalLang: Lang; texts: Partial<Record<Lang, string>> }
  onDone?: () => void
}) {
  const { t, i18n } = useTranslation()
  const hydrated = useHydrated()
  const lang = useLang()
  const save = useSaveThinking()
  const [originalLang, setOriginalLang] = useState<Lang>(initial?.originalLang ?? lang)
  const [texts, setTexts] = useState<Partial<Record<Lang, string>>>(initial?.texts ?? {})
  const other: Lang = originalLang === 'zh' ? 'en' : 'zh'
  const fieldError = save.error instanceof ApiError ? Object.entries(save.error.fields)[0] : undefined

  const submit = (event: React.FormEvent) => {
    event.preventDefault()
    const translations: ThinkingInput['translations'] = {}
    for (const l of langs) {
      const text = texts[l]?.trim() ?? ''
      if (text) translations[l] = { body: text }
      // Editing: an emptied translation is removed
      else if (id !== null && initial?.texts[l]) translations[l] = null
    }
    save.mutate(
      { id, input: { status: 'published', originalLang, translations } },
      {
        onSuccess: () => {
          toast.success(t('admin.saved'))
          if (id === null) setTexts({})
          onDone?.()
        },
      },
    )
  }

  return (
    <form method="post" onSubmit={submit} className="grid gap-3 rounded-lg border p-4" noValidate>
      <div className="flex items-center gap-2">
        <Label className="mr-auto">{id === null ? t('admin.newThinking') : t('admin.edit')}</Label>
        <Select value={originalLang} onValueChange={(value) => setOriginalLang(value as Lang)}>
          <SelectTrigger className="w-32" aria-label={t('admin.originalLang')}>
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
      </div>
      <Textarea
        rows={3}
        maxLength={maxLength}
        lang={originalLang}
        placeholder={t('admin.thinkingPlaceholder')}
        aria-label={t(`langs.${originalLang}`)}
        value={texts[originalLang] ?? ''}
        onChange={(event) => setTexts({ ...texts, [originalLang]: event.target.value })}
      />
      <Textarea
        rows={2}
        maxLength={maxLength}
        lang={other}
        placeholder={t('admin.thinkingTranslation', { lang: t(`langs.${other}`) })}
        aria-label={t(`langs.${other}`)}
        value={texts[other] ?? ''}
        onChange={(event) => setTexts({ ...texts, [other]: event.target.value })}
      />
      {fieldError && <p className="text-xs text-destructive">{fieldErrorMessage(i18n, fieldError[1])}</p>}
      {save.error && !fieldError && <p className="text-xs text-destructive">{errorMessage(i18n, save.error)}</p>}
      <div className="flex justify-end gap-2">
        {onDone && id !== null && (
          <Button type="button" variant="ghost" onClick={onDone}>
            {t('admin.cancel')}
          </Button>
        )}
        <Button type="submit" disabled={!hydrated || save.isPending || !texts[originalLang]?.trim()}>
          {id === null ? t('admin.publish') : t('admin.save')}
        </Button>
      </div>
    </form>
  )
}

function Row({ item }: { item: { id: number; status: string; body: string; translations: Lang[]; publishedAt: string | null } }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const remove = useDeleteThinking()
  const [editing, setEditing] = useState<{ originalLang: Lang; texts: Partial<Record<Lang, string>> } | null>(null)

  if (editing) return <Composer id={item.id} initial={editing} onDone={() => setEditing(null)} />

  return (
    <div className="flex items-start gap-3 p-3">
      <div className="min-w-0 flex-1">
        <p className="text-sm whitespace-pre-wrap">{item.body}</p>
        <div className="mt-1 flex gap-3 text-xs text-muted-foreground">
          <LocalDate iso={item.publishedAt} lang={lang} />
          <span>{item.translations.map((l) => t(`langs.${l}`)).join(' · ')}</span>
          {item.status === 'draft' && <Badge variant="outline">{t('admin.statusDraft')}</Badge>}
        </div>
      </div>
      <Button
        variant="ghost"
        size="icon-sm"
        aria-label={t('admin.edit')}
        onClick={async () => {
          try {
            const full = await api(`/api/admin/thinking/${item.id}`, AdminThinking)
            setEditing({
              originalLang: full.originalLang,
              texts: Object.fromEntries(Object.entries(full.translations).map(([l, text]) => [l, text?.body ?? ''])),
            })
          } catch (error) {
            toast.error(errorMessage(i18n, error))
          }
        }}
      >
        <PencilIcon />
      </Button>
      <DeleteButton
        title={item.body.slice(0, 30)}
        onConfirm={() => remove.mutate(item.id, { onError: (error) => toast.error(errorMessage(i18n, error)) })}
      />
    </div>
  )
}

export default function AdminThinkingPage() {
  const { t } = useTranslation()
  const [page, setPage] = useState(1)
  const list = useAdminThinking(page)

  return (
    <div className="grid gap-6">
      <Composer id={null} />
      <FormError error={list.error} />
      {list.isPending && <Skeleton className="h-40 w-full" />}
      {list.data?.thinking.length === 0 && <p className="py-12 text-center text-muted-foreground">{t('thinking.empty')}</p>}
      {list.data && list.data.thinking.length > 0 && (
        <div className="divide-y rounded-lg border">
          {list.data.thinking.map((item) => (
            <Row key={item.id} item={item} />
          ))}
        </div>
      )}
      {list.data && (page > 1 || list.data.hasMore) && (
        <div className="flex justify-center gap-2">
          <Button variant="outline" size="sm" disabled={page <= 1} onClick={() => setPage(page - 1)}>
            ‹
          </Button>
          <span className="px-2 py-1 text-sm">{page}</span>
          <Button variant="outline" size="sm" disabled={!list.data.hasMore} onClick={() => setPage(page + 1)}>
            ›
          </Button>
        </div>
      )}
    </div>
  )
}
