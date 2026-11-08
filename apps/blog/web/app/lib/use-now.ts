import { useEffect, useState } from 'react'

// The time, again every interval: for "online for 12 minutes" and the like
export function useNow(interval = 30_000): number {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), interval)
    return () => clearInterval(timer)
  }, [interval])
  return now
}
