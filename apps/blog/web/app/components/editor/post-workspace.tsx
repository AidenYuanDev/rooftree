import { BookmarkIcon, CodeIcon, EyeIcon, PlusIcon, SendIcon, WifiOffIcon, XIcon } from 'lucide-react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useNavigate } from 'react-router'
import { toast } from 'sonner'
import { PostEditorsDialog } from '~/components/admin/post-editors'
import { PostRevisionsDialog } from '~/components/admin/post-revisions'
import { UploadButton } from '~/components/admin/upload-button'
import { CollabField } from '~/components/editor/collab-field'
import { TagPicker } from '~/components/editor/tag-picker'
import { FormError } from '~/components/form'
import { Badge } from '~/components/ui/badge'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle, DialogTrigger } from '~/components/ui/dialog'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '~/components/ui/select'
import { Switch } from '~/components/ui/switch'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '~/components/ui/tabs'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage } from '~/i18n'
import { api } from '~/lib/api'
import { CollabConnection, insertAtCursor, type Peer, type PeerUser, peerColor } from '~/lib/collab'
import { localePath, useLang } from '~/lib/lang'
import { type PostSettings, useMe, usePublishPost, useSavePostSettings, useSavePostVersion, useWithdrawReview } from '~/lib/queries'
import { type AdminPost, AdminCommentList, canModerate, type Lang, langs } from '~/lib/schemas'

// A post as its editors see it: its text written together, live (lib/collab.ts), as a working copy
// readers get once someone with the publish right publishes it; its settings saved apart, by who may
// change them

type Texts = Partial<Record<Lang, { title?: string; summary?: string; body?: string }>>

// An ISO time as a datetime-local value, in the reader's zone
function localInput(iso: string | null) {
  if (!iso) return ''
  const date = new Date(iso)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`
}

export function PostWorkspace({ post }: { post: AdminPost }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const publish = usePublishPost(post.id)
  const withdraw = useWithdrawReview(post.id)
  const me = useMe().data?.user
  const [connection, setConnection] = useState<CollabConnection | null>(null)
  const [online, setOnline] = useState(true)
  const [texts, setTexts] = useState<Texts>({})
  const [peers, setPeers] = useState<Record<string, Peer[]>>({})
  const [closed, setClosed] = useState<'removed' | 'deleted' | null>(null)
  // Languages begun here, not in the working copy yet
  const [added, setAdded] = useState<Lang[]>([])
  const present = langs.filter((l) => post.translations[l] || post.draftLangs.includes(l) || added.includes(l))
  const [activeLang, setActiveLang] = useState<Lang>(post.originalLang)

  useEffect(() => {
    // In the browser only; its fields open their documents on it once it's there
    const created = new CollabConnection()
    const frame = requestAnimationFrame(() => setConnection(created))
    const stop = created.onOnline(setOnline)
    return () => {
      cancelAnimationFrame(frame)
      stop()
      created.dispose()
    }
  }, [])

  const onText = useCallback(
    (l: Lang, field: 'title' | 'summary' | 'body') => (text: string) =>
      setTexts((current) => ({ ...current, [l]: { ...current[l], [field]: text } })),
    [],
  )
  const onPeers = useCallback((doc: string, list: Peer[]) => setPeers((current) => ({ ...current, [doc]: list })), [])
  const onError = useCallback((code: string) => toast.error(code === 'too_long' ? t('collab.tooLong') : t('collab.refused')), [t])

  // Who else is here, once each, and what they are on
  const editing = new Map<number, { user: PeerUser; docs: string[] }>()
  for (const [doc, list] of Object.entries(peers))
    for (const peer of list) {
      const entry = editing.get(peer.user.id) ?? { user: peer.user, docs: [] }
      entry.docs.push(doc)
      editing.set(peer.user.id, entry)
    }

  // What readers don't have yet
  const changed = present.some((l) => {
    const live = post.translations[l]
    const text = texts[l]
    if (!text) return post.draftLangs.includes(l) && !live
    if (!live) return !!(text.title || text.body)
    return (
      (text.title !== undefined && text.title !== live.title) ||
      (text.summary !== undefined && text.summary !== live.summary) ||
      (text.body !== undefined && text.body !== live.body)
    )
  })
  const canPublish = post.rights.includes('publish')

  const onPublish = () =>
    publish.mutate(undefined, {
      onSuccess: (saved) => {
        // With reviews, a writer's publishing asks the admin
        if (saved.reviewRequestedAt && saved.status === post.status && !canModerate(me)) return toast.success(t('collab.reviewAsked'))
        toast.success(post.status === 'published' ? t('collab.updated') : t('collab.published'))
        // Comments on passages whose text the post no longer has: the moderators' to look at
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
    <div className="grid gap-6">
      <div className="flex flex-wrap items-center gap-3">
        <h1 className="mr-auto text-2xl font-semibold tracking-tight">
          {texts[post.originalLang]?.title || post.translations[post.originalLang]?.title || post.slug}
        </h1>
        <div className="flex items-center -space-x-2" aria-label={t('collab.editingNow')}>
          {[...editing.values()].map(({ user }) => (
            <span
              key={user.id}
              title={user.displayName}
              className="rounded-full ring-2"
              style={{ ['--tw-ring-color' as string]: peerColor(user.id) }}
            >
              <UserAvatar user={user} className="size-7 text-xs" />
            </span>
          ))}
        </div>
        {!online && (
          <Badge variant="outline" className="gap-1 text-destructive">
            <WifiOffIcon className="size-3" />
            {t('collab.offline')}
          </Badge>
        )}
        <PostEditorsDialog postId={post.id} rights={post.rights} />
        <SaveVersion post={post} />
        <PostRevisionsDialog post={post} />
        {post.status === 'published' && (
          <Button type="button" variant="ghost" asChild>
            <Link to={localePath(lang, `/posts/${post.slug}`)}>{t('admin.view')}</Link>
          </Button>
        )}
        {canPublish && (
          <Button type="button" onClick={onPublish} disabled={publish.isPending || (post.status === 'published' && !changed)}>
            <SendIcon />
            {post.status === 'published' ? t('collab.publishUpdate') : t('collab.publish')}
          </Button>
        )}
      </div>

      <p className="-mt-3 text-sm text-muted-foreground">
        {post.status === 'published' ? (changed ? t('collab.unpublished') : t('collab.upToDate')) : t('collab.draft')}
      </p>

      {post.reviewRequestedAt && (
        <div role="status" className="flex flex-wrap items-center gap-3 rounded-md border border-primary/40 bg-primary/5 p-3 text-sm">
          <span className="mr-auto">{me?.role === 'admin' ? t('collab.reviewWaiting') : t('collab.reviewPending')}</span>
          {post.rights.includes('publish') && (
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

      <Settings post={post} />

      <Tabs value={activeLang} onValueChange={(value) => setActiveLang(value as Lang)}>
        <div className="flex flex-wrap items-center gap-2">
          <TabsList>
            {present.map((l) => (
              <TabsTrigger key={l} value={l}>
                {t(`langs.${l}`)}
                {!post.translations[l] && <span className="ml-1 text-xs text-muted-foreground">({t('collab.notPublished')})</span>}
              </TabsTrigger>
            ))}
          </TabsList>
          {langs
            .filter((l) => !present.includes(l))
            .map((l) => (
              <Button
                key={l}
                type="button"
                variant="outline"
                size="sm"
                onClick={() => {
                  setAdded([...added, l])
                  setActiveLang(l)
                }}
              >
                <PlusIcon />
                {t('admin.addTranslation', { lang: t(`langs.${l}`) })}
              </Button>
            ))}
          {activeLang !== post.originalLang && post.rights.includes('settings') && post.translations[activeLang] && (
            <RemoveLanguage post={post} lang={activeLang} onRemoved={() => setActiveLang(post.originalLang)} />
          )}
        </div>
        {present.map((l) => (
          <TabsContent key={l} value={l} className="mt-4" forceMount hidden={l !== activeLang}>
            <LanguageFields
              connection={connection}
              postId={post.id}
              lang={l}
              onText={onText}
              onPeers={onPeers}
              onClosed={setClosed}
              onError={onError}
            />
          </TabsContent>
        ))}
      </Tabs>
    </div>
  )
}

function LanguageFields({
  connection,
  postId,
  lang,
  onText,
  onPeers,
  onClosed,
  onError,
}: {
  connection: CollabConnection | null
  postId: number
  lang: Lang
  onText: (l: Lang, field: 'title' | 'summary' | 'body') => (text: string) => void
  onPeers: (doc: string, peers: Peer[]) => void
  onClosed: (reason: 'removed' | 'deleted') => void
  onError: (code: string) => void
}) {
  const { t } = useTranslation()
  const [sourceMode, setSourceMode] = useState(false)
  const bodyHost = useRef<HTMLDivElement>(null)
  const doc = (field: string) => `post:${postId}:${lang}:${field}`
  const common = { connection, lang, onPeers, onClosed, onError }

  return (
    <div className="grid gap-4">
      <div className="grid gap-1.5">
        <Label>{t('admin.title_')}</Label>
        <CollabField {...common} doc={doc('title')} kind="line" label={t('admin.title_')} onText={onText(lang, 'title')} />
      </div>
      <div className="grid gap-1.5">
        <Label>{t('admin.summary')}</Label>
        <CollabField {...common} doc={doc('summary')} kind="text" label={t('admin.summary')} onText={onText(lang, 'summary')} />
      </div>
      <div className="grid gap-1.5">
        <div className="flex items-center gap-2">
          <Label className="mr-auto">{t('admin.body')}</Label>
          <UploadButton
            label={t('admin.uploadImage')}
            postId={postId}
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
        <div ref={bodyHost}>
          <CollabField
            {...common}
            doc={doc('body')}
            kind="markdown"
            label={t('admin.body')}
            sourceMode={sourceMode}
            onToggleSource={() => setSourceMode((current) => !current)}
            className="min-h-[24rem]"
            onText={onText(lang, 'body')}
          />
        </div>
      </div>
    </div>
  )
}

// A published language taken away, its working copy with it
function RemoveLanguage({ post, lang, onRemoved }: { post: AdminPost; lang: Lang; onRemoved: () => void }) {
  const { t, i18n } = useTranslation()
  const save = useSavePostSettings(post.id)
  return (
    <Button
      type="button"
      variant="ghost"
      size="sm"
      className="ml-auto"
      disabled={save.isPending}
      onClick={() =>
        save.mutate(
          { translations: { [lang]: null } },
          { onSuccess: onRemoved, onError: (error) => toast.error(errorMessage(i18n, error)) },
        )
      }
    >
      <XIcon />
      {t('admin.removeTranslation', { lang: t(`langs.${lang}`) })}
    </Button>
  )
}

// What isn't text: by who may change it (the settings right, the publish right for its state)
function Settings({ post }: { post: AdminPost }) {
  const { t, i18n } = useTranslation()
  const save = useSavePostSettings(post.id)
  const [values, setValues] = useState({
    slug: post.slug,
    cover: post.cover,
    status: post.status,
    publishAt: localInput(post.publishAt),
    originalLang: post.originalLang,
    commentsEnabled: post.commentsEnabled,
    tags: post.tags,
  })
  const canSettings = post.rights.includes('settings')
  const canPublish = post.rights.includes('publish')
  if (!canSettings && !canPublish) return null
  const set = <K extends keyof typeof values>(key: K, value: (typeof values)[K]) => setValues({ ...values, [key]: value })

  const submit = (event: React.FormEvent) => {
    event.preventDefault()
    const input: PostSettings = {}
    if (canSettings) {
      Object.assign(input, {
        slug: values.slug,
        cover: values.cover,
        originalLang: values.originalLang,
        commentsEnabled: values.commentsEnabled,
        tags: values.tags,
      })
    }
    if (canPublish) {
      input.status = values.status
      input.publishAt = values.status === 'draft' && values.publishAt ? new Date(values.publishAt).toISOString() : null
    }
    save.mutate(input, { onSuccess: () => toast.success(t('admin.saved')), onError: (error) => toast.error(errorMessage(i18n, error)) })
  }

  return (
    <form onSubmit={submit} className="grid gap-4 rounded-lg border p-4 md:grid-cols-2">
      <div className="grid gap-1.5">
        <Label htmlFor="post-slug">{t('admin.slug')}</Label>
        <Input
          id="post-slug"
          className="font-mono"
          disabled={!canSettings}
          value={values.slug}
          onChange={(e) => set('slug', e.target.value)}
        />
      </div>
      <div className="grid gap-1.5">
        <Label htmlFor="post-cover">{t('admin.cover')}</Label>
        <div className="flex gap-2">
          <Input id="post-cover" disabled={!canSettings} value={values.cover} onChange={(e) => set('cover', e.target.value)} />
          {canSettings && <UploadButton label={t('admin.uploadCover')} postId={post.id} onUploaded={(url) => set('cover', url)} />}
        </div>
      </div>
      <div className="grid gap-1.5">
        <Label htmlFor="post-status">{t('admin.status')}</Label>
        <Select value={values.status} disabled={!canPublish} onValueChange={(value) => set('status', value as AdminPost['status'])}>
          <SelectTrigger id="post-status" className="w-full">
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            <SelectItem value="draft">{t('admin.statusDraft')}</SelectItem>
            <SelectItem value="published">{t('admin.statusPublished')}</SelectItem>
          </SelectContent>
        </Select>
      </div>
      {values.status === 'draft' && (
        <div className="grid gap-1.5">
          <Label htmlFor="post-publish-at">{t('admin.publishAt')}</Label>
          <Input
            id="post-publish-at"
            type="datetime-local"
            disabled={!canPublish}
            value={values.publishAt}
            onChange={(e) => set('publishAt', e.target.value)}
          />
        </div>
      )}
      <div className="grid gap-1.5">
        <Label htmlFor="post-lang">{t('admin.originalLang')}</Label>
        <Select value={values.originalLang} disabled={!canSettings} onValueChange={(value) => set('originalLang', value as Lang)}>
          <SelectTrigger id="post-lang" className="w-full">
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
      <div className="grid gap-2 md:col-span-2">
        <Label>{t('admin.tags')}</Label>
        <TagPicker value={values.tags} onChange={(slugs) => set('tags', slugs)} disabled={!canSettings} />
      </div>
      <div className="flex items-center gap-2">
        <Switch
          id="comments-enabled"
          disabled={!canSettings}
          checked={values.commentsEnabled}
          onCheckedChange={(checked) => set('commentsEnabled', checked)}
        />
        <Label htmlFor="comments-enabled">{t('admin.commentsEnabled')}</Label>
      </div>
      <div className="flex justify-end">
        <Button type="submit" variant="outline" disabled={save.isPending}>
          {save.isPending ? t('admin.saving') : t('collab.saveSettings')}
        </Button>
      </div>
      <div className="md:col-span-2">
        <FormError error={save.error} />
      </div>
    </form>
  )
}

// The working copy as a version to go back to, with a note
function SaveVersion({ post }: { post: AdminPost }) {
  const { t, i18n } = useTranslation()
  const save = useSavePostVersion(post.id)
  const [open, setOpen] = useState(false)
  const [note, setNote] = useState('')
  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>
        <Button type="button" variant="ghost">
          <BookmarkIcon />
          {t('history.save')}
        </Button>
      </DialogTrigger>
      <DialogContent className="sm:max-w-md">
        <form
          className="grid gap-4"
          onSubmit={(event) => {
            event.preventDefault()
            save.mutate(note.trim(), {
              onSuccess: (saved) => {
                toast.success(saved.revisions.length ? t('history.savedVersion') : t('history.nothingToSave'))
                setNote('')
                setOpen(false)
              },
              onError: (error) => toast.error(errorMessage(i18n, error)),
            })
          }}
        >
          <DialogHeader>
            <DialogTitle>{t('history.save')}</DialogTitle>
            <DialogDescription>{t('history.saveHint')}</DialogDescription>
          </DialogHeader>
          <Input
            aria-label={t('history.note')}
            placeholder={t('history.note')}
            maxLength={200}
            value={note}
            onChange={(e) => setNote(e.target.value)}
          />
          <DialogFooter>
            <Button type="submit" disabled={save.isPending}>
              {t('history.save')}
            </Button>
          </DialogFooter>
        </form>
      </DialogContent>
    </Dialog>
  )
}
