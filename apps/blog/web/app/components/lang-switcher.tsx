import { LanguagesIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link, useLocation } from 'react-router'
import { Button } from '~/components/ui/button'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { switchLang, useLang } from '~/lib/lang'
import { langs } from '~/lib/schemas'

export function LangSwitcher() {
  const { t } = useTranslation()
  const lang = useLang()
  const location = useLocation()
  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button variant="ghost" size="icon" aria-label={t('nav.language')}>
          <LanguagesIcon />
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end">
        {langs.map((other) => (
          <DropdownMenuItem key={other} asChild disabled={other === lang}>
            <Link to={switchLang(location.pathname, other) + location.search} hrefLang={other} lang={other}>
              {t(`langs.${other}`)}
            </Link>
          </DropdownMenuItem>
        ))}
      </DropdownMenuContent>
    </DropdownMenu>
  )
}
