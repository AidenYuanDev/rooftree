import { useEffect, useState } from 'react'

// Seconds left of a wait started with start(seconds), 0 when there is none
export function useCountdown(): [number, (seconds: number) => void] {
  const [until, setUntil] = useState(0)
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    if (until <= now) return
    const timer = setTimeout(() => setNow(Date.now()), Math.min(1000, until - now))
    return () => clearTimeout(timer)
  }, [until, now])
  const left = Math.max(0, Math.ceil((until - now) / 1000))
  return [
    left,
    (seconds: number) => {
      const start = Date.now()
      setNow(start)
      setUntil(start + seconds * 1000)
    },
  ]
}
