import { type RouteConfig, index, route } from '@react-router/dev/routes'

// Every page lives under its language, /zh/... and /en/...; "/" sends readers to theirs.
// Prerendered (react-router.config.ts): the home page, posts, tags, the other public pages and
// the account pages', the status page's and the drive's shells.
// Everything else (admin, links to what is in a drive) is served by the SPA fallback.
export default [
  index('routes/root-redirect.tsx'),
  route(':lang', 'routes/lang.tsx', [
    index('routes/home.tsx'),
    route('posts', 'routes/posts.tsx'),
    route('posts/:slug', 'routes/post.tsx'),
    route('tags/:tag', 'routes/tag.tsx'),
    route('timeline', 'routes/timeline.tsx'),
    route('guestbook', 'routes/guestbook.tsx'),
    route('friends', 'routes/friends.tsx'),
    route('projects', 'routes/projects.tsx'),
    route('about', 'routes/about.tsx'),
    route('rss', 'routes/rss.tsx'),
    route('sitemap', 'routes/sitemap.tsx'),
    route('subscribe/confirm', 'routes/subscribe-confirm.tsx'),
    route('unsubscribe', 'routes/unsubscribe.tsx'),
    route('search', 'routes/search.tsx'),
    route('login', 'routes/login.tsx'),
    route('register', 'routes/register.tsx'),
    route('verify-email', 'routes/verify-email.tsx'),
    route('forgot-password', 'routes/forgot-password.tsx'),
    route('reset-password', 'routes/reset-password.tsx'),
    route('account', 'routes/account.tsx'),
    route('status', 'routes/status.tsx'),
    route('drive', 'routes/drive.tsx'),
    route('s/:token', 'routes/share.tsx'),
    route('u/:username', 'routes/person.tsx'),
    route('write', 'routes/write/layout.tsx', [
      index('routes/write/posts.tsx'),
      route('new', 'routes/write/post-edit.tsx', { id: 'write-post-new' }),
      route(':id', 'routes/write/post-edit.tsx', { id: 'write-post-edit' }),
    ]),
    route('admin', 'routes/admin/layout.tsx', [
      index('routes/admin/posts.tsx'),
      route('posts/new', 'routes/admin/post-edit.tsx', { id: 'admin-post-new' }),
      route('posts/:id', 'routes/admin/post-edit.tsx', { id: 'admin-post-edit' }),
      route('tags', 'routes/admin/tags.tsx'),
      route('friends', 'routes/admin/friends.tsx'),
      route('projects', 'routes/admin/projects.tsx'),
      route('site', 'routes/admin/site.tsx'),
      route('comments', 'routes/admin/comments.tsx'),
      route('users', 'routes/admin/users.tsx'),
      route('users/:id', 'routes/admin/user.tsx'),
      route('users/:id/drive', 'routes/admin/user-drive.tsx'),
      route('drives', 'routes/admin/drives.tsx'),
      route('stats', 'routes/admin/stats.tsx'),
    ]),
    route('*', 'routes/not-found.tsx'),
  ]),
] satisfies RouteConfig
