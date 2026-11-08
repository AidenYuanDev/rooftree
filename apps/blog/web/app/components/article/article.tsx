import { ChevronLeftIcon, ChevronRightIcon } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { ImageViewer } from '~/components/article/image-viewer'
import { ReadingProgress, TableOfContents } from '~/components/article/toc'
import { StarLoader } from '~/components/scenery/mark'
import { Comments } from '~/components/comments'
import { PostOwnerContext } from '~/lib/post-owner'
import { Passages } from '~/components/passages'
import { ReadingNow } from '~/components/live'
import { Reactions } from '~/components/reactions'
import type { CommentTarget, Lang } from '~/lib/schemas'
import { useHeaderTitle } from '~/stores/header'

// Writing newer than the build has no HTML from it: its Markdown as HTML here, by a module loaded when
// needed. Shown the same way as the build's, without its code highlighting
function useHtml(html: string | null, markdown: string): string | null {
  const [made, setMade] = useState<{ markdown: string; html: string } | null>(null)
  useEffect(() => {
    if (html) return
    let current = true
    void import('~/lib/markdown-html').then((module) => {
      if (current) setMade({ markdown, html: module.markdownHtml(markdown) })
    })
    return () => {
      current = false
    }
  }, [html, markdown])
  return html ?? (made?.markdown === markdown ? made.html : null)
}

export interface Neighbour {
  title: string
  to: string
}

// A post to read: on paper over the painting, its table of contents and reading progress beside
// it, older and newer below, then the comments. html is the build's (highlighted, image placeholders);
// without it the markdown is rendered here
export function Article({
  title,
  lang,
  header,
  cover,
  html,
  markdown,
  previous,
  next,
  comments,
  editor,
}: {
  title: string
  lang: Lang
  header: React.ReactNode
  cover?: string
  html: string | null
  markdown: string
  previous?: Neighbour | null
  next?: Neighbour | null
  // passages: comments on passages of its text (a post's)
  // ownerId: the post's owner, who moderates its comments
  comments: { target: CommentTarget; enabled: boolean; passages?: boolean; ownerId?: number }
  // When editing in place on the paper itself: replaces the article while keeping its width and comments
  editor?: React.ReactNode
}) {
  const { t } = useTranslation()
  const body = useRef<HTMLDivElement>(null)
  const shown = useHtml(html, markdown)
  useHeaderTitle(title)

  return (
    <PostOwnerContext.Provider value={comments.ownerId ?? null}>
      <div className="grid justify-center gap-10 xl:grid-cols-[minmax(0,48rem)_14rem]">
        {!editor && <ReadingProgress container={body} />}
        <div className="min-w-0">
          {editor ? (
            <div className="rounded-3xl border bg-paper p-6 shadow-sm backdrop-blur-md md:p-10">{editor}</div>
          ) : (
            <>
              <article className="rise-in rounded-3xl border bg-paper p-6 shadow-sm backdrop-blur-md md:p-10">
                <header className="mb-8">
                  {header}
                  <ReadingNow className="mt-3 block text-sm text-muted-foreground" />
                </header>
                {cover && <img src={cover} alt="" className="mb-8 w-full rounded-xl border object-cover" />}
                <div ref={body}>
                  {shown ? (
                    <div className="prose max-w-none prose-night" lang={lang} dangerouslySetInnerHTML={{ __html: shown }} />
                  ) : (
                    <StarLoader className="py-24" />
                  )}
                </div>
                {comments.passages && shown && (
                  <Passages container={body} target={comments.target} lang={lang} enabled={comments.enabled} contentKey={shown} />
                )}
                <ImageViewer container={body} contentKey={shown ?? markdown} />
                <Reactions target={comments.target} className="mt-10 border-t pt-6" />
              </article>

              {(previous || next) && (
                <nav aria-label={t('posts.neighbours')} className="mt-6 grid gap-3 sm:grid-cols-2">
                  {previous ? (
                    <Link
                      to={previous.to}
                      className="group rounded-2xl border bg-card/60 p-4 backdrop-blur-md transition hover:border-primary/40"
                    >
                      <span className="flex items-center gap-1 text-xs text-muted-foreground">
                        <ChevronLeftIcon className="size-3.5" />
                        {t('posts.previous')}
                      </span>
                      <span className="mt-1 block truncate font-heading font-bold group-hover:text-primary">{previous.title}</span>
                    </Link>
                  ) : (
                    <span />
                  )}
                  {next && (
                    <Link
                      to={next.to}
                      className="group rounded-2xl border bg-card/60 p-4 text-right backdrop-blur-md transition hover:border-primary/40"
                    >
                      <span className="flex items-center justify-end gap-1 text-xs text-muted-foreground">
                        {t('posts.next')}
                        <ChevronRightIcon className="size-3.5" />
                      </span>
                      <span className="mt-1 block truncate font-heading font-bold group-hover:text-primary">{next.title}</span>
                    </Link>
                  )}
                </nav>
              )}
            </>
          )}

          <div className="mt-8 rounded-3xl border bg-paper p-6 backdrop-blur-md md:p-10">
            <Comments target={comments.target} enabled={comments.enabled} />
          </div>
        </div>
        <aside>{!editor && <TableOfContents container={body} contentKey={shown ?? markdown} />}</aside>
      </div>
    </PostOwnerContext.Provider>
  )
}
