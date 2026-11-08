import { XIcon } from 'lucide-react'
import { useEffect, useMemo, useState } from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import { Link, useLocation, useSearchParams } from 'react-router'
import { toast } from 'sonner'
import { CommentItem, Composer } from '~/components/comments'
import { ReactionsProvider } from '~/components/reactions'
import { Button } from '~/components/ui/button'
import { errorMessage } from '~/i18n'
import { FreshComments, useFreshComments } from '~/lib/fresh-comments'
import { localePath } from '~/lib/lang'
import { capture, type Captured, highlight, locate, markerAttribute } from '~/lib/passage'
import { useAnchoredComments, useComments, useMe, useUpdateComment } from '~/lib/queries'
import { canModerate, type Comment, type CommentAnchor, type CommentTarget, type CommentThread, type Lang } from '~/lib/schemas'
import { cn } from '~/lib/utils'
import { usePassages } from '~/stores/passages'

// Comments on passages of a post, in its text: a 💬 where a thread's first comment selected its
// passage, with how many replies it has; a thread beside the post (a sheet from the bottom on a phone),
// the passage of the comment chosen in it highlighted. A selection in the post starts a thread, or is
// the passage of a reply, or a comment's new one. Only on a prerendered post: its text is the page's
// own, not React's, so markers can go into it

// The passage a comment is about: its own, else the one of the comment it answers, else its thread's
function passageOf(comment: Comment, thread: CommentThread): CommentAnchor | null {
  const byId = new Map([thread, ...thread.replies].map((item) => [item.id, item]))
  for (let at: Comment | undefined = comment; at; at = at.replyToId ? byId.get(at.replyToId) : undefined) {
    if (at.anchor) return at.anchor
    if (at.id === thread.id) break
  }
  return thread.anchor ?? null
}

interface Marker {
  host: HTMLElement
  threads: number[]
  replies: number
}

// The markers in the text; the threads whose passage the post lost
function useMarkers(container: React.RefObject<HTMLElement | null>, threads: CommentThread[], contentKey: string) {
  const [markers, setMarkers] = useState<Marker[]>([])
  const [lost, setLost] = useState<CommentThread[]>([])

  useEffect(() => {
    const placed: Marker[] = []
    // After layout, and its results by a callback: what it reads and writes is the page's text
    const frame = requestAnimationFrame(() => {
      const root = container.current
      if (!root) return

      const found: { thread: CommentThread; range: Range }[] = []
      const gone: CommentThread[] = []
      for (const thread of threads) {
        if (!thread.anchor) continue
        const range = thread.anchor.status === 'ok' ? locate(root, thread.anchor) : null
        if (range) found.push({ thread, range })
        else gone.push(thread)
      }

      // Threads on the same words share a marker, at the end of them all
      found.sort((a, b) => a.range.compareBoundaryPoints(Range.START_TO_START, b.range))
      const groups: { threads: CommentThread[]; range: Range }[] = []
      for (const item of found) {
        const last = groups.at(-1)
        if (last && last.range.comparePoint(item.range.startContainer, item.range.startOffset) <= 0) {
          last.threads.push(item.thread)
          if (last.range.comparePoint(item.range.endContainer, item.range.endOffset) > 0)
            last.range.setEnd(item.range.endContainer, item.range.endOffset)
        } else groups.push({ threads: [item.thread], range: item.range.cloneRange() })
      }

      // From the last: a marker put in splits a text node, the ranges before it stay where they were
      for (const group of [...groups].reverse()) {
        const host = document.createElement('span')
        host.setAttribute(markerAttribute, '')
        const end = group.range.cloneRange()
        end.collapse(false)
        end.insertNode(host)
        placed.unshift({
          host,
          threads: group.threads.map((thread) => thread.id),
          replies: group.threads.reduce((sum, thread) => sum + thread.replies.filter((reply) => reply.status === 'visible').length, 0),
        })
      }
      setMarkers([...placed])
      setLost(gone)
    })

    return () => {
      cancelAnimationFrame(frame)
      for (const { host } of placed) {
        const parent = host.parentNode
        host.remove()
        // The text node it split, whole again
        if (parent?.isConnected) parent.normalize()
      }
    }
  }, [container, threads, contentKey])

  return { markers, lost }
}

function MarkerButton({ marker }: { marker: Marker }) {
  const { t } = useTranslation()
  const openThreads = usePassages((state) => state.openThreads)
  const open = usePassages((state) => state.open)
  const active = !!open && marker.threads.some((id) => open.includes(id))
  // The 💬 and the count are CSS (app.css): never text of the post, selected or copied with it
  return (
    <button
      type="button"
      className={cn('passage-marker', active && 'passage-marker-active')}
      data-label={marker.replies ? `💬 ${marker.replies}` : '💬'}
      aria-label={marker.replies ? t('passages.marker', { count: marker.replies }) : t('passages.marker')}
      onClick={() => openThreads(marker.threads)}
    />
  )
}

// The selection in the post, where it is, and what it may be
function useSelectionInPost(container: React.RefObject<HTMLElement | null>, lang: Lang) {
  const [state, setState] = useState<{ captured: Captured; rect: DOMRect } | null>(null)
  useEffect(() => {
    let frame = 0
    const update = () => {
      cancelAnimationFrame(frame)
      frame = requestAnimationFrame(() => {
        const root = container.current
        const selection = window.getSelection()
        if (!root || !selection || selection.isCollapsed || selection.rangeCount === 0) return setState(null)
        const captured = capture(root, selection, lang)
        if (!captured.ok && (captured.error === 'outside' || captured.error === 'empty')) return setState(null)
        const rects = selection.getRangeAt(0).getClientRects()
        const last = rects[rects.length - 1] ?? selection.getRangeAt(0).getBoundingClientRect()
        setState({ captured, rect: last })
      })
    }
    document.addEventListener('selectionchange', update)
    return () => {
      cancelAnimationFrame(frame)
      document.removeEventListener('selectionchange', update)
    }
  }, [container, lang])
  return state
}

function SelectionButton({
  selection,
  target,
  canComment,
  loggedIn,
}: {
  selection: { captured: Captured; rect: DOMRect }
  target: CommentTarget
  canComment: boolean
  loggedIn: boolean
}) {
  const { t, i18n } = useTranslation()
  const location = useLocation()
  const lang: Lang = i18n.language === 'en' ? 'en' : 'zh'
  const picking = usePassages((state) => state.picking)
  const startNew = usePassages((state) => state.startNew)
  const setReplyAnchor = usePassages((state) => state.setReplyAnchor)
  const startPicking = usePassages((state) => state.startPicking)
  const update = useUpdateComment(target)

  // Below the selection's last line: a phone's own menu is above it
  const style = {
    top: selection.rect.bottom + window.scrollY + 8,
    left: Math.max(8, Math.min(selection.rect.left + window.scrollX, window.innerWidth - 200)),
  }
  const { captured } = selection
  let content: React.ReactNode
  if (!captured.ok)
    content = <span className="px-2 text-xs">{captured.error === 'tooLong' ? t('passages.tooLong') : t('passages.oneBlock')}</span>
  else if (picking?.kind === 'reply')
    content = (
      <Button size="sm" onClick={() => setReplyAnchor({ replyTo: picking.replyTo.id, anchor: captured.anchor })}>
        {t('passages.replyHere')}
      </Button>
    )
  else if (picking?.kind === 'reselect')
    content = (
      <Button
        size="sm"
        disabled={update.isPending}
        onClick={() =>
          update.mutate(
            { id: picking.commentId, anchor: captured.anchor },
            {
              onSuccess: () => {
                toast.success(t('passages.moved'))
                startPicking(null)
              },
              onError: (error) => toast.error(errorMessage(i18n, error)),
            },
          )
        }
      >
        {t('passages.moveHere')}
      </Button>
    )
  else if (canComment)
    content = (
      <Button size="sm" onClick={() => startNew(captured.anchor)}>
        {t('passages.commentHere')}
      </Button>
    )
  else if (!loggedIn)
    content = (
      <Button size="sm" variant="outline" asChild>
        <Link to={`${localePath(lang, '/login')}?next=${encodeURIComponent(location.pathname)}`}>{t('passages.loginToComment')}</Link>
      </Button>
    )
  else return null

  return createPortal(
    <div
      className="absolute z-40 rounded-lg border bg-popover p-1 text-popover-foreground shadow-lg"
      style={style}
      // Clicking keeps the selection until the button acts on it
      onMouseDown={(event) => event.preventDefault()}
    >
      {content}
    </div>,
    document.body,
  )
}

// A thread in the panel: its first comment, its last reply, the ones between folded
function ThreadView({ thread, target, canComment }: { thread: CommentThread; target: CommentTarget; canComment: boolean }) {
  const { t } = useTranslation()
  const me = useMe()
  const user = me.data?.user
  const focused = usePassages((state) => state.focused)
  const focus = usePassages((state) => state.focus)
  const replyAnchor = usePassages((state) => state.replyAnchor)
  const setReplyAnchor = usePassages((state) => state.setReplyAnchor)
  const startPicking = usePassages((state) => state.startPicking)
  const [replyTo, setReplyTo] = useState<{ id: number; name: string } | null>(null)
  const [unfolded, setUnfolded] = useState(false)

  // The chosen comment shows what is folded around it
  const folded = !unfolded && thread.replies.length > 2 && !thread.replies.slice(0, -1).some((reply) => reply.id === focused)
  const shown = folded ? thread.replies.slice(-1) : thread.replies
  const answered = replyTo && [thread, ...thread.replies].find((comment) => comment.id === replyTo.id)
  const ownAnchor = replyTo && replyAnchor?.replyTo === replyTo.id ? replyAnchor.anchor : null

  // The chosen one: a thin line beside it, its passage highlighted in the post
  const card = (comment: Comment) => (
    <div
      key={comment.id}
      className={cn(
        'cursor-pointer rounded-md border-l-2 py-1.5 pr-1 pl-2.5 transition-colors',
        focused === comment.id ? 'border-primary bg-primary/5' : 'border-transparent hover:bg-muted/50',
      )}
      onClick={() => focus(comment.id)}
    >
      <CommentItem
        comment={comment}
        threadId={thread.id}
        user={user}
        canReply={canComment && thread.status === 'visible'}
        onReply={setReplyTo}
        compact
        // The thread's passage is above it; a reply's own one is shown
        quote={comment.id !== thread.id}
      />
    </div>
  )

  return (
    <div className="grid gap-1">
      {card(thread)}
      {thread.replies.length > 0 && (
        <div className="ml-3 grid gap-0.5 border-l border-border/70 pl-2">
          {folded && (
            <button type="button" className="py-1 text-left text-xs text-primary hover:underline" onClick={() => setUnfolded(true)}>
              {t('passages.moreReplies', { count: thread.replies.length - 1 })}
            </button>
          )}
          {shown.map(card)}
        </div>
      )}
      {replyTo && answered && (
        <div className="mt-2 ml-3">
          <Composer
            target={target}
            replyTo={replyTo}
            anchor={ownAnchor}
            autoFocus
            onDone={() => {
              setReplyTo(null)
              setReplyAnchor(null)
            }}
            header={
              <div className="flex flex-wrap items-baseline gap-x-2 text-xs text-muted-foreground">
                {ownAnchor ? (
                  <>
                    <span>{t('passages.own')}</span>
                    <span className="line-clamp-1 italic">{ownAnchor.quote}</span>
                    <button type="button" className="text-primary hover:underline" onClick={() => setReplyAnchor(null)}>
                      {t('passages.useInherited')}
                    </button>
                  </>
                ) : (
                  <span>{t('passages.inherits')}</span>
                )}
                <button
                  type="button"
                  className="text-primary hover:underline"
                  onClick={() => startPicking({ kind: 'reply', threadId: thread.id, replyTo })}
                >
                  {t('passages.another')}
                </button>
              </div>
            }
          />
        </div>
      )}
    </div>
  )
}

export function Passages({
  container,
  target,
  lang,
  enabled,
  contentKey,
}: {
  container: React.RefObject<HTMLElement | null>
  target: CommentTarget
  lang: Lang
  enabled: boolean
  contentKey: string
}) {
  const { t } = useTranslation()
  const me = useMe()
  const user = me.data?.user
  const anchored = useAnchoredComments(target, lang)
  // A thread opened from the comment area whose first comment isn't on a passage: the area has it
  const comments = useComments(target)
  const threads = useMemo(() => anchored.data?.threads ?? [], [anchored.data])
  const { markers, lost } = useMarkers(container, threads, contentKey)
  const selection = useSelectionInPost(container, lang)

  const { open, focused, pending, picking, close, startPicking } = usePassages()
  const canComment = enabled && !!user?.emailVerified && !user.denied?.includes('comment')
  const findThread = (id: number) =>
    threads.find((thread) => thread.id === id) ?? comments.data?.pages.flatMap((page) => page.threads).find((thread) => thread.id === id)
  const openThreads = (open ?? []).map(findThread).filter((thread): thread is CommentThread => !!thread)
  const fresh = useFreshComments(target, anchored.data ? threads.flatMap((thread) => [thread, ...thread.replies]) : undefined, 1, user?.id)

  // An admin's link from its comments page: choose that comment's passage again
  const [searchParams, setSearchParams] = useSearchParams()
  const pick = Number(searchParams.get('pick'))
  // From the comment area of the other language's page: the thread of a passage of this one
  const linkedThread = Number(searchParams.get('thread'))
  const linkedComment = Number(searchParams.get('comment'))
  useEffect(() => {
    if (!linkedThread) return
    usePassages.getState().openThreads([linkedThread], linkedComment || undefined)
    setSearchParams(
      (params) => {
        params.delete('thread')
        params.delete('comment')
        return params
      },
      // Where the thread scrolled the page to stays
      { replace: true, preventScrollReset: true },
    )
  }, [linkedThread, linkedComment, setSearchParams])

  useEffect(() => {
    if (!pick || !canModerate(user)) return
    startPicking({ kind: 'reselect', commentId: pick })
    setSearchParams((params) => {
      params.delete('pick')
      return params
    })
  }, [pick, user, startPicking, setSearchParams])

  // The chosen comment's passage, highlighted and shown
  useEffect(() => {
    const root = container.current
    if (!root || focused === null) return highlight(null)
    const thread = openThreads.find((item) => item.id === focused || item.replies.some((reply) => reply.id === focused))
    const comment = thread && [thread, ...thread.replies].find((item) => item.id === focused)
    const passage = comment && thread ? passageOf(comment, thread) : null
    const range = passage && passage.status === 'ok' ? locate(root, passage) : null
    highlight(range)
    const rect = range?.getBoundingClientRect()
    if (rect && (rect.top < 80 || rect.bottom > window.innerHeight * 0.55)) window.scrollBy({ top: rect.top - 120, behavior: 'smooth' })
    // eslint-disable-next-line react-hooks/exhaustive-deps -- the threads' contents, not their array
  }, [container, focused, open, threads, contentKey])
  useEffect(() => () => highlight(null), [])

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') close()
    }
    document.addEventListener('keydown', onKey)
    return () => document.removeEventListener('keydown', onKey)
  }, [close])

  // Hidden, not gone, while a passage is picked in the text: a reply being written stays
  const panelOpen = pending !== null || openThreads.length > 0

  return (
    <>
      {markers.map((marker) => createPortal(<MarkerButton marker={marker} />, marker.host, `marker-${marker.threads.join('-')}`))}
      {selection && (canComment || !user || picking) && (
        <SelectionButton selection={selection} target={target} canComment={canComment} loggedIn={!!user} />
      )}

      {lost.length > 0 && (
        <details className="mt-8 rounded-xl border bg-muted/30 p-4 text-sm">
          <summary className="cursor-pointer font-medium">{t('passages.changedTitle', { count: lost.length })}</summary>
          <p className="mt-2 text-muted-foreground">{t('passages.changedHint')}</p>
          <ul className="mt-3 grid gap-3">
            {lost.map((thread) => (
              <li key={thread.id}>
                <button
                  type="button"
                  className="w-full rounded-lg border bg-background/60 p-3 text-left hover:border-primary/40"
                  onClick={() => usePassages.getState().openThreads([thread.id])}
                >
                  <BlockAsItWas anchor={thread.anchor!} />
                  <span className="mt-1 block text-xs text-muted-foreground">
                    {thread.author?.displayName} · {t('passages.moreReplies', { count: thread.replies.length })}
                  </span>
                </button>
              </li>
            ))}
          </ul>
        </details>
      )}

      {/* On the page itself: the paper the post is on has a backdrop filter, inside it fixed is the paper's */}
      {picking &&
        createPortal(
          <div className="fixed inset-x-4 bottom-4 z-40 mx-auto flex max-w-md items-center gap-3 rounded-xl border bg-popover p-3 text-sm shadow-lg">
            <span className="flex-1">{picking.kind === 'reply' ? t('passages.pickReply') : t('passages.pickReselect')}</span>
            <Button size="sm" variant="ghost" onClick={() => startPicking(null)}>
              {t('passages.cancelPick')}
            </Button>
          </div>,
          document.body,
        )}

      {panelOpen &&
        createPortal(
          <aside
            aria-label={pending ? t('passages.newTitle') : t('passages.panelTitle')}
            className={cn(
              'fixed z-40 flex flex-col border bg-popover/95 text-popover-foreground shadow-2xl backdrop-blur-md',
              picking && 'hidden',
              // A sheet from the bottom on a phone, a card beside the post on a wide screen, as tall as it needs
              'inset-x-0 bottom-0 max-h-[70dvh] rounded-t-3xl',
              'md:inset-x-auto md:top-20 md:right-6 md:bottom-auto md:max-h-[calc(100dvh-7rem)] md:w-[26rem] md:rounded-2xl',
            )}
          >
            {/* A phone's sheet: its handle */}
            <div className="mx-auto mt-2 h-1 w-10 shrink-0 rounded-full bg-muted-foreground/30 md:hidden" aria-hidden />
            <div className="flex items-center justify-between px-4 pt-2 pb-1 md:pt-3">
              <h2 className="text-sm font-semibold text-muted-foreground">{pending ? t('passages.newTitle') : t('passages.panelTitle')}</h2>
              <Button size="icon-sm" variant="ghost" aria-label={t('passages.close')} onClick={close}>
                <XIcon />
              </Button>
            </div>
            <div className="min-h-0 flex-1 overflow-y-auto px-4 pb-4">
              {pending ? (
                <Composer target={target} anchor={pending} autoFocus onDone={close} header={<PassageBlock quote={pending.quote} />} />
              ) : (
                <ReactionsProvider
                  targets={openThreads.flatMap((thread) => [thread, ...thread.replies]).map((comment) => `comment:${comment.id}`)}
                >
                  <FreshComments value={fresh}>
                    <div className="grid gap-4 divide-y divide-border/60">
                      {openThreads.map((thread) => (
                        <div key={thread.id} className="pt-4 first:pt-0">
                          {thread.anchor && <PassageBlock quote={thread.anchor.quote} className="mb-2" />}
                          <ThreadView thread={thread} target={target} canComment={canComment} />
                        </div>
                      ))}
                    </div>
                  </FreshComments>
                </ReactionsProvider>
              )}
            </div>
          </aside>,
          document.body,
        )}
    </>
  )
}

// A lost passage's block as it was, its quote marked
function BlockAsItWas({ anchor }: { anchor: CommentAnchor }) {
  const text = anchor.text ?? anchor.quote
  const at = text.indexOf(anchor.quote)
  if (at === -1) return <span className="italic">{anchor.quote}</span>
  return (
    <span className="line-clamp-4 text-muted-foreground">
      {text.slice(0, at)}
      <mark className="rounded bg-primary/15 px-0.5 text-foreground">{anchor.quote}</mark>
      {text.slice(at + anchor.quote.length)}
    </span>
  )
}

// The passage a thread is on, once at its top
function PassageBlock({ quote, className }: { quote: string; className?: string }) {
  return (
    <blockquote
      className={cn(
        'line-clamp-4 rounded-lg border-l-2 border-primary/60 bg-primary/5 px-3 py-2 text-sm [overflow-wrap:anywhere] whitespace-pre-wrap text-muted-foreground',
        className,
      )}
    >
      {quote}
    </blockquote>
  )
}
