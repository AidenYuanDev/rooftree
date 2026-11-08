import { diffLines, diffWordsWithSpace } from 'diff'
import { HistoryIcon, RotateCcwIcon, SendIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { LocalDate } from '~/components/local-date'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle, DialogTrigger } from '~/components/ui/dialog'
import { Skeleton } from '~/components/ui/skeleton'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage } from '~/i18n'
import { useLang } from '~/lib/lang'
import { usePublishPost, usePostRevision, usePostRevisions, useRestorePostRevision } from '~/lib/queries'
import type { AdminPost, PostRevision, PostRevisionItem } from '~/lib/schemas'
import { cn } from '~/lib/utils'

// Every version of a post, kept: each publication and each one an editor saved. One opened shows
// what it changed from the one before it in its language, or its whole text; it goes back into the
// working copy (its editors go on from it), and to readers too for who may publish
export function PostRevisionsDialog({ post }: { post: AdminPost }) {
  const { t } = useTranslation()
  const [open, setOpen] = useState(false)
  const revisions = usePostRevisions(open ? post.id : null)
  const [shown, setShown] = useState<number | null>(null)
  const list = revisions.data?.pages.flatMap((page) => page.revisions) ?? []

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>
        <Button type="button" variant="ghost">
          <HistoryIcon />
          {t('admin.revisions')}
        </Button>
      </DialogTrigger>
      <DialogContent className="max-h-[85vh] overflow-y-auto sm:max-w-3xl">
        <DialogHeader>
          <DialogTitle>{t('admin.revisions')}</DialogTitle>
          <DialogDescription>{t('admin.revisionsHint')}</DialogDescription>
        </DialogHeader>
        {revisions.isPending && <Skeleton className="h-32 w-full" />}
        {revisions.isSuccess && list.length === 0 && <p className="text-muted-foreground">{t('admin.revisionsEmpty')}</p>}
        <ul className="grid gap-2">
          {list.map((revision) => (
            <li key={revision.id} className="rounded-lg border">
              <RevisionLine
                revision={revision}
                expanded={shown === revision.id}
                onToggle={() => setShown(shown === revision.id ? null : revision.id)}
              />
              {shown === revision.id && <RevisionDetail post={post} item={revision} onDone={() => setOpen(false)} />}
            </li>
          ))}
        </ul>
        {revisions.hasNextPage && (
          <Button
            variant="outline"
            className="justify-self-center"
            disabled={revisions.isFetchingNextPage}
            onClick={() => void revisions.fetchNextPage()}
          >
            {t('posts.loadMore')}
          </Button>
        )}
      </DialogContent>
    </Dialog>
  )
}

function RevisionLine({ revision, expanded, onToggle }: { revision: PostRevisionItem; expanded: boolean; onToggle: () => void }) {
  const { t } = useTranslation()
  const lang = useLang()
  return (
    <button
      type="button"
      aria-expanded={expanded}
      className="flex w-full flex-wrap items-center gap-2 p-3 text-left text-sm"
      onClick={onToggle}
    >
      <Badge variant={revision.kind === 'published' ? 'default' : 'secondary'}>
        {revision.kind === 'published' ? t('history.published') : t('history.saved')}
      </Badge>
      <Badge variant="outline">{t(`langs.${revision.lang}`)}</Badge>
      <span className="font-medium">{revision.title}</span>
      {revision.note && <span className="text-muted-foreground">· {revision.note}</span>}
      <span className="ml-auto flex items-center gap-2 text-xs text-muted-foreground">
        {revision.author ? (
          <>
            <UserAvatar user={revision.author} className="size-5 text-[0.6rem]" />
            {revision.author.displayName}
          </>
        ) : (
          t('history.scheduled')
        )}
        <LocalDate iso={revision.createdAt} lang={lang} />
      </span>
    </button>
  )
}

function RevisionDetail({ post, item, onDone }: { post: AdminPost; item: PostRevisionItem; onDone: () => void }) {
  const { t, i18n } = useTranslation()
  const revision = usePostRevision(post.id, item.id)
  const restore = useRestorePostRevision(post.id)
  const publish = usePublishPost(post.id)
  const [whole, setWhole] = useState(false)
  const onError = (error: unknown) => toast.error(errorMessage(i18n, error))

  if (!revision.data) return <Skeleton className="m-3 h-24" />
  const busy = restore.isPending || publish.isPending
  return (
    <div className="grid gap-3 border-t p-3">
      <div className="flex flex-wrap items-center gap-2">
        <div className="flex rounded-md border p-0.5 text-xs">
          {[false, true].map((value) => (
            <button
              key={String(value)}
              type="button"
              aria-pressed={whole === value}
              className={cn('rounded px-2 py-1', whole === value && 'bg-muted font-medium')}
              onClick={() => setWhole(value)}
            >
              {value ? t('history.whole') : t('history.changes')}
            </button>
          ))}
        </div>
        <span className="mr-auto" />
        <Button
          size="sm"
          variant="outline"
          disabled={busy}
          onClick={() =>
            restore.mutate(item.id, {
              onSuccess: () => {
                toast.success(t('history.restored'))
                onDone()
              },
              onError,
            })
          }
        >
          <RotateCcwIcon />
          {t('history.restore')}
        </Button>
        {post.rights.includes('publish') && (
          <Button
            size="sm"
            disabled={busy}
            onClick={() =>
              restore.mutate(item.id, {
                onSuccess: () =>
                  publish.mutate(undefined, {
                    onSuccess: () => {
                      toast.success(t('history.restoredPublished'))
                      onDone()
                    },
                    onError,
                  }),
                onError,
              })
            }
          >
            <SendIcon />
            {t('history.restorePublish')}
          </Button>
        )}
      </div>
      {whole ? <Whole revision={revision.data} /> : <Changes revision={revision.data} />}
    </div>
  )
}

function Whole({ revision }: { revision: PostRevision }) {
  return (
    <div className="grid gap-2 text-sm">
      <p className="font-medium">{revision.title}</p>
      {revision.summary && <p className="text-muted-foreground">{revision.summary}</p>}
      <pre className="max-h-96 overflow-auto rounded-md bg-muted/40 p-3 text-xs break-words whitespace-pre-wrap">{revision.body}</pre>
    </div>
  )
}

// Lines unchanged around a change, the rest folded
const context = 3

function Changes({ revision }: { revision: PostRevision }) {
  const { t } = useTranslation()
  const previous = revision.previous
  if (!previous) return <p className="text-sm text-muted-foreground">{t('history.first')}</p>

  const parts = diffLines(previous.body, revision.body)
  const same =
    previous.title === revision.title && previous.summary === revision.summary && parts.every((part) => !part.added && !part.removed)
  if (same) return <p className="text-sm text-muted-foreground">{t('history.noChanges')}</p>

  return (
    <div className="grid gap-2 text-sm">
      {previous.title !== revision.title && <Inline label={t('admin.title_')} before={previous.title} after={revision.title} />}
      {previous.summary !== revision.summary && <Inline label={t('admin.summary')} before={previous.summary} after={revision.summary} />}
      <pre className="max-h-96 overflow-auto rounded-md border font-mono text-xs leading-5">
        {parts.map((part, index) => {
          const lines = part.value.replace(/\n$/, '').split('\n')
          if (part.added || part.removed)
            return lines.map((line, at) => (
              <div
                key={`${index}-${at}`}
                className={cn(
                  'px-3 break-words whitespace-pre-wrap',
                  part.added ? 'bg-emerald-500/15' : 'bg-rose-500/15 line-through decoration-rose-500/40',
                )}
              >
                <span className="mr-2 text-muted-foreground select-none">{part.added ? '+' : '−'}</span>
                {line}
              </div>
            ))
          // Unchanged: only around the changes
          const first = index === 0
          const last = index === parts.length - 1
          const kept =
            lines.length <= context * 2 + 1
              ? lines
              : [...(first ? [] : lines.slice(0, context)), null, ...(last ? [] : lines.slice(-context))]
          return kept.map((line, at) =>
            line === null ? (
              <div key={`${index}-${at}`} className="px-3 text-muted-foreground select-none">
                ⋯
              </div>
            ) : (
              <div key={`${index}-${at}`} className="px-3 break-words whitespace-pre-wrap text-muted-foreground">
                <span className="mr-2 select-none"> </span>
                {line}
              </div>
            ),
          )
        })}
      </pre>
    </div>
  )
}

// A short field's change, word by word
function Inline({ label, before, after }: { label: string; before: string; after: string }) {
  return (
    <p>
      <span className="mr-2 text-xs text-muted-foreground">{label}</span>
      {diffWordsWithSpace(before, after).map((part, index) => (
        <span key={index} className={cn(part.added && 'bg-emerald-500/15', part.removed && 'bg-rose-500/15 line-through')}>
          {part.value}
        </span>
      ))}
    </p>
  )
}
