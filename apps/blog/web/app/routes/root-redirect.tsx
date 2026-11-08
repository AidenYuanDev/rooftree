import { useEffect } from 'react'
import { useNavigate } from 'react-router'
import { preferredLang } from '~/lib/lang'

// "/": the server sends readers to /zh or /en by Accept-Language already (server/StaticHandlers.cpp); this
// page is for everything else (vite dev, a static host), with plain links for readers without JavaScript
export default function RootRedirect() {
  const navigate = useNavigate()
  useEffect(() => {
    void navigate(`/${preferredLang(navigator.languages)}`, { replace: true })
  }, [navigate])

  return (
    <main className="flex min-h-dvh items-center justify-center gap-6 text-lg">
      <a href="/zh" hrefLang="zh" className="underline">
        中文
      </a>
      <a href="/en" hrefLang="en" className="underline">
        English
      </a>
    </main>
  )
}
