import { data } from 'react-router'
import { isLang, type Lang } from './schemas'

// In loaders: the URL's language, or a 404
export function requireLang(lang: string | undefined): Lang {
  if (!isLang(lang)) throw data(null, { status: 404 })
  return lang
}

// The slug of the placeholder pages react-router.config.ts prerenders: never a real one (a slug can't
// start with a dash), its loaders answer "not found" without asking the API. Not "_": React Router
// reads /x/_.data as the data of /x/
export const placeholderSlug = '-'
