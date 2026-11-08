import { useTranslation } from 'react-i18next'
import { Link, useSearchParams } from 'react-router'
import { toast } from 'sonner'
import { BanDialog } from '~/components/admin/ban-dialog'
import { FormError } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Skeleton } from '~/components/ui/skeleton'
import { Tabs, TabsList, TabsTrigger } from '~/components/ui/tabs'
import { errorMessage } from '~/i18n'
import { targetKind, targetPath } from '~/lib/targets'
import { localePath, useLang } from '~/lib/lang'
import { useAdminComments, useModerateComment } from '~/lib/queries'

export default function AdminComments() {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const [searchParams, setSearchParams] = useSearchParams()
  const status = searchParams.get('status') ?? ''
  const passage = (['any', 'orphaned'] as const).find((value) => value === searchParams.get('passage')) ?? ''
  const comments = useAdminComments(status, passage)
  const setFilter = (key: string, value: string) =>
    setSearchParams((params) => {
      if (value) params.set(key, value)
      else params.delete(key)
      return params
    })
  const moderate = useModerateComment()
  const list = comments.data?.pages.flatMap((page) => page.comments) ?? []

  return (
    <div className="grid gap-4">
      <div className="flex flex-wrap gap-2">
        <Tabs value={status || 'all'} onValueChange={(value) => setFilter('status', value === 'all' ? '' : value)}>
          <TabsList>
            <TabsTrigger value="all">{t('admin.allStatuses')}</TabsTrigger>
            <TabsTrigger value="visible">{t('admin.statusVisible')}</TabsTrigger>
            <TabsTrigger value="hidden">{t('admin.statusHidden')}</TabsTrigger>
            <TabsTrigger value="deleted">{t('admin.statusDeleted')}</TabsTrigger>
          </TabsList>
        </Tabs>
        <Tabs value={passage || 'all'} onValueChange={(value) => setFilter('passage', value === 'all' ? '' : value)}>
          <TabsList>
            <TabsTrigger value="all">{t('admin.passageAll')}</TabsTrigger>
            <TabsTrigger value="any">{t('admin.passageAny')}</TabsTrigger>
            <TabsTrigger value="orphaned">{t('admin.passageOrphaned')}</TabsTrigger>
          </TabsList>
        </Tabs>
      </div>

      <FormError error={comments.error} />
      {comments.isPending && <Skeleton className="h-40 w-full" />}
      {comments.isSuccess && list.length === 0 && <p className="py-12 text-center text-muted-foreground">{t('admin.noComments')}</p>}
      <ul className="divide-y rounded-lg border">
        {list.map((comment) => (
          <li key={comment.id} className="grid gap-1 p-3">
            <div className="flex flex-wrap items-center gap-2 text-sm">
              <span className="font-medium">{comment.author.displayName}</span>
              <span className="text-muted-foreground">@{comment.author.username}</span>
              <span className="font-mono text-xs text-muted-foreground">{comment.ip}</span>
              {comment.author.banned && <Badge variant="destructive">{t('admin.ban')}</Badge>}
              {comment.status !== 'visible' && (
                <Badge variant="outline">{comment.status === 'hidden' ? t('admin.statusHidden') : t('admin.statusDeleted')}</Badge>
              )}
              {comment.isReply && <span className="text-xs text-muted-foreground">{t('admin.reply')}</span>}
              <span className="ml-auto text-xs text-muted-foreground">
                <LocalDate iso={comment.createdAt} lang={lang} />
              </span>
            </div>
            {comment.passage && (
              <div className="border-l-2 border-primary/40 pl-2 text-xs text-muted-foreground">
                <span className="italic">{comment.passage.quote}</span>
                {comment.passage.status !== 'ok' && (
                  <Badge variant="outline" className="ml-2">
                    {comment.passage.status === 'kept' ? t('admin.passageKept') : t('admin.passageOrphaned')}
                  </Badge>
                )}
                {/* What it was on, now that the post lost it */}
                {comment.passage.status !== 'ok' && comment.passage.text !== comment.passage.quote && (
                  <p className="mt-1 line-clamp-3">{comment.passage.text}</p>
                )}
              </div>
            )}
            <p className="text-sm break-words whitespace-pre-wrap">{comment.body}</p>
            <div className="flex flex-wrap items-center gap-1 text-muted-foreground">
              <Button variant="ghost" size="xs" asChild>
                <Link to={localePath(lang, targetPath(comment.target, comment.postSlug))}>
                  {t(`admin.on.${targetKind(comment.target)}`)}
                  {comment.postSlug ? `: ${comment.postSlug}` : comment.target === 'guestbook' ? '' : ` #${comment.target.split(':')[1]}`}
                </Link>
              </Button>
              {comment.status !== 'deleted' && (
                <Button
                  variant="ghost"
                  size="xs"
                  onClick={() =>
                    moderate.mutate(
                      { id: comment.id, status: comment.status === 'hidden' ? 'visible' : 'hidden' },
                      { onError: (error) => toast.error(errorMessage(i18n, error)) },
                    )
                  }
                >
                  {comment.status === 'hidden' ? t('comments.show') : t('comments.hide')}
                </Button>
              )}
              {comment.passage && comment.postSlug && comment.status !== 'deleted' && (
                <Button variant="ghost" size="xs" asChild>
                  <Link to={`${localePath(comment.passage.lang, `/posts/${comment.postSlug}`)}?pick=${comment.id}`}>
                    {t('admin.repickPassage')}
                  </Link>
                </Button>
              )}
              {comment.passage?.status === 'orphaned' && (
                <Button
                  variant="ghost"
                  size="xs"
                  onClick={() =>
                    moderate.mutate(
                      { id: comment.id, passage: 'kept' },
                      {
                        onSuccess: () => toast.success(t('admin.passageKeptToast')),
                        onError: (error) => toast.error(errorMessage(i18n, error)),
                      },
                    )
                  }
                >
                  {t('admin.keepPassage')}
                </Button>
              )}
              {!comment.author.banned && <BanDialog userId={comment.author.id} name={comment.author.displayName} />}
            </div>
          </li>
        ))}
      </ul>
      {comments.hasNextPage && (
        <div className="flex justify-center">
          <Button variant="outline" onClick={() => void comments.fetchNextPage()} disabled={comments.isFetchingNextPage}>
            {t('posts.loadMore')}
          </Button>
        </div>
      )}
    </div>
  )
}
