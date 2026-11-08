import { UsersIcon, XIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle, DialogTrigger } from '~/components/ui/dialog'
import { Input } from '~/components/ui/input'
import { Skeleton } from '~/components/ui/skeleton'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage } from '~/i18n'
import { useMe, usePostEditors, useRemovePostEditor, useSetPostEditor, useTransferPost } from '~/lib/queries'
import { type AdminPost, type PostPerson, type PostRight, postRights } from '~/lib/schemas'

// Who edits a post besides its owner: each edits its text, and may do more by the rights it was
// given. Who has the editors right invites others, with no more rights than its own; the admin also
// gives the post to another owner
export function PostEditorsDialog({
  postId,
  rights: knownRights,
  size,
}: {
  postId: number
  rights?: AdminPost['rights']
  size?: 'default' | 'sm' | 'xs'
}) {
  const { t, i18n } = useTranslation()
  const me = useMe().data?.user
  const [open, setOpen] = useState(false)
  const editors = usePostEditors(open ? postId : null)
  const set = useSetPostEditor(postId)
  const remove = useRemovePostEditor(postId)
  const [username, setUsername] = useState('')
  const [rights, setRights] = useState<PostRight[]>([])
  const onError = (error: unknown) => toast.error(errorMessage(i18n, error))
  const ownRights: AdminPost['rights'] =
    knownRights ??
    (me?.role === 'admin' || (editors.data && me?.id === editors.data.owner.id)
      ? [...postRights]
      : (editors.data?.editors.find((editor) => editor.user.id === me?.id)?.rights ?? []))
  const canManage = ownRights.includes('editors')
  // What it may give: the rights it has
  const giveable = postRights.filter((right) => ownRights.includes(right))
  const mayChange = (held: PostRight[]) => canManage && held.every((right) => ownRights.includes(right))

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>
        <Button type="button" variant="ghost" size={size}>
          <UsersIcon />
          {t('editors.title')}
        </Button>
      </DialogTrigger>
      <DialogContent className="max-h-[85vh] overflow-y-auto sm:max-w-2xl">
        <DialogHeader>
          <DialogTitle>{t('editors.title')}</DialogTitle>
          <DialogDescription>{t('editors.hint')}</DialogDescription>
        </DialogHeader>
        {editors.isPending && <Skeleton className="h-32 w-full" />}
        {editors.data && (
          <div className="grid gap-4">
            <div className="flex items-center gap-3">
              <Person person={editors.data.owner} />
              <span className="text-xs text-muted-foreground">{t('editors.owner')}</span>
            </div>
            {me?.role === 'admin' && <Transfer postId={postId} />}

            <ul className="grid gap-2">
              {editors.data.editors.length === 0 && <li className="text-sm text-muted-foreground">{t('editors.none')}</li>}
              {editors.data.editors.map((editor) => (
                <li key={editor.user.id} className="flex flex-wrap items-center gap-2 rounded-lg border p-3">
                  <Person person={editor.user} />
                  <div className="ml-auto flex flex-wrap items-center gap-1">
                    <span className="text-xs text-muted-foreground">{t('editors.text')}</span>
                    {postRights.map((right) => {
                      const has = editor.rights.includes(right)
                      return (
                        <Button
                          key={right}
                          type="button"
                          size="xs"
                          variant={has ? 'default' : 'outline'}
                          aria-pressed={has}
                          disabled={!mayChange(editor.rights) || !ownRights.includes(right) || set.isPending}
                          onClick={() =>
                            set.mutate(
                              {
                                username: editor.user.username,
                                rights: has ? editor.rights.filter((r) => r !== right) : [...editor.rights, right],
                              },
                              { onError },
                            )
                          }
                        >
                          {t(`editors.rights.${right}`)}
                        </Button>
                      )
                    })}
                    {(mayChange(editor.rights) || editor.user.id === me?.id) && (
                      <Button
                        type="button"
                        size="icon-xs"
                        variant="ghost"
                        aria-label={editor.user.id === me?.id ? t('editors.leave') : t('editors.remove', { name: editor.user.displayName })}
                        onClick={() =>
                          remove.mutate(editor.user.id, {
                            onSuccess: () => {
                              if (editor.user.id === me?.id) setOpen(false)
                            },
                            onError,
                          })
                        }
                      >
                        <XIcon />
                      </Button>
                    )}
                  </div>
                </li>
              ))}
            </ul>

            {canManage && (
              <form
                className="grid gap-2 rounded-lg border p-3"
                onSubmit={(event) => {
                  event.preventDefault()
                  if (!username.trim()) return
                  set.mutate(
                    { username: username.trim(), rights },
                    {
                      onSuccess: () => {
                        toast.success(t('editors.invited', { name: username.trim() }))
                        setUsername('')
                        setRights([])
                      },
                      onError,
                    },
                  )
                }}
              >
                <label htmlFor="invite-editor" className="text-sm font-medium">
                  {t('editors.invite')}
                </label>
                <div className="flex flex-wrap items-center gap-2">
                  <Input
                    id="invite-editor"
                    className="w-48"
                    placeholder={t('editors.username')}
                    value={username}
                    onChange={(event) => setUsername(event.target.value)}
                  />
                  {giveable.map((right) => {
                    const has = rights.includes(right)
                    return (
                      <Button
                        key={right}
                        type="button"
                        size="xs"
                        variant={has ? 'default' : 'outline'}
                        aria-pressed={has}
                        onClick={() => setRights(has ? rights.filter((r) => r !== right) : [...rights, right])}
                      >
                        {t(`editors.rights.${right}`)}
                      </Button>
                    )
                  })}
                  <Button type="submit" size="sm" className="ml-auto" disabled={set.isPending}>
                    {t('editors.inviteButton')}
                  </Button>
                </div>
              </form>
            )}
          </div>
        )}
      </DialogContent>
    </Dialog>
  )
}

function Person({ person }: { person: PostPerson }) {
  return (
    <span className="flex items-center gap-2 text-sm">
      <UserAvatar user={person} className="size-7 text-xs" />
      <span className="font-medium">{person.displayName}</span>
      <span className="text-muted-foreground">@{person.username}</span>
    </span>
  )
}

// The admin's: the post to another account; its owner before edits it, with every right
function Transfer({ postId }: { postId: number }) {
  const { t, i18n } = useTranslation()
  const transfer = useTransferPost(postId)
  const [owner, setOwner] = useState('')
  return (
    <form
      className="flex flex-wrap items-center gap-2"
      onSubmit={(event) => {
        event.preventDefault()
        if (!owner.trim()) return
        transfer.mutate(owner.trim(), {
          onSuccess: () => {
            toast.success(t('editors.transferred', { name: owner.trim() }))
            setOwner('')
          },
          onError: (error) => toast.error(errorMessage(i18n, error)),
        })
      }}
    >
      <Input
        className="w-48"
        aria-label={t('editors.newOwner')}
        placeholder={t('editors.newOwner')}
        value={owner}
        onChange={(event) => setOwner(event.target.value)}
      />
      <Button type="submit" size="sm" variant="outline" disabled={transfer.isPending}>
        {t('editors.transfer')}
      </Button>
    </form>
  )
}
