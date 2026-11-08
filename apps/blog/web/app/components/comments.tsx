import { useContext, useEffect, useRef, useState } from 'react'
import { ArrowUpRightIcon, EllipsisIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link, useLocation } from 'react-router'
import { toast } from 'sonner'
import { FormError } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '~/components/ui/alert-dialog'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { Skeleton } from '~/components/ui/skeleton'
import { Textarea } from '~/components/ui/textarea'
import { errorMessage } from '~/i18n'
import { FreshComments, useFreshComments } from '~/lib/fresh-comments'
import { useHydrated } from '~/lib/hydrated'
import { localePath, switchLang, useLang } from '~/lib/lang'
import { useComments, useCreateComment, useDeleteComment, useMe, useModerateComment, useUpdateComment } from '~/lib/queries'
import { Reactions, ReactionsProvider } from '~/components/reactions'
import { canModerate, type Comment, type CommentAnchorInput, type CommentTarget, type CommentThread, type User } from '~/lib/schemas'
import { UserAvatar } from '~/components/user-avatar'
import { cn } from '~/lib/utils'
import { PostOwnerContext } from '~/lib/post-owner'
import { useWatch } from '~/stores/live'
import { usePassages } from '~/stores/passages'

const maxLength = 2000
// The server's Blog.Comments.EditWindow default; the server decides either way
const editWindowMs = 5 * 60 * 1000

// anchor: the passage of the post it is on (a new thread's, or a reply's own); header: above it, what
// it is on
export function Composer({
  target,
  replyTo,
  anchor,
  header,
  onDone,
  autoFocus,
}: {
  target: CommentTarget
  replyTo?: { id: number; name: string }
  anchor?: CommentAnchorInput | null
  header?: React.ReactNode
  onDone?: () => void
  autoFocus?: boolean
}) {
  const { t } = useTranslation()
  const hydrated = useHydrated()
  const create = useCreateComment(target)
  const [body, setBody] = useState('')

  return (
    <form
      method="post"
      className="grid gap-2"
      onSubmit={(event) => {
        event.preventDefault()
        create.mutate(
          { body, replyTo: replyTo?.id, anchor: anchor ?? undefined },
          {
            onSuccess: () => {
              setBody('')
              onDone?.()
            },
          },
        )
      }}
    >
      {header}
      <FormError error={create.error} />
      <Textarea
        value={body}
        onChange={(event) => setBody(event.target.value)}
        placeholder={replyTo ? t('comments.replyPlaceholder', { name: replyTo.name }) : t('comments.placeholder')}
        aria-label={replyTo ? t('comments.replyPlaceholder', { name: replyTo.name }) : t('comments.placeholder')}
        maxLength={maxLength}
        rows={replyTo ? 2 : 3}
        autoFocus={autoFocus}
      />
      <div className="flex items-center justify-end gap-2">
        {onDone && (
          <Button type="button" variant="ghost" size="sm" onClick={onDone}>
            {t('comments.cancel')}
          </Button>
        )}
        <Button type="submit" size="sm" disabled={!hydrated || create.isPending || body.trim() === ''}>
          {replyTo ? t('comments.reply') : t('comments.submit')}
        </Button>
      </div>
    </form>
  )
}

// A long comment shows its first lines until asked for the rest
function ClampedText({ text, className }: { text: string; className?: string }) {
  const { t } = useTranslation()
  const element = useRef<HTMLParagraphElement>(null)
  const [long, setLong] = useState(false)
  const [open, setOpen] = useState(false)
  useEffect(() => {
    const p = element.current
    if (p && !open) setLong(p.scrollHeight > p.clientHeight + 1)
  }, [text, open])

  return (
    <>
      <p ref={element} className={cn('mt-1 [overflow-wrap:anywhere] whitespace-pre-wrap', !open && 'line-clamp-6', className)}>
        {text}
      </p>
      {(long || open) && (
        <button
          type="button"
          className="mt-0.5 text-xs text-primary hover:underline"
          onClick={(event) => {
            event.stopPropagation()
            setOpen(!open)
          }}
        >
          {open ? t('comments.showLess') : t('comments.showMore')}
        </button>
      )}
    </>
  )
}

// What a comment quotes of the post: opens its thread beside the post (components/passages.tsx). A
// passage of the other language's page is only there: a link to it, the thread opened on it
export function PassageQuote({ comment, threadId, className }: { comment: Comment; threadId: number; className?: string }) {
  const { t } = useTranslation()
  const lang = useLang()
  const location = useLocation()
  const openThreads = usePassages((state) => state.openThreads)
  if (!comment.anchor) return null
  const lost = comment.anchor.status !== 'ok'
  const elsewhere = comment.anchor.lang !== lang
  const classes = cn(
    'mt-1 flex max-w-full items-baseline gap-1.5 border-l-2 border-primary/40 pl-2 text-left text-xs text-muted-foreground hover:text-foreground',
    className,
  )
  const content = (
    <>
      <span className="shrink-0">{t('passages.quoting')}</span>
      <span className="line-clamp-2 min-w-0 [overflow-wrap:anywhere] italic">{comment.anchor.quote}</span>
      {elsewhere && (
        <Badge variant="outline" className="shrink-0">
          {t(comment.anchor.lang === 'en' ? 'passages.inEn' : 'passages.inZh')}
          <ArrowUpRightIcon />
        </Badge>
      )}
      {lost && (
        <Badge variant="outline" className="shrink-0">
          {t('passages.changed')}
        </Badge>
      )}
    </>
  )
  if (elsewhere)
    return (
      <Link to={`${switchLang(location.pathname, comment.anchor.lang)}?thread=${threadId}&comment=${comment.id}`} className={classes}>
        {content}
      </Link>
    )
  return (
    <button
      type="button"
      className={classes}
      onClick={(event) => {
        event.stopPropagation()
        openThreads([threadId], comment.id)
      }}
    >
      {content}
    </button>
  )
}

export function CommentItem({
  comment,
  threadId,
  user,
  canReply,
  onReply,
  compact = false,
  quote = true,
}: {
  comment: Comment
  threadId: number
  user: User | null | undefined
  canReply: boolean
  onReply: (target: { id: number; name: string }) => void
  // Beside the post: smaller, its thread's passage shown once above it (quote: false)
  compact?: boolean
  quote?: boolean
}) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const update = useUpdateComment(comment.target)
  const remove = useDeleteComment(comment.target)
  const moderate = useModerateComment()
  const [editing, setEditing] = useState(false)
  const [confirmDelete, setConfirmDelete] = useState(false)
  const [draft, setDraft] = useState(comment.body)
  // When the comment was shown: the edit button doesn't need to vanish the moment the window closes
  const [shownAt] = useState(Date.now)
  const startPicking = usePassages((state) => state.startPicking)
  const ownerId = useContext(PostOwnerContext)
  const fresh = useContext(FreshComments).has(comment.id)

  if (comment.status === 'deleted' || !comment.author)
    return <p className="py-2 text-sm text-muted-foreground italic">{t('comments.deleted')}</p>

  const own = user?.id === comment.author.id
  const isAdmin = canModerate(user) || (!!user && user.id === ownerId)
  // A deleted account keeps its comments, not its name
  const author = comment.author.deleted ? { ...comment.author, displayName: t('comments.deletedUser') } : comment.author
  const editable = own && shownAt - Date.parse(comment.createdAt) < editWindowMs
  const onError = (error: unknown) => toast.error(errorMessage(i18n, error))
  // Its passage chosen again: within the edit window, its author's once the post lost it, a moderator's any time
  const canReselect = !!comment.anchor && (editable || (own && comment.anchor.status !== 'ok') || isAdmin)
  const actions = [
    editable && {
      key: 'edit',
      label: t('comments.edit'),
      run: () => {
        setDraft(comment.body)
        setEditing(true)
      },
    },
    canReselect && { key: 'reselect', label: t('passages.reselect'), run: () => startPicking({ kind: 'reselect', commentId: comment.id }) },
    isAdmin && {
      key: 'moderate',
      label: comment.status === 'hidden' ? t('comments.show') : t('comments.hide'),
      run: () => moderate.mutate({ id: comment.id, status: comment.status === 'hidden' ? 'visible' : 'hidden' }, { onError }),
    },
    (own || isAdmin) && { key: 'delete', label: t('comments.delete'), destructive: true, run: () => setConfirmDelete(true) },
  ].filter((action) => !!action)

  return (
    <div className={cn('min-w-0', comment.status === 'hidden' && 'opacity-60', fresh && 'comment-new')}>
      <div className={cn('flex flex-wrap items-center gap-x-2 gap-y-1', compact ? 'text-xs' : 'text-sm')}>
        <UserAvatar user={author} className={compact ? 'size-5 text-[0.6rem]' : 'size-7 text-xs'} />
        <span className={comment.author.deleted ? 'text-muted-foreground italic' : cn('font-medium', compact && 'text-sm')}>
          {author.displayName}
        </span>
        {comment.author.role === 'admin' && <Badge variant="secondary">{t('comments.author')}</Badge>}
        {comment.author.role === 'moderator' && <Badge variant="outline">{t('comments.moderator')}</Badge>}
        {comment.replyTo && (
          <span className="text-muted-foreground">
            {t('comments.replyTo')} <span className="text-foreground">@{comment.replyTo.displayName || t('comments.deletedUser')}</span>
          </span>
        )}
        <span className="text-muted-foreground">
          <LocalDate iso={comment.createdAt} lang={lang} />
        </span>
        {comment.editedAt && <span className="text-xs text-muted-foreground">({t('comments.edited')})</span>}
        {comment.status === 'hidden' && <Badge variant="outline">{t('comments.hidden')}</Badge>}
      </div>

      {editing ? (
        <div className="mt-2 grid gap-2">
          <Textarea
            value={draft}
            onChange={(event) => setDraft(event.target.value)}
            maxLength={maxLength}
            rows={3}
            aria-label={t('comments.edit')}
          />
          <div className="flex justify-end gap-2">
            <Button size="sm" variant="ghost" onClick={() => setEditing(false)}>
              {t('comments.cancel')}
            </Button>
            <Button
              size="sm"
              disabled={update.isPending || draft.trim() === ''}
              onClick={() => update.mutate({ id: comment.id, body: draft }, { onSuccess: () => setEditing(false), onError })}
            >
              {t('comments.save')}
            </Button>
          </div>
        </div>
      ) : (
        <>
          {quote && <PassageQuote comment={comment} threadId={threadId} />}
          <ClampedText text={comment.body} className={compact ? 'text-sm' : undefined} />
        </>
      )}

      {!editing && (
        <div className={cn('mt-1 flex flex-wrap items-center gap-1 text-muted-foreground', compact && '-ml-2')}>
          {comment.status === 'visible' && <Reactions target={`comment:${comment.id}`} className="mr-1" />}
          {canReply && (
            <Button variant="ghost" size="xs" onClick={() => onReply({ id: comment.id, name: author.displayName })}>
              {t('comments.reply')}
            </Button>
          )}
          {compact
            ? // Beside the post: the rest in a menu, so the row stays one line
              actions.length > 0 && (
                <DropdownMenu>
                  <DropdownMenuTrigger asChild>
                    <Button variant="ghost" size="icon-xs" aria-label={t('nav.more')}>
                      <EllipsisIcon />
                    </Button>
                  </DropdownMenuTrigger>
                  <DropdownMenuContent align="start">
                    {actions.map((action) => (
                      <DropdownMenuItem key={action.key} variant={action.destructive ? 'destructive' : 'default'} onSelect={action.run}>
                        {action.label}
                      </DropdownMenuItem>
                    ))}
                  </DropdownMenuContent>
                </DropdownMenu>
              )
            : actions.map((action) => (
                <Button key={action.key} variant="ghost" size="xs" onClick={action.run}>
                  {action.label}
                </Button>
              ))}
          <AlertDialog open={confirmDelete} onOpenChange={setConfirmDelete}>
            <AlertDialogContent>
              <AlertDialogHeader>
                <AlertDialogTitle>{t('comments.deleteConfirm')}</AlertDialogTitle>
              </AlertDialogHeader>
              <AlertDialogFooter>
                <AlertDialogCancel>{t('comments.cancel')}</AlertDialogCancel>
                <AlertDialogAction variant="destructive" onClick={() => remove.mutate(comment.id, { onError })}>
                  {t('comments.delete')}
                </AlertDialogAction>
              </AlertDialogFooter>
            </AlertDialogContent>
          </AlertDialog>
        </div>
      )}
    </div>
  )
}

function Thread({ thread, user, canReply }: { thread: CommentThread; user: User | null | undefined; canReply: boolean }) {
  const [replyTo, setReplyTo] = useState<{ id: number; name: string } | null>(null)
  return (
    <li id={`comment-${thread.id}`} className="scroll-mt-24 py-4">
      <CommentItem
        comment={thread}
        threadId={thread.id}
        user={user}
        canReply={canReply && thread.status === 'visible'}
        onReply={setReplyTo}
      />
      {(thread.replies.length > 0 || replyTo) && (
        <ul className="mt-3 grid gap-3 border-l-2 pl-4">
          {thread.replies.map((reply) => (
            <li key={reply.id} id={`comment-${reply.id}`} className="scroll-mt-24">
              <CommentItem comment={reply} threadId={thread.id} user={user} canReply={canReply} onReply={setReplyTo} />
            </li>
          ))}
          {replyTo && (
            <li>
              <Composer target={thread.target} replyTo={replyTo} onDone={() => setReplyTo(null)} autoFocus />
            </li>
          )}
        </ul>
      )}
    </li>
  )
}

// Comments load in the browser: prerendered pages would show them only until the next build
export function Comments({ target, enabled = true }: { target: CommentTarget; enabled?: boolean }) {
  const { t } = useTranslation()
  const lang = useLang()
  const location = useLocation()
  const me = useMe()
  const comments = useComments(target)
  const user = me.data?.user
  const canComment = enabled && !!user?.emailVerified && !user.denied?.includes('comment')

  let composer: React.ReactNode
  if (!enabled) composer = <p className="text-sm text-muted-foreground">{t('comments.closed')}</p>
  else if (me.isPending) composer = <Skeleton className="h-24 w-full" />
  else if (!user)
    composer = (
      <p className="text-sm">
        <Link to={`${localePath(lang, '/login')}?next=${encodeURIComponent(location.pathname)}`} className="text-primary underline">
          {t('comments.loginToComment')}
        </Link>
      </p>
    )
  else if (!user.emailVerified) composer = <p className="text-sm text-muted-foreground">{t('comments.verifyToComment')}</p>
  else if (user.denied?.includes('comment')) composer = <p className="text-sm text-muted-foreground">{t('comments.notAllowed')}</p>
  else composer = <Composer target={target} />

  const threads = comments.data?.pages.flatMap((page) => page.threads) ?? []
  // What others write meanwhile comes by itself (components/live.tsx)
  useWatch(target)
  const fresh = useFreshComments(
    target,
    comments.data ? threads.flatMap((thread) => [thread, ...thread.replies]) : undefined,
    comments.data?.pages.length ?? 0,
    user?.id,
  )

  // #comment-12 (a reply mail's link): shown once it is loaded, the next pages loaded for it if needed
  const hash = location.hash
  const shown = useRef('')
  useEffect(() => {
    if (!hash.startsWith('#comment-') || shown.current === hash || !comments.isSuccess) return
    const element = document.getElementById(hash.slice(1))
    if (element) {
      shown.current = hash
      element.scrollIntoView({ behavior: 'smooth', block: 'center' })
    } else if (comments.hasNextPage && !comments.isFetchingNextPage) void comments.fetchNextPage()
  }, [hash, comments, threads.length])

  return (
    <section className="mx-auto max-w-3xl" aria-labelledby="comments-title">
      <h2 id="comments-title" className="mb-4 text-xl font-semibold">
        {t('comments.title')}
      </h2>
      {composer}
      <FormError error={comments.error} />
      {comments.isPending && <Skeleton className="mt-6 h-32 w-full" />}
      {comments.isSuccess && threads.length === 0 && <p className="mt-6 text-muted-foreground">{t('comments.empty')}</p>}
      <ReactionsProvider targets={threads.flatMap((thread) => [thread, ...thread.replies]).map((comment) => `comment:${comment.id}`)}>
        <FreshComments value={fresh}>
          <ul className="mt-4 divide-y">
            {threads.map((thread) => (
              <Thread key={thread.id} thread={thread} user={user} canReply={canComment} />
            ))}
          </ul>
        </FreshComments>
      </ReactionsProvider>
      {comments.hasNextPage && (
        <div className="mt-4 flex justify-center">
          <Button variant="outline" onClick={() => void comments.fetchNextPage()} disabled={comments.isFetchingNextPage}>
            {t('comments.loadMore')}
          </Button>
        </div>
      )}
    </section>
  )
}
