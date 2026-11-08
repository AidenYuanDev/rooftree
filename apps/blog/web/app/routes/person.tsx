import { CheckIcon, ClockIcon, MessageSquareIcon, MoreHorizontalIcon, UserMinusIcon, UserPlusIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import type { Route } from './+types/person'
import { NotFoundPage } from './not-found'
import { errorMessage } from '~/i18n'
import { FormError } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import { PostCard } from '~/components/post-card'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '~/components/ui/alert-dialog'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { Input } from '~/components/ui/input'
import { Skeleton } from '~/components/ui/skeleton'
import { UserAvatar } from '~/components/user-avatar'
import { ApiError } from '~/lib/api'
import { useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import {
  useAcceptFriendRequest,
  useMe,
  usePerson,
  useRejectFriendRequest,
  useRemoveFriend,
  useSendFriendRequest,
  useUserFriends,
  useUserFriendshipStatus,
} from '~/lib/queries'
import { useChatStore } from '~/stores/chat'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'person.title')

function FriendshipAction({ username, displayName }: { username: string; displayName: string }) {
  const { t, i18n } = useTranslation()
  const me = useMe()
  const currentUser = me.data?.user
  const enabled = !!currentUser && currentUser.username !== username
  const statusQuery = useUserFriendshipStatus(username, enabled)
  const friendsQuery = useUserFriends(enabled && statusQuery.data?.status === 'friends')

  const sendRequest = useSendFriendRequest()
  const acceptRequest = useAcceptFriendRequest()
  const rejectRequest = useRejectFriendRequest()
  const removeFriend = useRemoveFriend()

  const [dialogOpen, setDialogOpen] = useState(false)
  const [unfriendOpen, setUnfriendOpen] = useState(false)
  const [message, setMessage] = useState('')

  if (!currentUser || currentUser.username === username) return null
  if (statusQuery.isPending) return <Skeleton className="h-8 w-24" />

  const status = statusQuery.data?.status
  const requestId = statusQuery.data?.requestId

  const onSend = () => {
    sendRequest.mutate(
      { to: username, message },
      {
        onSuccess: () => {
          toast.success(t('userFriends.requestSentSuccess'))
          setDialogOpen(false)
          setMessage('')
        },
        onError: (error) => toast.error(errorMessage(i18n, error)),
      },
    )
  }

  const onAccept = () => {
    if (!requestId) return
    acceptRequest.mutate(requestId, {
      onSuccess: () => toast.success(t('userFriends.requestAccepted')),
    })
  }

  const onCancelOrReject = () => {
    if (!requestId) return
    rejectRequest.mutate(requestId, {
      onSuccess: () => toast.success(status === 'pending_sent' ? t('userFriends.requestCanceled') : t('userFriends.requestRejected')),
    })
  }

  const onUnfriend = () => {
    const friend = friendsQuery.data?.friends.find((f) => f.username === username)
    if (!friend) return
    removeFriend.mutate(friend.id, {
      onSuccess: () => {
        toast.success(t('userFriends.unfriended'))
        setUnfriendOpen(false)
      },
    })
  }

  if (status === 'friends') {
    const friend = friendsQuery.data?.friends.find((f) => f.username === username)
    return (
      <div className="flex items-center gap-2">
        <Button
          size="sm"
          variant="outline"
          onClick={() => friend && useChatStore.getState().openDirectChat(friend.id, currentUser.id)}
          className="gap-1.5 text-xs"
        >
          <MessageSquareIcon className="size-3.5 text-primary" />
          {t('chat.sendMessage')}
        </Button>
        <div className="inline-flex items-center gap-1.5 rounded-full border border-emerald-500/20 bg-emerald-500/10 px-3 py-1 text-xs font-medium text-emerald-600 dark:text-emerald-400">
          <span className={`size-1.5 rounded-full ${friend?.online ? 'animate-pulse bg-emerald-500' : 'bg-muted-foreground'}`} />
          {t('userFriends.isFriend')}
        </div>
        <DropdownMenu>
          <DropdownMenuTrigger asChild>
            <Button variant="ghost" size="icon" className="size-8">
              <MoreHorizontalIcon className="size-4" />
            </Button>
          </DropdownMenuTrigger>
          <DropdownMenuContent align="end">
            <DropdownMenuItem onSelect={() => setUnfriendOpen(true)} className="text-destructive">
              <UserMinusIcon className="mr-2 size-4" />
              {t('userFriends.unfriend')}
            </DropdownMenuItem>
          </DropdownMenuContent>
        </DropdownMenu>

        <AlertDialog open={unfriendOpen} onOpenChange={setUnfriendOpen}>
          <AlertDialogContent>
            <AlertDialogHeader>
              <AlertDialogTitle>{t('userFriends.unfriend')}</AlertDialogTitle>
              <AlertDialogDescription>{t('userFriends.unfriendConfirm', { name: displayName })}</AlertDialogDescription>
            </AlertDialogHeader>
            <AlertDialogFooter>
              <AlertDialogCancel>{t('userFriends.cancel')}</AlertDialogCancel>
              <AlertDialogAction onClick={onUnfriend} className="text-destructive-foreground bg-destructive hover:bg-destructive/90">
                {t('userFriends.unfriend')}
              </AlertDialogAction>
            </AlertDialogFooter>
          </AlertDialogContent>
        </AlertDialog>
      </div>
    )
  }

  if (status === 'pending_sent') {
    return (
      <Button
        variant="outline"
        size="sm"
        disabled={rejectRequest.isPending}
        onClick={onCancelOrReject}
        className="gap-1.5 text-xs text-muted-foreground"
      >
        <ClockIcon className="size-3.5" />
        {t('userFriends.cancel')}
      </Button>
    )
  }

  if (status === 'pending_received') {
    return (
      <div className="flex items-center gap-2">
        <Button size="sm" disabled={acceptRequest.isPending} onClick={onAccept} className="gap-1.5 text-xs">
          <CheckIcon className="size-3.5" />
          {t('userFriends.accept')}
        </Button>
        <Button variant="outline" size="sm" disabled={rejectRequest.isPending} onClick={onCancelOrReject} className="text-xs">
          {t('userFriends.reject')}
        </Button>
      </div>
    )
  }

  return (
    <>
      <Button size="sm" onClick={() => setDialogOpen(true)} className="gap-1.5 text-xs">
        <UserPlusIcon className="size-3.5" />
        {t('userFriends.addFriend')}
      </Button>

      <Dialog open={dialogOpen} onOpenChange={setDialogOpen}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>{t('userFriends.addFriend')}</DialogTitle>
            <DialogDescription>
              @{username} ({displayName})
            </DialogDescription>
          </DialogHeader>
          <Input
            value={message}
            onChange={(e) => setMessage(e.target.value)}
            placeholder={t('userFriends.messagePlaceholder')}
            maxLength={200}
          />
          <DialogFooter>
            <Button variant="outline" onClick={() => setDialogOpen(false)}>
              {t('userFriends.cancel')}
            </Button>
            <Button onClick={onSend} disabled={sendRequest.isPending}>
              {t('userFriends.addFriend')}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  )
}

// Someone's page, where a post's people link to: who they are and the posts they wrote (owned or
// edited), in the page's language
export default function PersonPage({ params }: Route.ComponentProps) {
  const { t } = useTranslation()
  const lang = useLang()
  const person = usePerson(params.username, lang)

  if (person.isPending) return <Skeleton className="h-64 w-full" />
  if (person.error instanceof ApiError && person.error.status === 404) return <NotFoundPage />
  if (!person.data) return <FormError error={person.error} />

  const { person: who, since, posts } = person.data
  return (
    <div className="mx-auto grid w-full max-w-3xl gap-8">
      <header className="flex flex-wrap items-center justify-between gap-4">
        <div className="flex items-center gap-4">
          <UserAvatar user={who} className="size-16 text-xl" />
          <div>
            <h1 className="font-heading text-2xl font-bold">{who.displayName}</h1>
            <p className="text-sm text-muted-foreground">
              @{who.username} · {t('person.since')} <LocalDate iso={since} lang={lang} />
            </p>
          </div>
        </div>
        <FriendshipAction username={who.username} displayName={who.displayName} />
      </header>
      <section className="grid gap-4">
        <h2 className="text-lg font-semibold">{t('person.posts', { count: posts.length })}</h2>
        {posts.length === 0 ? (
          <p className="text-muted-foreground">{t('person.noPosts')}</p>
        ) : (
          posts.map((post) => <PostCard key={post.id} post={post} />)
        )}
      </section>
    </div>
  )
}
