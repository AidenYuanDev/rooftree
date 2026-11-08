import { MonitorIcon, MoonIcon, SunIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Button } from '~/components/ui/button'
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuRadioGroup,
  DropdownMenuRadioItem,
  DropdownMenuTrigger,
} from '~/components/ui/dropdown-menu'
import { useRef } from 'react'
import { switchTheme, type Theme, useThemeStore } from '~/stores/theme'

export function ThemeToggle() {
  const { t } = useTranslation()
  const theme = useThemeStore((state) => state.theme)
  const button = useRef<HTMLButtonElement>(null)
  const choose = (value: string) => {
    const box = button.current?.getBoundingClientRect()
    switchTheme(value as Theme, box ? box.left + box.width / 2 : innerWidth / 2, box ? box.top + box.height / 2 : 0)
  }
  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button ref={button} variant="ghost" size="icon" aria-label={t('nav.theme')}>
          <SunIcon className="dark:hidden" />
          <MoonIcon className="hidden dark:block" />
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end">
        <DropdownMenuRadioGroup value={theme} onValueChange={choose}>
          <DropdownMenuRadioItem value="light">
            <SunIcon />
            {t('nav.themeLight')}
          </DropdownMenuRadioItem>
          <DropdownMenuRadioItem value="dark">
            <MoonIcon />
            {t('nav.themeDark')}
          </DropdownMenuRadioItem>
          <DropdownMenuRadioItem value="system">
            <MonitorIcon />
            {t('nav.themeSystem')}
          </DropdownMenuRadioItem>
        </DropdownMenuRadioGroup>
      </DropdownMenuContent>
    </DropdownMenu>
  )
}
