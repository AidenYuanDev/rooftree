import { FileTextIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useNavigate } from 'react-router'
import { FormError } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogHeader, DialogTitle, DialogTrigger } from '~/components/ui/dialog'
import { Skeleton } from '~/components/ui/skeleton'
import { localePath } from '~/lib/lang'
import { useAdminPosts, useMe } from '~/lib/queries'
import type { Lang } from '~/lib/schemas'

// A writer's or invited editor's drafts and shared posts, opened right from /posts onto the paper
export function PostDraftsDialog({ lang, onOpenDraft }: { lang: Lang; onOpenDraft: (id: number) => void }) {
  const { t } = useTranslation()
  const navigate = useNavigate()
  const [open, setOpen] = useState(false)
  const [page, setPage] = useState(1)
  const admin = useMe().data?.user?.role === 'admin'
  const posts = useAdminPosts('', page, open)

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>
        <Button type="button" variant="outline" size="sm">
          <FileTextIcon />
          {t('collab.drafts')}
        </Button>
      </DialogTrigger>
      <DialogContent className="max-h-[85vh] overflow-y-auto sm:max-w-xl">
        <DialogHeader>
          <DialogTitle>{t('collab.drafts')}</DialogTitle>
        </DialogHeader>
        <FormError error={posts.error} />
        {posts.isPending && <Skeleton className="h-32 w-full" />}
        {posts.data && posts.data.posts.length === 0 && (
          <p className="py-6 text-center text-sm text-muted-foreground">{t('admin.noPosts')}</p>
        )}
        {posts.data && posts.data.posts.length > 0 && (
          <ul className="divide-y rounded-lg border">
            {posts.data.posts.map((post) => (
              <li key={post.id}>
                <button
                  type="button"
                  className="flex w-full flex-wrap items-center gap-2 p-3 text-left transition hover:bg-muted/50"
                  onClick={() => {
                    setOpen(false)
                    if (post.status === 'published') void navigate(localePath(lang, `/posts/${post.slug}?edit=1`))
                    else onOpenDraft(post.id)
                  }}
                >
                  <div className="min-w-0 flex-1">
                    <div className="flex flex-wrap items-center gap-2">
                      <span className="truncate font-medium">{post.title}</span>
                      <Badge variant={post.status === 'published' ? 'default' : 'outline'}>
                        {post.status === 'published' ? t('admin.statusPublished') : t('admin.statusDraft')}
                      </Badge>
                      {post.reviewRequested && <Badge variant="secondary">{t('admin.statusReview')}</Badge>}
                      {!admin && !post.mine && <Badge variant="outline">{t('admin.editing')}</Badge>}
                    </div>
                    <div className="mt-0.5 flex gap-3 text-xs text-muted-foreground">
                      <span className="font-mono">/{post.slug}</span>
                      <span>
                        {t('admin.updatedAt')} <LocalDate iso={post.updatedAt} lang={lang} />
                      </span>
                    </div>
                  </div>
                </button>
              </li>
            ))}
          </ul>
        )}
        {posts.data && (page > 1 || posts.data.hasMore) && (
          <div className="flex justify-center gap-2">
            <Button type="button" variant="outline" size="sm" disabled={page <= 1} onClick={() => setPage(page - 1)}>
              ‹
            </Button>
            <span className="px-2 py-1 text-sm">{page}</span>
            <Button type="button" variant="outline" size="sm" disabled={!posts.data.hasMore} onClick={() => setPage(page + 1)}>
              ›
            </Button>
          </div>
        )}
      </DialogContent>
    </Dialog>
  )
}
