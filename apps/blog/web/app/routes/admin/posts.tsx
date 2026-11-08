import { PencilIcon, PlusIcon, Trash2Icon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link, useSearchParams } from 'react-router'
import { toast } from 'sonner'
import { FormError } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogTrigger,
} from '~/components/ui/alert-dialog'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Skeleton } from '~/components/ui/skeleton'
import { Tabs, TabsList, TabsTrigger } from '~/components/ui/tabs'
import { errorMessage } from '~/i18n'
import { formatDate as formatDateTime } from '~/lib/drive'
import { localePath, useLang, usePostsBase } from '~/lib/lang'
import { useAdminPosts, useDeletePost, useMe } from '~/lib/queries'

export default function AdminPosts() {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const [searchParams, setSearchParams] = useSearchParams()
  const status = searchParams.get('status') ?? ''
  const page = Math.max(1, Number(searchParams.get('page') ?? 1) || 1)
  const posts = useAdminPosts(status, page)
  const remove = useDeletePost()
  const base = usePostsBase()
  const user = useMe().data?.user
  const admin = user?.role === 'admin'

  return (
    <div className="grid gap-4">
      <div className="flex flex-wrap items-center gap-3">
        <Tabs value={status || 'all'} onValueChange={(value) => setSearchParams(value === 'all' ? {} : { status: value })}>
          <TabsList>
            <TabsTrigger value="all">{t('admin.all')}</TabsTrigger>
            <TabsTrigger value="published">{t('admin.statusPublished')}</TabsTrigger>
            <TabsTrigger value="draft">{t('admin.statusDraft')}</TabsTrigger>
            <TabsTrigger value="review">{t('admin.statusReview')}</TabsTrigger>
          </TabsList>
        </Tabs>
        {user?.write && (
          <Button asChild className="ml-auto">
            <Link to={localePath(lang, `${base}/new`)}>
              <PlusIcon />
              {t('admin.newPost')}
            </Link>
          </Button>
        )}
      </div>

      <FormError error={posts.error} />
      {posts.isPending && <Skeleton className="h-40 w-full" />}
      {posts.data && posts.data.posts.length === 0 && <p className="py-12 text-center text-muted-foreground">{t('admin.noPosts')}</p>}
      {posts.data && posts.data.posts.length > 0 && (
        <ul className="divide-y rounded-lg border">
          {posts.data.posts.map((post) => (
            <li key={post.id} className="flex flex-wrap items-center gap-3 p-3">
              <div className="min-w-0 flex-1">
                <div className="flex items-center gap-2">
                  <Link to={localePath(lang, `${base}/${post.id}`)} className="truncate font-medium hover:underline">
                    {post.title}
                  </Link>
                  <Badge variant={post.status === 'published' ? 'default' : 'outline'}>
                    {post.status === 'published'
                      ? t('admin.statusPublished')
                      : post.publishAt
                        ? t('admin.scheduledFor', { date: formatDateTime(post.publishAt, lang) })
                        : t('admin.statusDraft')}
                  </Badge>
                  {post.reviewRequested && <Badge variant="secondary">{t('admin.statusReview')}</Badge>}
                  {!admin && !post.mine && <Badge variant="outline">{t('admin.editing')}</Badge>}
                </div>
                <div className="mt-0.5 flex gap-3 text-xs text-muted-foreground">
                  <span className="font-mono">/{post.slug}</span>
                  <span>{post.translations.map((l) => t(`langs.${l}`)).join(' · ')}</span>
                  <span>
                    {t('admin.updatedAt')} <LocalDate iso={post.updatedAt} lang={lang} />
                  </span>
                </div>
              </div>
              {post.status === 'published' && (
                <Button variant="ghost" size="sm" asChild>
                  <Link to={localePath(lang, `/posts/${post.slug}`)}>{t('admin.view')}</Link>
                </Button>
              )}
              <Button variant="ghost" size="icon-sm" asChild aria-label={t('admin.edit')}>
                <Link to={localePath(lang, `${base}/${post.id}`)}>
                  <PencilIcon />
                </Link>
              </Button>
              {(admin || post.mine) && (
                <AlertDialog>
                  <AlertDialogTrigger asChild>
                    <Button variant="ghost" size="icon-sm" aria-label={t('admin.delete')}>
                      <Trash2Icon />
                    </Button>
                  </AlertDialogTrigger>
                  <AlertDialogContent>
                    <AlertDialogHeader>
                      <AlertDialogTitle>{t('admin.delete')}</AlertDialogTitle>
                      <AlertDialogDescription>{t('admin.deleteConfirm', { title: post.title })}</AlertDialogDescription>
                    </AlertDialogHeader>
                    <AlertDialogFooter>
                      <AlertDialogCancel>{t('admin.cancel')}</AlertDialogCancel>
                      <AlertDialogAction
                        variant="destructive"
                        onClick={() => remove.mutate(post.id, { onError: (error) => toast.error(errorMessage(i18n, error)) })}
                      >
                        {t('admin.delete')}
                      </AlertDialogAction>
                    </AlertDialogFooter>
                  </AlertDialogContent>
                </AlertDialog>
              )}
            </li>
          ))}
        </ul>
      )}

      {posts.data && (page > 1 || posts.data.hasMore) && (
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
            disabled={!posts.data.hasMore}
            onClick={() => setSearchParams({ ...(status ? { status } : {}), page: String(page + 1) })}
          >
            ›
          </Button>
        </div>
      )}
    </div>
  )
}
