import { isbot } from 'isbot'
import { useEffect, useRef } from 'react'
import { useLocation } from 'react-router'
import { apiOrigin } from '~/lib/api'
import { useMe } from '~/lib/queries'

// Tells the server which page was read, for the admin's statistics (StatsHandlers.cpp): once per
// page shown, with the other site the reader came from on the first one. Not the admin's own reads,
// not a bot's; the server leaves out the pages that aren't the site's (the drive, the account's)
export function PageViews() {
  const { pathname } = useLocation()
  const me = useMe()
  const role = me.isPending ? undefined : (me.data?.user?.role ?? null)
  const sent = useRef<string | null>(null)

  useEffect(() => {
    if (role === undefined || role === 'admin' || sent.current === pathname || isbot(navigator.userAgent)) return
    const first = sent.current === null
    sent.current = pathname
    void fetch(`${apiOrigin}/api/views`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: pathname, referrer: first ? document.referrer : '' }),
      credentials: 'same-origin',
      keepalive: true,
    }).catch(() => {})
  }, [pathname, role])

  return null
}
