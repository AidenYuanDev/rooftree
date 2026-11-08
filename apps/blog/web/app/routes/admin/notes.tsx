import { PencilIcon, PlusIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link, useSearchParams } from 'react-router'
import { toast } from 'sonner'
import { DeleteButton } from '~/components/admin/delete-button'
import { FormError } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Skeleton } from '~/components/ui/skeleton'
import { Tabs, TabsList, TabsTrigger } from '~/components/ui/tabs'
import { errorMessage } from '~/i18n'
import { localePath, useLang } from '~/lib/lang'
import { useAdminNotes, useDeleteNote } from '~/lib/queries'

export default function AdminNotes() {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const [searchParams, setSearchParams] = useSearchParams()
  const status = searchParams.get('status') ?? ''
  const page = Math.max(1, Number(searchParams.get('page') ?? 1) || 1)
  const notes = useAdminNotes(status, page)
  const remove = useDeleteNote()

  return (
    <div className="grid gap-4">
      <div className="flex flex-wrap items-center gap-3">
        <Tabs value={status || 'all'} onValueChange={(value) => setSearchParams(value === 'all' ? {} : { status: value })}>
          <TabsList>
            <TabsTrigger value="all">{t('admin.all')}</TabsTrigger>
            <TabsTrigger value="published">{t('admin.statusPublished')}</TabsTrigger>
            <TabsTrigger value="draft">{t('admin.statusDraft')}</TabsTrigger>
          </TabsList>
        </Tabs>
        <Button asChild className="ml-auto">
          <Link to={localePath(lang, '/admin/notes/new')}>
            <PlusIcon />
            {t('admin.newNote')}
          </Link>
        </Button>
      </div>

      <FormError error={notes.error} />
      {notes.isPending && <Skeleton className="h-40 w-full" />}
      {notes.data?.notes.length === 0 && <p className="py-12 text-center text-muted-foreground">{t('notes.empty')}</p>}
      {notes.data && notes.data.notes.length > 0 && (
        <ul className="divide-y rounded-lg border">
          {notes.data.notes.map((note) => (
            <li key={note.id} className="flex flex-wrap items-center gap-3 p-3">
              <div className="min-w-0 flex-1">
                <div className="flex items-center gap-2">
                  <span className="font-mono text-xs text-muted-foreground">#{note.id}</span>
                  <Link to={localePath(lang, `/admin/notes/${note.id}`)} className="truncate font-medium hover:underline">
                    {note.title}
                  </Link>
                  <Badge variant={note.status === 'published' ? 'default' : 'outline'}>
                    {note.status === 'published' ? t('admin.statusPublished') : t('admin.statusDraft')}
                  </Badge>
                </div>
                <div className="mt-0.5 flex gap-3 text-xs text-muted-foreground">
                  <span>{note.translations.map((l) => t(`langs.${l}`)).join(' · ')}</span>
                  <span>
                    {t('admin.updatedAt')} <LocalDate iso={note.updatedAt} lang={lang} />
                  </span>
                </div>
              </div>
              {note.status === 'published' && (
                <Button variant="ghost" size="sm" asChild>
                  <Link to={localePath(lang, `/notes/${note.id}`)}>{t('admin.view')}</Link>
                </Button>
              )}
              <Button variant="ghost" size="icon-sm" asChild aria-label={t('admin.edit')}>
                <Link to={localePath(lang, `/admin/notes/${note.id}`)}>
                  <PencilIcon />
                </Link>
              </Button>
              <DeleteButton
                title={note.title}
                onConfirm={() => remove.mutate(note.id, { onError: (error) => toast.error(errorMessage(i18n, error)) })}
              />
            </li>
          ))}
        </ul>
      )}

      {notes.data && (page > 1 || notes.data.hasMore) && (
        <div className="flex justify-center gap-2">
          <Button
            variant="outline"
            size="sm"
            disabled={page <= 1}
            onClick={() => setSearchParams({ ...(status ? { status } : {}), page: String(page - 1) })}
          >
            ‹
          </Button>
          <span className="px-2 py-1 text-sm">{page}</span>
          <Button
            variant="outline"
            size="sm"
            disabled={!notes.data.hasMore}
            onClick={() => setSearchParams({ ...(status ? { status } : {}), page: String(page + 1) })}
          >
            ›
          </Button>
        </div>
      )}
    </div>
  )
}
