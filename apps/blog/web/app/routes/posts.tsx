import { PlusIcon } from 'lucide-react'
import { lazy, Suspense } from 'react'
import { useTranslation } from 'react-i18next'
import { useSearchParams } from 'react-router'
import type { Route } from './+types/posts'
import { PostDraftsDialog } from '~/components/editor/post-drafts-dialog'
import { PageHeader } from '~/components/page-header'
import { PostFeed } from '~/components/post-feed'
import { TagList } from '~/components/tag-list'
import { Button } from '~/components/ui/button'
import { Skeleton } from '~/components/ui/skeleton'
import { getI18n } from '~/i18n'
import { fetchPostList, fetchSiteCached, fetchTags } from '~/lib/content'
import { pageMeta } from '~/lib/meta'
import { useCurrentUser } from '~/lib/queries'
import { requireLang } from '~/lib/route'
import type { Lang } from '~/lib/schemas'

const PostNewPaper = lazy(() => import('~/components/editor/post-new-paper').then((module) => ({ default: module.PostNewPaper })))
const PostInlineEditor = lazy(() =>
  import('~/components/editor/post-inline-editor').then((module) => ({ default: module.PostInlineEditor })),
)

async function load(lang: string | undefined) {
  const valid = requireLang(lang)
  const [firstPage, tags, site] = await Promise.all([fetchPostList(valid), fetchTags(valid), fetchSiteCached(valid)])
  return { firstPage, tags, siteName: site.name }
}

export async function loader({ params }: Route.LoaderArgs) {
  return load(params.lang)
}

export async function clientLoader({ params, serverLoader }: Route.ClientLoaderArgs) {
  try {
    return await serverLoader()
  } catch {
    return load(params.lang)
  }
}
// With hydrate and no HydrateFallback, the prerender renders the page itself and hydration reuses its data
clientLoader.hydrate = true as const

export function meta({ params, loaderData }: Route.MetaArgs) {
  const lang = params.lang === 'en' ? 'en' : 'zh'
  return pageMeta({ lang, title: `${getI18n(lang).t('nav.posts')} · ${loaderData?.siteName ?? ''}`, path: '/posts' })
}

export default function Posts({ loaderData, params }: Route.ComponentProps) {
  const { t } = useTranslation()
  const lang: Lang = params.lang === 'en' ? 'en' : 'zh'
  const user = useCurrentUser()
  const [searchParams, setSearchParams] = useSearchParams()
  const canWrite = !!user?.write
  const canSeeDrafts = !!user && (user.role === 'admin' || !!user.write || !!user.editing)
  const creating = canWrite && searchParams.get('new') === '1'
  const draftParam = Number(searchParams.get('draft'))
  const draftId = canSeeDrafts && Number.isInteger(draftParam) && draftParam > 0 ? draftParam : null

  const closePaper = () =>
    setSearchParams(
      (current) => {
        current.delete('new')
        current.delete('draft')
        return current
      },
      { replace: true },
    )
  const openDraft = (id: number) =>
    setSearchParams(
      (current) => {
        current.delete('new')
        current.set('draft', String(id))
        return current
      },
      { replace: true },
    )

  if (creating || draftId !== null) {
    return (
      <div className="mx-auto max-w-[48rem]">
        <div className="rounded-3xl border bg-paper p-6 shadow-sm backdrop-blur-md md:p-10">
          <Suspense fallback={<Skeleton className="h-96 w-full" />}>
            {creating ? (
              <PostNewPaper lang={lang} onCancel={closePaper} onDraftCreated={openDraft} />
            ) : (
              <PostInlineEditor postId={draftId!} lang={lang} backLabel={t('collab.backToPosts')} onDone={closePaper} />
            )}
          </Suspense>
        </div>
      </div>
    )
  }

  return (
    <div className="grid gap-10 md:grid-cols-[1fr_15rem]">
      <section>
        <PageHeader
          title={t('nav.posts')}
          description={t('posts.listDescription')}
          actions={
            canSeeDrafts ? (
              <>
                <PostDraftsDialog lang={lang} onOpenDraft={(id) => setSearchParams({ draft: String(id) })} />
                {canWrite && (
                  <Button type="button" size="sm" onClick={() => setSearchParams({ new: '1' })}>
                    <PlusIcon />
                    {t('admin.newPost')}
                  </Button>
                )}
              </>
            ) : undefined
          }
        />
        <PostFeed firstPage={loaderData.firstPage} />
      </section>
      <aside className="md:pt-24">
        <h2 className="mb-4 font-heading text-lg font-bold">{t('nav.tags')}</h2>
        <TagList tags={loaderData.tags} />
      </aside>
    </div>
  )
}
