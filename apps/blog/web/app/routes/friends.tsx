import { zodResolver } from '@hookform/resolvers/zod'
import { useForm } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { Link, useLocation } from 'react-router'
import { z } from 'zod'
import type { Route } from './+types/friends'
import { FormError, FormField } from '~/components/form'
import { PageHeader } from '~/components/page-header'
import { Alert, AlertDescription } from '~/components/ui/alert'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { Textarea } from '~/components/ui/textarea'
import { getI18n } from '~/i18n'
import { fetchFriends, fetchSiteCached } from '~/lib/content'
import { applyFieldErrors } from '~/lib/form-errors'
import { useHydrated } from '~/lib/hydrated'
import { localePath, useLang } from '~/lib/lang'
import { pageMeta, publicOrigin } from '~/lib/meta'
import { useApplyFriend, useCurrentUser } from '~/lib/queries'
import { requireLang } from '~/lib/route'
import type { Friend, Site } from '~/lib/schemas'

async function load(lang: string | undefined) {
  const valid = requireLang(lang)
  const [friends, site] = await Promise.all([fetchFriends(), fetchSiteCached(valid)])
  return { friends, site }
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
  return pageMeta({ lang, title: `${getI18n(lang).t('nav.friends')} · ${loaderData?.site.name ?? ''}`, path: '/friends' })
}

function FriendCard({ friend }: { friend: Friend }) {
  return (
    <a
      href={friend.url}
      target="_blank"
      rel="noopener"
      className="group flex items-center gap-4 rounded-2xl border bg-card/60 p-4 shadow-sm backdrop-blur-md transition hover:-translate-y-0.5 hover:border-primary/40"
    >
      {friend.avatar ? (
        <img src={friend.avatar} alt="" loading="lazy" className="size-12 shrink-0 rounded-full border object-cover" />
      ) : (
        <span className="flex size-12 shrink-0 items-center justify-center rounded-full bg-primary/15 font-heading text-lg font-bold text-primary">
          {[...friend.name][0]}
        </span>
      )}
      <span className="min-w-0">
        <span className="block truncate font-heading font-bold group-hover:text-primary">{friend.name}</span>
        {friend.description && <span className="mt-0.5 line-clamp-2 text-sm text-muted-foreground">{friend.description}</span>}
      </span>
    </a>
  )
}

const applyForm = z.object({
  name: z.string().trim().min(1, 'required').max(50, 'too_long'),
  url: z
    .string()
    .trim()
    .max(255, 'too_long')
    .refine((v) => /^https?:\/\/[^\s@]{4,}$/.test(v), 'invalid'),
  avatar: z
    .string()
    .trim()
    .max(255, 'too_long')
    .refine((v) => v === '' || v.startsWith('https://'), 'invalid'),
  description: z.string().trim().max(200, 'too_long'),
  message: z.string().trim().max(500, 'too_long'),
})
type ApplyForm = z.infer<typeof applyForm>

function Apply() {
  const { t } = useTranslation()
  const hydrated = useHydrated()
  const lang = useLang()
  const location = useLocation()
  const user = useCurrentUser()
  const apply = useApplyFriend()
  const form = useForm<ApplyForm>({
    resolver: zodResolver(applyForm),
    defaultValues: { name: '', url: '', avatar: '', description: '', message: '' },
  })

  if (user === null) {
    return (
      <p className="text-sm">
        <Link to={`${localePath(lang, '/login')}?next=${encodeURIComponent(location.pathname)}`} className="text-primary underline">
          {t('friends.loginToApply')}
        </Link>
      </p>
    )
  }
  if (user && !user.emailVerified) return <p className="text-sm text-muted-foreground">{t('friends.verifyToApply')}</p>
  if (apply.isSuccess)
    return (
      <Alert>
        <AlertDescription>{t('friends.applied')}</AlertDescription>
      </Alert>
    )

  const errors = form.formState.errors
  const onSubmit = form.handleSubmit((values) => apply.mutate(values, { onError: (error) => applyFieldErrors(form, error) }))
  return (
    <form method="post" onSubmit={onSubmit} className="grid gap-4 sm:grid-cols-2" noValidate>
      <FormField label={t('friends.name')} error={errors.name?.message}>
        {(id, describedBy) => <Input id={id} aria-describedby={describedBy} {...form.register('name')} />}
      </FormField>
      <FormField label={t('friends.url')} error={errors.url?.message}>
        {(id, describedBy) => <Input id={id} type="url" placeholder="https://" aria-describedby={describedBy} {...form.register('url')} />}
      </FormField>
      <FormField label={t('friends.avatar')} error={errors.avatar?.message}>
        {(id, describedBy) => (
          <Input id={id} type="url" placeholder="https://" aria-describedby={describedBy} {...form.register('avatar')} />
        )}
      </FormField>
      <FormField label={t('friends.descriptionField')} error={errors.description?.message}>
        {(id, describedBy) => <Input id={id} aria-describedby={describedBy} {...form.register('description')} />}
      </FormField>
      <div className="sm:col-span-2">
        <FormField label={t('friends.message')} error={errors.message?.message}>
          {(id, describedBy) => <Textarea id={id} rows={3} aria-describedby={describedBy} {...form.register('message')} />}
        </FormField>
      </div>
      <FormError error={apply.error} />
      <div className="sm:col-span-2">
        <Button type="submit" disabled={!hydrated || apply.isPending}>
          {t('friends.submit')}
        </Button>
      </div>
    </form>
  )
}

// What others put on their page to link here
function OurLink({ site }: { site: Site }) {
  const { t } = useTranslation()
  const rows: [string, string][] = [
    [t('friends.name'), site.name],
    [t('friends.url'), publicOrigin],
    ...(site.avatar
      ? ([[t('friends.avatar'), site.avatar.startsWith('/') ? publicOrigin + site.avatar : site.avatar]] as [string, string][])
      : []),
    ...(site.tagline ? ([[t('friends.descriptionField'), site.tagline.replace(/\*\*/g, '')]] as [string, string][]) : []),
  ]
  return (
    <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-sm">
      {rows.map(([label, value]) => (
        <div key={label} className="contents">
          <dt className="text-muted-foreground">{label}</dt>
          <dd className="font-mono break-all select-all">{value}</dd>
        </div>
      ))}
    </dl>
  )
}

export default function Friends({ loaderData }: Route.ComponentProps) {
  const { t } = useTranslation()
  return (
    <div>
      <PageHeader title={t('nav.friends')} description={t('friends.description')} />
      {loaderData.friends.length === 0 && <p className="py-8 text-center text-muted-foreground">{t('friends.empty')}</p>}
      <ul className="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
        {loaderData.friends.map((friend, i) => (
          <li key={friend.id} className="rise-in" style={{ '--rise-index': Math.min(i, 8) } as React.CSSProperties}>
            <FriendCard friend={friend} />
          </li>
        ))}
      </ul>

      <div className="mt-16 grid gap-6 lg:grid-cols-[1fr_22rem]">
        <section className="rounded-3xl border bg-paper p-6 backdrop-blur-md">
          <h2 className="mb-4 font-heading text-xl font-bold">{t('friends.applyTitle')}</h2>
          <Apply />
        </section>
        <section className="rounded-3xl border bg-paper p-6 backdrop-blur-md">
          <h2 className="mb-4 font-heading text-xl font-bold">{t('friends.ourLink')}</h2>
          <OurLink site={loaderData.site} />
        </section>
      </div>
    </div>
  )
}
