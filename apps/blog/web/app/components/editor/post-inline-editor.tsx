import { useQueryClient } from '@tanstack/react-query'
import { ArrowLeftIcon, CodeIcon, EyeIcon, SendIcon, WifiOffIcon } from 'lucide-react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useNavigate } from 'react-router'
import { toast } from 'sonner'
import { PostEditorsDialog } from '~/components/admin/post-editors'
import { UploadButton } from '~/components/admin/upload-button'
import { CollabField } from '~/components/editor/collab-field'
import { FormError } from '~/components/form'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Skeleton } from '~/components/ui/skeleton'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage } from '~/i18n'
import { api } from '~/lib/api'
import { CollabConnection, insertAtCursor, type Peer, type PeerUser, peerColor } from '~/lib/collab'
import { localePath } from '~/lib/lang'
import { queryKeys, useAdminPost, useMe, usePublishPost, useWithdrawReview } from '~/lib/queries'
import { type AdminPost, AdminCommentList, canModerate, type Lang } from '~/lib/schemas'
import { useContentVersion } from '~/stores/live'

// Editing a post in place on its reading page: single-column inside the paper card, live with any
// other editors, in the language of the page
export function PostInlineEditor({
  postId,
  lang,
  backLabel,
  onDone,
}: {
  postId: number
  lang: Lang
  backLabel?: string
  onDone: () => void
}) {
  const { t } = useTranslation()
  const queryClient = useQueryClient()
  const post = useAdminPost(postId)
  const version = useContentVersion(`post:${postId}`)
  const label = backLabel ?? t('collab.done')

  useEffect(() => {
    if (version) void queryClient.invalidateQueries({ queryKey: queryKeys.adminPost(postId) })
  }, [version, postId, queryClient])

  if (post.isPending)
    return (
      <div className="grid gap-4">
        <div className="flex items-center border-b pb-4">
          <Button type="button" variant="ghost" size="sm" onClick={onDone}>
            <ArrowLeftIcon />
            {label}
          </Button>
        </div>
        <Skeleton className="h-10 w-2/3" />
        <Skeleton className="h-96 w-full" />
      </div>
    )

  if (post.error || !post.data)
    return (
      <div className="grid gap-4">
        <div>
          <Button type="button" variant="ghost" size="sm" onClick={onDone}>
            <ArrowLeftIcon />
            {label}
          </Button>
        </div>
        <FormError error={post.error} />
      </div>
    )

  return <InlineWorkspace key={`${post.data.id}:${lang}`} post={post.data} lang={lang} backLabel={label} onDone={onDone} />
}

function InlineWorkspace({ post, lang, backLabel, onDone }: { post: AdminPost; lang: Lang; backLabel: string; onDone: () => void }) {
  const { t, i18n } = useTranslation()
  const navigate = useNavigate()
  const publish = usePublishPost(post.id)
  const withdraw = useWithdrawReview(post.id)
  const me = useMe().data?.user
  const bodyHost = useRef<HTMLDivElement>(null)
  const [connection, setConnection] = useState<CollabConnection | null>(null)
  const [online, setOnline] = useState(true)
  const [sourceMode, setSourceMode] = useState(false)
  const [text, setText] = useState<{ title?: string; summary?: string; body?: string }>({})
  const [peers, setPeers] = useState<Record<string, Peer[]>>({})
  const [closed, setClosed] = useState<'removed' | 'deleted' | null>(null)

  useEffect(() => {
    const created = new CollabConnection()
    const frame = requestAnimationFrame(() => setConnection(created))
    const stop = created.onOnline(setOnline)
    return () => {
      cancelAnimationFrame(frame)
      stop()
      created.dispose()
    }
  }, [])

  const onField = useCallback(
    (field: 'title' | 'summary' | 'body') => (value: string) => setText((current) => ({ ...current, [field]: value })),
    [],
  )
  const onPeers = useCallback((doc: string, list: Peer[]) => setPeers((current) => ({ ...current, [doc]: list })), [])
  const onError = useCallback((code: string) => toast.error(code === 'too_long' ? t('collab.tooLong') : t('collab.refused')), [t])

  const editing = new Map<number, PeerUser>()
  for (const list of Object.values(peers)) for (const peer of list) editing.set(peer.user.id, peer.user)

  const live = post.translations[lang]
  const changed = !live
    ? !!(text.title || text.body)
    : (text.title !== undefined && text.title !== live.title) ||
      (text.summary !== undefined && text.summary !== live.summary) ||
      (text.body !== undefined && text.body !== live.body)
  const canPublish = post.rights.includes('publish')
  const doc = (field: string) => `post:${post.id}:${lang}:${field}`

  const onPublish = () =>
    publish.mutate(undefined, {
      onSuccess: (saved) => {
        if (saved.reviewRequestedAt && saved.status === post.status && !canModerate(me)) return toast.success(t('collab.reviewAsked'))
        toast.success(post.status === 'published' ? t('collab.updated') : t('collab.published'))
        // Publishing a draft from /posts?draft=... lands straight on the published post page
        if (post.status === 'draft' && saved.status === 'published') {
          void navigate(localePath(lang, `/posts/${saved.slug}`))
          return
        }
        if (!canModerate(me)) return
        void api('/api/admin/comments?passage=orphaned', AdminCommentList)
          .then((list) => {
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
          .catch(() => undefined)
      },
      onError: (error) => toast.error(errorMessage(i18n, error)),
    })

  return (
    <div className="grid gap-4">
      <div className="flex flex-wrap items-center gap-2 border-b pb-4">
        <Button type="button" variant="ghost" size="sm" onClick={onDone}>
          <ArrowLeftIcon />
          {backLabel}
        </Button>
        <div className="flex items-center -space-x-2" aria-label={t('collab.editingNow')}>
          {[...editing.values()].map((user) => (
            <span
              key={user.id}
              title={user.displayName}
              className="rounded-full ring-2"
              style={{ ['--tw-ring-color' as string]: peerColor(user.id) }}
            >
              <UserAvatar user={user} className="size-6 text-[0.65rem]" />
            </span>
          ))}
        </div>
        {!online && (
          <Badge variant="outline" className="gap-1 text-destructive">
            <WifiOffIcon className="size-3" />
            {t('collab.offline')}
          </Badge>
        )}

        <div className="ml-auto flex flex-wrap items-center gap-1.5">
          <UploadButton
            label={t('admin.uploadImage')}
            postId={post.id}
            onUploaded={(url) => {
              if (insertAtCursor(bodyHost.current, `![](${url})`)) toast.success(t('admin.uploaded'))
            }}
          />
          <Button
            type="button"
            variant="ghost"
            size="sm"
            aria-pressed={sourceMode}
            title="Ctrl+/"
            onClick={() => setSourceMode((current) => !current)}
          >
            {sourceMode ? <EyeIcon /> : <CodeIcon />}
            {sourceMode ? t('collab.livePreview') : t('collab.source')}
          </Button>
          <PostEditorsDialog postId={post.id} rights={post.rights} size="sm" />
          {canPublish && (
            <Button type="button" size="sm" onClick={onPublish} disabled={publish.isPending || (post.status === 'published' && !changed)}>
              <SendIcon />
              {post.status === 'published' ? t('collab.publishUpdate') : t('collab.publish')}
            </Button>
          )}
        </div>
      </div>

      <p className="text-xs text-muted-foreground">
        {post.status === 'published' ? (changed ? t('collab.unpublished') : t('collab.upToDate')) : t('collab.draft')}
      </p>

      {post.reviewRequestedAt && (
        <div role="status" className="flex flex-wrap items-center gap-3 rounded-md border border-primary/40 bg-primary/5 p-3 text-sm">
          <span className="mr-auto">{me?.role === 'admin' ? t('collab.reviewWaiting') : t('collab.reviewPending')}</span>
          {canPublish && (
            <Button
              type="button"
              size="sm"
              variant="outline"
              disabled={withdraw.isPending}
              onClick={() => withdraw.mutate(undefined, { onError: (error) => toast.error(errorMessage(i18n, error)) })}
            >
              {me?.role === 'admin' ? t('collab.reviewDecline') : t('collab.reviewWithdraw')}
            </Button>
          )}
        </div>
      )}

      {closed && (
        <p role="alert" className="rounded-md border border-destructive/40 bg-destructive/10 p-3 text-sm text-destructive">
          {closed === 'removed' ? t('collab.removed') : t('collab.deleted')}
        </p>
      )}

      <div className="grid gap-4">
        <CollabField
          connection={connection}
          doc={doc('title')}
          kind="line"
          label={t('admin.title_')}
          placeholder={t('admin.title_')}
          lang={lang}
          className="font-heading text-2xl font-bold md:text-3xl"
          onText={onField('title')}
          onPeers={onPeers}
          onClosed={setClosed}
          onError={onError}
        />
        <CollabField
          connection={connection}
          doc={doc('summary')}
          kind="text"
          label={t('admin.summary')}
          placeholder={t('collab.summaryOptional')}
          lang={lang}
          className="text-muted-foreground"
          onText={onField('summary')}
          onPeers={onPeers}
          onClosed={setClosed}
          onError={onError}
        />
        <div ref={bodyHost}>
          <CollabField
            connection={connection}
            doc={doc('body')}
            kind="markdown"
            label={t('admin.body')}
            placeholder={t('admin.body')}
            lang={lang}
            sourceMode={sourceMode}
            onToggleSource={() => setSourceMode((current) => !current)}
            className="min-h-[24rem]"
            onText={onField('body')}
            onPeers={onPeers}
            onClosed={setClosed}
            onError={onError}
          />
        </div>
      </div>
    </div>
  )
}
