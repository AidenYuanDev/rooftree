import type { Config } from '@react-router/dev/config'

// The API the build reads the posts from; it has to be running (and current) during the build
const api = process.env.BLOG_API_URL ?? 'http://127.0.0.1:8100'
const langs = ['zh', 'en']
// Pages without a parameter. The account pages are prerendered as shells: no data, but a real page to
// hydrate quickly
const staticPages = [
  '',
  '/posts',
  '/timeline',
  '/guestbook',
  '/friends',
  '/projects',
  '/about',
  '/rss',
  '/sitemap',
  '/login',
  '/register',
  '/verify-email',
  '/forgot-password',
  '/reset-password',
  '/account',
  '/status',
  '/drive',
  '/subscribe/confirm',
  '/unsubscribe',
]

// Each with the languages it is in: a language's site has only what is written in it
interface Slugs {
  posts: { slug: string; translations: string[] }[]
  redirects: { from: string; to: string }[]
  tags: { slug: string; translations: string[] }[]
}

async function fetchSlugs(): Promise<Slugs> {
  try {
    const response = await fetch(`${api}/api/posts/slugs`)
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}`)
    return (await response.json()) as Slugs
  } catch (error) {
    // A build without the posts would replace the site with an empty one: fail instead, unless asked.
    // The dev server asks for the paths too, where they don't matter (pages render on request)
    if (process.env.NODE_ENV !== 'production' || process.env.BLOG_PRERENDER_WITHOUT_API === '1') {
      console.warn(`No posts prerendered, ${api} is not reachable: ${String(error)}`)
      return { posts: [], redirects: [], tags: [] }
    }
    throw new Error(`Prerendering needs the API at ${api} (BLOG_API_URL): ${String(error)}`, { cause: error })
  }
}

export default {
  // Static files only, served by the blog server from memory; pages that aren't prerendered (admin) get the SPA fallback
  ssr: false,
  async prerender() {
    const slugs = await fetchSlugs()
    const postLangs = new Map(slugs.posts.map((post) => [post.slug, post.translations]))
    return [
      '/',
      ...langs.flatMap((lang) => [
        ...staticPages.map((page) => `/${lang}${page}`),
        ...slugs.posts.filter((post) => post.translations.includes(lang)).map((post) => `/${lang}/posts/${post.slug}`),
        // Old addresses become pages pointing at the new ones
        ...slugs.redirects
          .filter((redirect) => postLangs.get(redirect.to)?.includes(lang))
          .map((redirect) => `/${lang}/posts/${redirect.from}`),
        ...slugs.tags.filter((tag) => tag.translations.includes(lang)).map((tag) => `/${lang}/tags/${tag.slug}`),
        // React Router refuses a route with a loader that no prerendered path matches, which a blog
        // without posts or tags would have. "-" is never a slug (lib/route.ts); the server answers it 404
        `/${lang}/posts/-`,
        `/${lang}/tags/-`,
      ]),
    ]
  },
} satisfies Config
