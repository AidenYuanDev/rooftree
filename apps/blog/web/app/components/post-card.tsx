import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { LocalDate } from '~/components/local-date'
import { Badge } from '~/components/ui/badge'
import { localePath, useLang } from '~/lib/lang'
import type { Lang, PostSummary } from '~/lib/schemas'

// linkLang: the language of the page it opens, the page's own unless said
export function PostCard({ post, linkLang }: { post: PostSummary; linkLang?: Lang }) {
  const { t } = useTranslation()
  const lang = useLang()
  return (
    <article className="group relative rounded-2xl border bg-card/60 p-5 shadow-sm backdrop-blur-md transition hover:-translate-y-0.5 hover:border-primary/40 hover:shadow-md">
      <h2 className="font-heading text-xl font-bold">
        <Link
          to={localePath(linkLang ?? lang, `/posts/${post.slug}`)}
          className="group-hover:text-primary after:absolute after:inset-0"
          lang={post.lang}
        >
          {post.title}
        </Link>
      </h2>
      <div className="mt-1 flex flex-wrap items-center gap-x-3 gap-y-1 text-sm text-muted-foreground">
        <LocalDate iso={post.publishedAt} lang={lang} />
        {post.commentCount > 0 && <span>{t('posts.comments', { count: post.commentCount })}</span>}
        {post.fallback && <Badge variant="outline">{t(`langs.${post.lang}`)}</Badge>}
      </div>
      {post.summary && (
        <p className="mt-2 text-muted-foreground" lang={post.lang}>
          {post.summary}
        </p>
      )}
      {post.tags.length > 0 && (
        <div className="relative mt-3 flex flex-wrap gap-1.5">
          {post.tags.map((tag) => (
            <Badge key={tag.slug} variant="secondary" asChild>
              <Link to={localePath(lang, `/tags/${tag.slug}`)}>{tag.name}</Link>
            </Badge>
          ))}
        </div>
      )}
    </article>
  )
}
