import { useInfiniteQuery } from '@tanstack/react-query'
import { useTranslation } from 'react-i18next'
import { PostCard } from '~/components/post-card'
import { Button } from '~/components/ui/button'
import { errorMessage } from '~/i18n'
import { fetchPostList } from '~/lib/content'
import { useLang } from '~/lib/lang'
import type { PostList } from '~/lib/schemas'

// The first page comes from the loader (prerendered); the next ones from the API in the browser
export function PostFeed({ firstPage, tag }: { firstPage: PostList; tag?: string }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const feed = useInfiniteQuery({
    queryKey: ['posts', lang, tag ?? ''],
    queryFn: ({ pageParam }) => fetchPostList(lang, { tag, cursor: pageParam }),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.nextCursor,
    initialData: { pages: [firstPage], pageParams: [null] },
    staleTime: Infinity,
  })

  const posts = feed.data.pages.flatMap((page) => page.posts)
  if (posts.length === 0) return <p className="py-12 text-center text-muted-foreground">{t('posts.empty')}</p>

  return (
    <div>
      <div className="space-y-4">
        {posts.map((post, i) => (
          <div key={post.id} className="rise-in" style={{ '--rise-index': Math.min(i, 8) } as React.CSSProperties}>
            <PostCard post={post} />
          </div>
        ))}
      </div>
      {feed.hasNextPage && (
        <div className="mt-6 flex justify-center">
          <Button variant="outline" onClick={() => void feed.fetchNextPage()} disabled={feed.isFetchingNextPage}>
            {feed.isFetchingNextPage ? t('posts.loading') : t('posts.loadMore')}
          </Button>
        </div>
      )}
      {feed.isFetchNextPageError && <p className="mt-4 text-center text-sm text-destructive">{errorMessage(i18n, feed.error)}</p>}
    </div>
  )
}

export function PostFeedSkeleton() {
  return (
    <div className="space-y-8">
      {[0, 1, 2].map((i) => (
        <div key={i} className="space-y-2">
          <div className="h-6 w-2/3 animate-pulse rounded bg-muted" />
          <div className="h-4 w-1/3 animate-pulse rounded bg-muted" />
          <div className="h-4 w-full animate-pulse rounded bg-muted" />
        </div>
      ))}
    </div>
  )
}
