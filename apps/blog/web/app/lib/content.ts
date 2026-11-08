// The public pages' data. Route loaders call these at build time (prerender), their clientLoaders in
// the browser when a page wasn't prerendered yet (a post published after the last build).

import { ApiError, api } from './api'
import { z } from 'zod'
import { About, FriendList, Lang, Post, PostList, ProjectList, Site, Slugs, TagList, Timeline } from './schemas'

export const pageSize = 10

export function fetchPostList(lang: Lang, options: { tag?: string; cursor?: string | null } = {}) {
  const query = new URLSearchParams({ lang, limit: String(pageSize) })
  if (options.tag) query.set('tag', options.tag)
  if (options.cursor) query.set('cursor', options.cursor)
  return api(`/api/posts?${query}`, PostList)
}

// A post that is there, not in the language asked for: each language's site has only what is
// written in it. The languages it is in
export interface NotInLang {
  notInLang: Lang[]
}

// A 404: null when there is no such thing, NotInLang when it is in other languages only
function notFound(error: unknown): NotInLang | null {
  if (!(error instanceof ApiError) || error.status !== 404) throw error
  const translations = z.array(Lang).safeParse(error.details.translations)
  return error.code === 'not_in_lang' && translations.success ? { notInLang: translations.data } : null
}

// A moved one answers with its new slug (the 301 is followed)
export async function fetchPost(lang: Lang, slug: string) {
  try {
    return await api(`/api/posts/${encodeURIComponent(slug)}?lang=${lang}`, Post)
  } catch (error) {
    return notFound(error)
  }
}

// postCount: of the posts in lang
export async function fetchTags(lang: Lang) {
  return (await api(`/api/tags?lang=${lang}`, TagList)).tags
}

export function fetchSlugs() {
  return api('/api/posts/slugs', Slugs)
}

export function fetchSite(lang: Lang) {
  return api(`/api/site?lang=${lang}`, Site)
}

export function fetchAbout(lang: Lang) {
  return api(`/api/site/about?lang=${lang}`, About)
}

export async function fetchTimeline(lang: Lang) {
  return (await api(`/api/timeline?lang=${lang}`, Timeline)).items
}

export async function fetchFriends() {
  return (await api('/api/friends', FriendList)).friends
}

export async function fetchProjects() {
  return (await api('/api/projects', ProjectList)).projects
}

// The profile is read by most pages' loaders: a build (a process of its own) asks once per language.
// The dev server asks every time, so a change in the admin shows right away
const siteCache = new Map<Lang, { at: number; site: Promise<Site> }>()
const siteCacheTime = import.meta.env.DEV ? 0 : 60_000

export function fetchSiteCached(lang: Lang): Promise<Site> {
  const cached = siteCache.get(lang)
  if (cached && Date.now() - cached.at < siteCacheTime) return cached.site
  const site = fetchSite(lang)
  // A failed fetch isn't kept
  site.catch(() => siteCache.delete(lang))
  siteCache.set(lang, { at: Date.now(), site })
  return site
}
