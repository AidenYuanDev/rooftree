// "Chrome on Windows" from a User-Agent header, for the admin's list of logged in devices; the header
// itself when nothing is recognized

const browsers: [RegExp, string][] = [
  [/Edg\//, 'Edge'],
  [/OPR\//, 'Opera'],
  [/Firefox\//, 'Firefox'],
  [/Chrome\//, 'Chrome'],
  [/Safari\//, 'Safari'],
]

const systems: [RegExp, string][] = [
  [/iPhone|iPad/, 'iOS'],
  [/Android/, 'Android'],
  [/Windows/, 'Windows'],
  [/Mac OS X|Macintosh/, 'macOS'],
  [/Linux/, 'Linux'],
]

export function describeUserAgent(userAgent: string): string {
  const browser = browsers.find(([pattern]) => pattern.test(userAgent))?.[1]
  const system = systems.find(([pattern]) => pattern.test(userAgent))?.[1]
  if (!browser && !system) return userAgent || '?'
  return [browser, system].filter(Boolean).join(' · ')
}
