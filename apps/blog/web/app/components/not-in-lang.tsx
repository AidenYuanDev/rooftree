import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { CottageMark } from '~/components/scenery/mark'
import { Button } from '~/components/ui/button'
import { localePath, useLang } from '~/lib/lang'
import type { Lang } from '~/lib/schemas'

// A post in other languages only: each language's site has only what is written in it, so
// not shown here, only where it is
export function NotInLang({ message, path, translations }: { message: string; path: string; translations: Lang[] }) {
  const { t } = useTranslation()
  const lang = useLang()
  return (
    <div className="flex flex-col items-center gap-4 py-16 text-center">
      <CottageMark className="size-24" />
      <h1 className="text-xl font-semibold">{message}</h1>
      <div className="flex flex-wrap justify-center gap-2">
        {translations.map((other) => (
          <Button key={other} asChild>
            <Link to={localePath(other, path)} hrefLang={other} lang={other}>
              {t('common.readIn', { lang: t(`langs.${other}`) })}
            </Link>
          </Button>
        ))}
        <Button asChild variant="outline">
          <Link to={localePath(lang)}>{t('common.backHome')}</Link>
        </Button>
      </div>
    </div>
  )
}
