import { SmilePlusIcon } from 'lucide-react'
import { use } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useLocation } from 'react-router'
import { toast } from 'sonner'
import { Button } from '~/components/ui/button'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { errorMessage } from '~/i18n'
import { localePath, useLang } from '~/lib/lang'
import { reactionEmoji, ReactionsContext } from '~/lib/reactions'
import { useCurrentUser, useReact, useReactions } from '~/lib/queries'
import { type ReactionKind, reactionKinds, type ReactionState } from '~/lib/schemas'
import { cn } from '~/lib/utils'

const empty: ReactionState = { counts: {}, mine: [] }

// The reactions given so far, each a button that adds or takes back the reader's own, and one to add
// another kind. Loaded in the browser: they change too often for the prerendered pages
export function Reactions({ target, className }: { target: string; className?: string }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const location = useLocation()
  const fromList = use(ReactionsContext)
  const loaded = useReactions(fromList ? [] : [target])
  const state = (fromList ?? loaded.data)?.[target] ?? empty
  const user = useCurrentUser()
  const react = useReact()

  const toggle = (kind: ReactionKind) => {
    if (!user) {
      toast(t('reactions.loginFirst'), {
        action: <Link to={`${localePath(lang, '/login')}?next=${encodeURIComponent(location.pathname)}`}>{t('nav.login')}</Link>,
      })
      return
    }
    if (!user.emailVerified) {
      toast(t('reactions.confirmEmail'))
      return
    }
    react.mutate({ target, kind, on: !state.mine.includes(kind) }, { onError: (error) => toast.error(errorMessage(i18n, error)) })
  }

  const given = reactionKinds.filter((kind) => (state.counts[kind] ?? 0) > 0)
  return (
    <div className={cn('flex flex-wrap items-center gap-1.5', className)}>
      {given.map((kind) => {
        const mine = state.mine.includes(kind)
        return (
          <button
            key={kind}
            type="button"
            aria-pressed={mine}
            aria-label={t(`reactions.${kind}`)}
            title={t(`reactions.${kind}`)}
            disabled={react.isPending}
            onClick={() => toggle(kind)}
            className={cn(
              'flex h-7 items-center gap-1 rounded-full border px-2.5 text-sm tabular-nums transition not-disabled:hover:-translate-y-px',
              mine ? 'border-primary/60 bg-primary/15 text-primary' : 'bg-card/60 text-muted-foreground not-disabled:hover:text-foreground',
            )}
          >
            <span aria-hidden>{reactionEmoji[kind]}</span>
            {state.counts[kind]}
          </button>
        )
      })}
      <DropdownMenu>
        <DropdownMenuTrigger asChild>
          <Button variant="ghost" size="icon-sm" className="size-7 rounded-full text-muted-foreground" aria-label={t('reactions.add')}>
            <SmilePlusIcon />
          </Button>
        </DropdownMenuTrigger>
        <DropdownMenuContent
          align="start"
          collisionPadding={16}
          className="flex w-auto max-w-(--radix-dropdown-menu-content-available-width) min-w-0 flex-wrap gap-1 p-1"
        >
          {reactionKinds.map((kind) => (
            <DropdownMenuItem
              key={kind}
              className={cn('cursor-pointer justify-center px-2 text-lg', state.mine.includes(kind) && 'bg-primary/15')}
              aria-label={t(`reactions.${kind}`)}
              onSelect={() => toggle(kind)}
            >
              {reactionEmoji[kind]}
            </DropdownMenuItem>
          ))}
        </DropdownMenuContent>
      </DropdownMenu>
    </div>
  )
}

// Loads the reactions of targets once, for the Reactions inside
export function ReactionsProvider({ targets, children }: { targets: string[]; children: React.ReactNode }) {
  const reactions = useReactions(targets)
  return <ReactionsContext value={reactions.data ?? {}}>{children}</ReactionsContext>
}
