import { useTranslation } from 'react-i18next'
import { Form, Link, useSearchParams } from 'react-router'
import type { Route } from './+types/search'
import { FormError } from '~/components/form'
import { PostCard } from '~/components/post-card'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Skeleton } from '~/components/ui/skeleton'
import { Switch } from '~/components/ui/switch'
import { localePath, useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { useSearch } from '~/lib/queries'
import type { Lang } from '~/lib/schemas'
import { isSearchable, plainText } from '~/lib/text'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'search.title')

// In the browser only: results depend on what is typed, nothing to prerender
export default function Search() {
  const { t } = useTranslation()
  const lang = useLang()
  const [searchParams, setSearchParams] = useSearchParams()
  const query = searchParams.get('q') ?? ''
  // The page's language only, unless asked for all (?all=1): each result then opens in its own
  const allLangs = searchParams.get('all') === '1'
  const search = useSearch(lang, query, allLangs)
  const resultLang = (found: Lang) => (allLangs ? found : lang)
  const posts = search.data?.posts ?? []
  const notes = search.data?.notes ?? []
  const long = isSearchable(query)

  return (
    <div className="mx-auto grid max-w-3xl gap-6">
      <h1 className="text-2xl font-semibold tracking-tight">{t('search.title')}</h1>
      <Form method="get" role="search" className="grid gap-3">
        {allLangs && <input type="hidden" name="all" value="1" />}
        <Input
          key={query}
          name="q"
          type="search"
          defaultValue={query}
          placeholder={t('search.placeholder')}
          aria-label={t('search.title')}
          autoFocus
        />
        <div className="flex items-center gap-2">
          <Switch
            id="search-all-langs"
            checked={allLangs}
            onCheckedChange={(checked) =>
              setSearchParams((params) => {
                if (checked) params.set('all', '1')
                else params.delete('all')
                return params
              })
            }
          />
          <Label htmlFor="search-all-langs">{t('search.allLangs')}</Label>
        </div>
      </Form>
      {!long && <p className="text-sm text-muted-foreground">{t('search.hint')}</p>}
      {long && search.isPending && <Skeleton className="h-32 w-full" />}
      <FormError error={search.error} />
      {long && search.isSuccess && (
        <>
          <p className="text-sm text-muted-foreground">
            {posts.length + notes.length ? t('search.results', { count: posts.length + notes.length }) : t('search.empty', { query })}
          </p>
          <div className="space-y-4">
            {posts.map((post) => (
              <PostCard key={post.id} post={post} linkLang={resultLang(post.lang)} />
            ))}
          </div>
          {notes.length > 0 && (
            <section className="grid gap-2">
              <h2 className="font-heading text-lg font-bold">{t('nav.notes')}</h2>
              {notes.map((note) => (
                <Link
                  key={note.id}
                  to={localePath(resultLang(note.lang), `/notes/${note.id}`)}
                  className="rounded-2xl border bg-card/60 p-4 backdrop-blur-md transition hover:border-primary/40"
                >
                  <span className="block font-heading font-bold" lang={note.lang}>
                    {note.title}
                  </span>
                  <span className="mt-1 line-clamp-2 block text-sm text-muted-foreground" lang={note.lang}>
                    {plainText(note.excerpt)}
                  </span>
                </Link>
              ))}
            </section>
          )}
        </>
      )}
    </div>
  )
}
