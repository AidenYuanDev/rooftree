// What the API returns, as Zod schemas (see api.ts). Field names follow apps/blog/server.

import { z } from 'zod'

export const langs = ['zh', 'en'] as const
export const Lang = z.enum(langs)
export type Lang = z.infer<typeof Lang>

export const isLang = (value: unknown): value is Lang => Lang.safeParse(value).success

// Moderators hide comments and ban users; the admin does everything
export const roles = ['admin', 'moderator', 'user'] as const
export const Role = z.enum(roles)
export type Role = z.infer<typeof Role>
export const canModerate = (user: { role: Role } | null | undefined) => user?.role === 'admin' || user?.role === 'moderator'

// What the admin can take from an account, one by one
export const deniableActions = ['comment', 'react', 'avatar', 'friend', 'chat'] as const
export const DeniableAction = z.enum(deniableActions)
export type DeniableAction = z.infer<typeof DeniableAction>

export const User = z.object({
  id: z.number(),
  username: z.string(),
  displayName: z.string(),
  role: Role,
  // "/uploads/avatar-….webp", null: the initials
  avatar: z.string().nullish(),
  email: z.string().optional(),
  emailVerified: z.boolean().optional(),
  locale: Lang.optional(),
  totpEnabled: z.boolean().optional(),
  // False for an account made by a GitHub or Google login, until it sets one
  hasPassword: z.boolean().optional(),
  bannedUntil: z.string().optional(),
  denied: z.array(DeniableAction).optional(),
  // Has a drive: the admin, and accounts the admin gave one
  drive: z.boolean().optional(),
  // A mail when someone replies to its comments
  replyMails: z.boolean().optional(),
  // Writes posts of its own: the admin, and accounts the admin let
  write: z.boolean().optional(),
  // Invited to edit someone else's post
  editing: z.boolean().optional(),
})
export type User = z.infer<typeof User>

export const Me = z.object({
  user: User.nullable(),
  csrfToken: z.string().nullable(),
})
export type Me = z.infer<typeof Me>

export const Ok = z.object({ ok: z.literal(true) })

// GitHub and Google logins: the ones the server has set up, where to send the browser, an account's links
export const OAuthProviders = z.object({ providers: z.array(z.string()) })
export const OAuthStart = z.object({ url: z.string() })
export const OAuthTotpLogin = Me.extend({ next: z.string() })
// hasAvatar: the provider gave a picture at the last login or link, the account may take it
export const Identity = z.object({ provider: z.string(), login: z.string(), createdAt: z.string(), hasAvatar: z.boolean().optional() })
export type Identity = z.infer<typeof Identity>
export const IdentityList = z.object({ identities: z.array(Identity) })

// GET /api/captcha: a picture as a data: URL, or nothing to answer when the server has it off
export const Captcha = z.union([
  z.object({ enabled: z.literal(false) }),
  z.object({ enabled: z.literal(true), id: z.string(), image: z.string(), width: z.number(), height: z.number() }),
])
export type Captcha = z.infer<typeof Captcha>

export const TagRef = z.object({ slug: z.string(), name: z.string() })

export const PostSummary = z.object({
  id: z.number(),
  slug: z.string(),
  lang: Lang,
  fallback: z.boolean(),
  originalLang: Lang,
  title: z.string(),
  summary: z.string(),
  cover: z.string(),
  tags: z.array(TagRef),
  translations: z.array(Lang),
  commentCount: z.number(),
  publishedAt: z.string(),
  updatedAt: z.string(),
})
export type PostSummary = z.infer<typeof PostSummary>

export const PostList = z.object({
  posts: z.array(PostSummary),
  nextCursor: z.string().nullable(),
})
export type PostList = z.infer<typeof PostList>

export const Post = z.object({
  id: z.number(),
  slug: z.string(),
  lang: Lang,
  fallback: z.boolean(),
  originalLang: Lang,
  translations: z.array(Lang),
  title: z.string(),
  summary: z.string(),
  body: z.string(),
  cover: z.string(),
  author: z.string(),
  tags: z.array(TagRef),
  commentsEnabled: z.boolean(),
  commentCount: z.number(),
  publishedAt: z.string().nullable(),
  updatedAt: z.string(),
  // Older and newer
  previous: z.object({ slug: z.string(), title: z.string() }).nullable(),
  next: z.object({ slug: z.string(), title: z.string() }).nullable(),
  // Who wrote it: its owner first, then its editors
  people: z.array(z.object({ id: z.number(), username: z.string(), displayName: z.string(), avatar: z.string().nullish() })).default([]),
})
export type Post = z.infer<typeof Post>

// Someone's page: who they are and the posts they wrote
export const Person = z.object({
  person: z.object({ id: z.number(), username: z.string(), displayName: z.string(), avatar: z.string().nullish() }),
  since: z.string(),
  posts: z.array(PostSummary),
})
export type Person = z.infer<typeof Person>

export const Tag = z.object({
  slug: z.string(),
  nameZh: z.string(),
  nameEn: z.string(),
  postCount: z.number(),
})
export type Tag = z.infer<typeof Tag>

export const TagList = z.object({ tags: z.array(Tag) })

export const Slugs = z.object({
  posts: z.array(z.object({ slug: z.string(), updatedAt: z.string(), translations: z.array(Lang) })),
  redirects: z.array(z.object({ from: z.string(), to: z.string() })),
  tags: z.array(z.object({ slug: z.string(), translations: z.array(Lang) })),
})
export type Slugs = z.infer<typeof Slugs>

export const Translation = z.object({ title: z.string(), summary: z.string(), body: z.string() })
export type Translation = z.infer<typeof Translation>

// Who a post is of, or edited by
export const PostPerson = z.object({ id: z.number(), username: z.string(), displayName: z.string(), avatar: z.string().nullish() })
export type PostPerson = z.infer<typeof PostPerson>

// What an editor may do on top of editing a post's text (BlogTypes.h)
export const PostRight = z.enum(['publish', 'settings', 'editors'])
export type PostRight = z.infer<typeof PostRight>
export const postRights = PostRight.options

export const AdminPost = z.object({
  id: z.number(),
  slug: z.string(),
  status: z.enum(['draft', 'published']),
  originalLang: Lang,
  cover: z.string(),
  commentsEnabled: z.boolean(),
  tags: z.array(z.string()),
  translations: z.partialRecord(Lang, Translation),
  publishedAt: z.string().nullable(),
  // A draft the server publishes at that time
  publishAt: z.string().nullable(),
  updatedAt: z.string(),
  createdAt: z.string().nullable(),
  owner: PostPerson,
  // What the one asking may do on top of editing its text
  rights: z.array(z.union([PostRight, z.literal('delete')])),
  // The languages its working copy has, published or not (its editors write there, lib/collab.ts)
  draftLangs: z.array(Lang),
  // Publishing it was asked for, the admin's to do
  reviewRequestedAt: z.string().nullish(),
})
export type AdminPost = z.infer<typeof AdminPost>

export const PostEditors = z.object({
  owner: PostPerson,
  editors: z.array(z.object({ user: PostPerson, rights: z.array(PostRight), createdAt: z.string() })),
})
export type PostEditors = z.infer<typeof PostEditors>

export const AdminPostList = z.object({
  posts: z.array(
    z.object({
      id: z.number(),
      slug: z.string(),
      status: z.enum(['draft', 'published']),
      originalLang: Lang,
      title: z.string(),
      translations: z.array(Lang),
      updatedAt: z.string(),
      publishedAt: z.string().nullable(),
      publishAt: z.string().nullable(),
      // The one asking owns it
      mine: z.boolean(),
      reviewRequested: z.boolean().default(false),
    }),
  ),
  hasMore: z.boolean(),
})
export type AdminPostList = z.infer<typeof AdminPostList>

export const TotpSetup = z.object({ secret: z.string(), otpauthUri: z.string() })

export const CommentAuthor = z.object({ id: z.number(), username: z.string(), displayName: z.string(), avatar: z.string().nullish() })

// deleted: the account is gone, its comments kept, with no name
export const CommentUser = CommentAuthor.extend({ role: Role, deleted: z.boolean().optional() })

// What a comment is on: "post:7", "guestbook"
export type CommentTarget = `post:${number}` | 'guestbook'
export const CommentTarget = z.custom<CommentTarget>((value) => typeof value === 'string' && /^(post:\d+|guestbook)$/.test(value))

// The passage of a post a comment is on (lib/passage.ts): its text, the text around it, its block as it
// was (only once the post lost it: orphaned; kept, the admin left it so)
export const CommentAnchor = z.object({
  lang: Lang,
  block: z.number(),
  quote: z.string(),
  prefix: z.string(),
  suffix: z.string(),
  text: z.string().nullish(),
  status: z.enum(['ok', 'orphaned', 'kept']),
})
export type CommentAnchor = z.infer<typeof CommentAnchor>
// As sent: its whole block, no status
export type CommentAnchorInput = Omit<CommentAnchor, 'status' | 'text'> & { text: string }

export const Comment = z.object({
  id: z.number(),
  target: CommentTarget,
  parentId: z.number().nullable(),
  status: z.enum(['visible', 'hidden', 'deleted']),
  body: z.string(),
  author: CommentUser.nullable(),
  replyTo: z.object({ id: z.number(), displayName: z.string() }).nullable(),
  createdAt: z.string(),
  editedAt: z.string().nullable(),
  // The comment a reply answers; its own passage (a reply without one has the passage of what it answers)
  replyToId: z.number().nullish(),
  anchor: CommentAnchor.nullish(),
})
export type Comment = z.infer<typeof Comment>

export const CommentThread = Comment.extend({ replies: z.array(Comment) })
export type CommentThread = z.infer<typeof CommentThread>

export const CommentPage = z.object({ threads: z.array(CommentThread), nextCursor: z.string().nullable() })
export type CommentPage = z.infer<typeof CommentPage>

export const AdminCommentList = z.object({
  comments: z.array(
    z.object({
      id: z.number(),
      target: CommentTarget,
      // Posts are shown by slug, the rest by number
      postSlug: z.string().nullable(),
      author: CommentAuthor.extend({ banned: z.boolean() }),
      body: z.string(),
      status: z.enum(['visible', 'hidden', 'deleted']),
      createdAt: z.string(),
      ip: z.string(),
      isReply: z.boolean(),
      passage: z.object({ lang: Lang, quote: z.string(), text: z.string(), status: z.enum(['ok', 'orphaned', 'kept']) }).nullish(),
    }),
  ),
  nextCursor: z.string().nullable(),
})

// A post's last published versions in each language, newest first
// Every version of a post: each publication, each one an editor saved (draft)
export const PostRevisionItem = z.object({
  id: z.number(),
  lang: Lang,
  kind: z.enum(['published', 'draft']),
  createdAt: z.string(),
  title: z.string(),
  note: z.string(),
  // Of its Markdown, in characters
  length: z.number(),
  // null: the server published it (a scheduled post)
  author: PostPerson.nullish(),
})
export type PostRevisionItem = z.infer<typeof PostRevisionItem>

export const PostRevisions = z.object({ revisions: z.array(PostRevisionItem), hasMore: z.boolean() })
export type PostRevisions = z.infer<typeof PostRevisions>

const RevisionText = z.object({ title: z.string(), summary: z.string(), body: z.string() })

// One in full, with the version before it in its language (none: the first)
export const PostRevision = PostRevisionItem.extend({ summary: z.string(), body: z.string(), previous: RevisionText.nullish() })
export type PostRevision = z.infer<typeof PostRevision>
export type AdminCommentList = z.infer<typeof AdminCommentList>

export const AdminUser = z.object({
  id: z.number(),
  username: z.string(),
  displayName: z.string(),
  // Not for moderators
  email: z.string().optional(),
  role: Role,
  emailVerified: z.boolean(),
  bannedUntil: z.string().nullable(),
  banReason: z.string().nullable(),
  createdAt: z.string(),
  commentCount: z.number(),
  avatar: z.string().nullish(),
  denied: z.array(DeniableAction),
  deleted: z.boolean(),
  totpEnabled: z.boolean(),
  lastSeenAt: z.string().nullable(),
  onlineSeconds: z.number(),
  // Now: since when, the pages open, how many
  online: z.object({ since: z.string(), pages: z.array(z.string()), connections: z.number() }).nullable(),
  // Not for moderators. quota: its own size in bytes, null the default
  drive: z
    .object({
      enabled: z.boolean(),
      quota: z.number().nullable(),
      defaultQuota: z.number(),
      used: z.number(),
      // Days its trash keeps what it deleted, null: the default
      trashDays: z.number().nullable(),
      defaultTrashDays: z.number(),
    })
    .optional(),
  // Writes posts of its own
  write: z.boolean(),
})
export type AdminUser = z.infer<typeof AdminUser>

export const AdminUserList = z.object({ users: z.array(AdminUser), hasMore: z.boolean() })
export type AdminUserList = z.infer<typeof AdminUserList>

export const AdminUserSession = z.object({
  id: z.string(),
  ip: z.string(),
  userAgent: z.string(),
  createdAt: z.string(),
  lastSeenAt: z.string(),
  expiresAt: z.string(),
  // The admin's own
  current: z.boolean(),
})
export type AdminUserSession = z.infer<typeof AdminUserSession>

export const AdminUserDetails = AdminUser.extend({
  email: z.string(),
  reactionCount: z.number(),
  locale: Lang,
  hasPassword: z.boolean(),
  identities: z.array(z.object({ provider: z.string(), login: z.string(), createdAt: z.string() })),
  sessions: z.array(AdminUserSession),
})
export type AdminUserDetails = z.infer<typeof AdminUserDetails>

// Pages open now: all of them, and the logged in users among their readers
export const OnlineCounts = z.object({ viewers: z.number(), users: z.number() })

// The status page's numbers (ServerStatus.h): now, and a sample per interval, by column
const statusMeasures = [
  'readers',
  'pages',
  'requests',
  'updateAvgUs',
  'updateMaxUs',
  'memoryMiB',
  'cpuPercent',
  'load',
  'hostMemoryPercent',
  'diskFreePercent',
] as const
export type StatusMeasure = (typeof statusMeasures)[number]
const measures = <T extends z.ZodType>(type: T) =>
  z.object(Object.fromEntries(statusMeasures.map((key) => [key, type])) as Record<StatusMeasure, T>)

export const ServerStatus = z.object({
  now: measures(z.number()).extend({
    at: z.number(),
    uptime: z.number(),
    cores: z.number(),
    chatMessagesTotal: z.number().default(0),
    chatChannelsCount: z.number().default(0),
  }),
  interval: z.number(),
  series: measures(z.array(z.number())).extend({ at: z.array(z.number()) }),
})
export type ServerStatus = z.infer<typeof ServerStatus>

export const IpBan = z.object({ ip: z.string(), reason: z.string(), createdAt: z.string(), expiresAt: z.string().nullable() })
export type IpBan = z.infer<typeof IpBan>

export const IpBanList = z.object({ bans: z.array(IpBan) })

export const Upload = z.object({ url: z.string(), mime: z.string(), size: z.number() })

export const SearchResults = z.object({
  posts: z.array(PostSummary),
})

// The site's profile (admin "Site settings"), in the page's language: the home page's introduction
export const socialKinds = [
  'github',
  'x',
  'email',
  'rss',
  'bilibili',
  'telegram',
  'music',
  'mastodon',
  'bluesky',
  'zhihu',
  'weibo',
  'steam',
  'discord',
  'youtube',
] as const
export const SocialKind = z.enum(socialKinds)
export type SocialKind = z.infer<typeof SocialKind>

export const Social = z.object({ kind: SocialKind, url: z.string() })

export const SiteStats = z.object({
  posts: z.number(),
  words: z.number(),
  // Since Site.since, counting the first day
  days: z.number(),
})
export type SiteStats = z.infer<typeof SiteStats>

export const Site = z.object({
  name: z.string(),
  author: z.string(),
  avatar: z.string(),
  // "Writing **C++** ...": the starred part is highlighted
  tagline: z.string(),
  bio: z.string(),
  quote: z.string(),
  since: z.string(),
  socials: z.array(Social),
  stats: SiteStats,
})
export type Site = z.infer<typeof Site>

// Markdown, in the page's language
export const About = z.object({ about: z.string(), aboutSite: z.string() })
export type About = z.infer<typeof About>

const Localized = z.object({ zh: z.string(), en: z.string() })

// The profile as the admin edits it, both languages
export const AdminSite = z.object({
  name: Localized,
  author: z.string(),
  avatar: z.string(),
  tagline: Localized,
  bio: Localized,
  quote: Localized,
  since: z.string(),
  socials: z.array(Social),
  about: Localized,
  aboutSite: Localized,
  // The admin reads what writers publish first
  reviewWriters: z.boolean().default(false),
})
export type AdminSite = z.infer<typeof AdminSite>

export const TimelineItem = z.object({
  kind: z.literal('post'),
  id: z.number(),
  slug: z.string(),
  title: z.string(),
  publishedAt: z.string(),
})
export type TimelineItem = z.infer<typeof TimelineItem>

export const Timeline = z.object({ items: z.array(TimelineItem) })

export const Friend = z.object({ id: z.number(), name: z.string(), url: z.string(), avatar: z.string(), description: z.string() })
export type Friend = z.infer<typeof Friend>

export const FriendList = z.object({ friends: z.array(Friend) })

export const AdminFriend = Friend.extend({
  status: z.enum(['pending', 'approved', 'rejected']),
  message: z.string(),
  rejectReason: z.string(),
  sort: z.number(),
  createdAt: z.string(),
  reviewedAt: z.string().nullable(),
  applicant: CommentAuthor.nullable(),
})
export type AdminFriend = z.infer<typeof AdminFriend>

export const AdminFriendList = z.object({ friends: z.array(AdminFriend) })

export const Project = z.object({
  id: z.number(),
  name: z.string(),
  url: z.string(),
  repoUrl: z.string(),
  icon: z.string(),
  descriptionZh: z.string(),
  descriptionEn: z.string(),
  sort: z.number(),
})
export type Project = z.infer<typeof Project>

export const ProjectList = z.object({ projects: z.array(Project) })

// Uploaded images by file name: the blurred placeholder and size the build puts into posts
export const UploadMeta = z.object({
  images: z.record(z.string(), z.object({ thumbhash: z.string(), width: z.number(), height: z.number() })),
})
export type UploadMeta = z.infer<typeof UploadMeta>

// Readers' reactions to a post or comment ("comment:12"): counts per kind, and the
// reader's own
export const reactionKinds = ['like', 'heart', 'laugh', 'party', 'cat', 'star', 'eyes', 'thinking', 'thanks'] as const
export const ReactionKind = z.enum(reactionKinds)
export type ReactionKind = z.infer<typeof ReactionKind>

export const ReactionState = z.object({ counts: z.partialRecord(ReactionKind, z.number()), mine: z.array(ReactionKind) })
export type ReactionState = z.infer<typeof ReactionState>

export const ReactionList = z.object({ reactions: z.record(z.string(), ReactionState) })

// Drives (lib/drive.ts): an account's own folders and files, and the links it gives others

export const DriveFolder = z.object({ id: z.number(), parentId: z.number().nullable(), name: z.string(), createdAt: z.string() })
export type DriveFolder = z.infer<typeof DriveFolder>

export const DriveFile = z.object({
  id: z.number(),
  folderId: z.number().nullable(),
  name: z.string(),
  size: z.number(),
  mime: z.string(),
  createdAt: z.string(),
  updatedAt: z.string(),
  // Has a small picture (lib/thumbnail.ts)
  thumb: z.boolean(),
})
export type DriveFile = z.infer<typeof DriveFile>

// The folders from the drive's root (a link's folder) down to the one shown
export const DrivePath = z.array(z.object({ id: z.number(), name: z.string() }))

// trashDays: how long the trash keeps what is deleted, 0: deleted at once
export const DriveUsage = z.object({
  used: z.number(),
  quota: z.number(),
  maxFileSize: z.number(),
  chunkSize: z.number(),
  trashDays: z.number(),
})
export type DriveUsage = z.infer<typeof DriveUsage>

// A folder's contents; folder null: the root
export const DriveListing = z.object({
  folder: DriveFolder.nullable(),
  path: DrivePath,
  folders: z.array(DriveFolder),
  files: z.array(DriveFile),
  usage: DriveUsage,
})
export type DriveListing = z.infer<typeof DriveListing>

export const DriveFolders = z.object({ folders: z.array(DriveFolder) })

// Reads of the site, the admin's page: per day (the last is today), the most read pages, where readers came from
export const AdminStats = z.object({
  today: z.string(),
  days: z.array(z.object({ day: z.string(), views: z.number(), visitors: z.number() })),
  pages: z.array(z.object({ path: z.string(), views: z.number(), visitors: z.number() })),
  referrers: z.array(z.object({ host: z.string(), views: z.number() })),
})
export type AdminStats = z.infer<typeof AdminStats>

// The admin's page of drives: the disk they are on, every account's (trashed: of used, what is in its trash)
export const AdminDrives = z.object({
  disk: z.object({ total: z.number(), free: z.number(), minFree: z.number() }),
  quotas: z.number(),
  used: z.number(),
  accounts: z.array(
    z.object({
      id: z.number(),
      username: z.string(),
      displayName: z.string(),
      role: z.string(),
      enabled: z.boolean(),
      quota: z.number(),
      used: z.number(),
      files: z.number(),
      trashed: z.number(),
      trashDays: z.number(),
    }),
  ),
})
export type AdminDrives = z.infer<typeof AdminDrives>

// Deleted: into the trash, or for good when the account keeps none
export const DriveDeleted = z.object({ ok: z.literal(true), trashed: z.boolean() })

// A file or folder in the trash (with what is under it), the folders it was in, when it goes for good
export const DriveTrashItem = z.object({
  kind: z.enum(['file', 'folder']),
  id: z.number(),
  name: z.string(),
  size: z.number(),
  mime: z.string().optional(),
  trashedAt: z.string(),
  purgeAt: z.string(),
  from: z.array(z.string()),
})
export type DriveTrashItem = z.infer<typeof DriveTrashItem>

export const DriveTrash = z.object({ trash: z.array(DriveTrashItem), days: z.number() })

export const shareExpiries = ['1d', '7d', '30d', 'never'] as const
export type ShareExpiry = (typeof shareExpiries)[number]

export const DriveShare = z.object({
  id: z.number(),
  token: z.string(),
  kind: z.enum(['file', 'folder']),
  targetId: z.number(),
  name: z.string(),
  hasPassword: z.boolean(),
  expiresAt: z.string().nullable(),
  expired: z.boolean(),
  createdAt: z.string(),
  // Accounts that opened it, how often in all, how often they downloaded
  visitors: z.number(),
  views: z.number(),
  downloads: z.number(),
  // What it shows is in the trash: it opens nothing until restored
  trashed: z.boolean(),
})
export type DriveShare = z.infer<typeof DriveShare>

export const DriveShares = z.object({ shares: z.array(DriveShare) })

export const ShareAccess = z.object({
  access: z.array(
    z.object({
      userId: z.number(),
      displayName: z.string(),
      username: z.string(),
      views: z.number(),
      downloads: z.number(),
      firstAt: z.string(),
      lastAt: z.string(),
    }),
  ),
})

// What a link shows another account: locked until its password is given; then the file, or a folder of
// it (path from the link's folder)
export const ShareView = z.object({
  share: z.object({
    kind: z.enum(['file', 'folder']),
    owner: z.string(),
    expiresAt: z.string().nullable(),
    locked: z.boolean(),
    isOwner: z.boolean(),
    name: z.string().optional(),
  }),
  file: DriveFile.optional(),
  folder: DriveFolder.optional(),
  path: DrivePath.optional(),
  folders: z.array(DriveFolder).optional(),
  files: z.array(DriveFile).optional(),
})
export type ShareView = z.infer<typeof ShareView>

// User friendships (social) between readers

export const UserFriendItem = z.object({
  id: z.number(),
  username: z.string(),
  displayName: z.string(),
  avatar: z.string().nullable(),
  online: z.boolean(),
  since: z.string(),
})
export type UserFriendItem = z.infer<typeof UserFriendItem>

export const UserFriendsList = z.object({
  friends: z.array(UserFriendItem),
})
export type UserFriendsList = z.infer<typeof UserFriendsList>

export const UserFriendRequestItem = z.object({
  id: z.number(),
  user: UserFriendItem,
  message: z.string(),
  createdAt: z.string(),
})
export type UserFriendRequestItem = z.infer<typeof UserFriendRequestItem>

export const UserFriendRequests = z.object({
  incoming: z.array(UserFriendRequestItem),
  outgoing: z.array(UserFriendRequestItem),
})
export type UserFriendRequests = z.infer<typeof UserFriendRequests>

export const UserFriendshipStatus = z.object({
  status: z.enum(['none', 'pending_sent', 'pending_received', 'friends', 'self']),
  requestId: z.number().nullable(),
})
export type UserFriendshipStatus = z.infer<typeof UserFriendshipStatus>

// Chat system (Square & Direct Friend chats)

export const ChatMessageItem = z.object({
  id: z.number(),
  channelId: z.string(),
  senderId: z.number(),
  senderName: z.string(),
  senderAvatar: z.string().nullable(),
  content: z.string(),
  createdAt: z.string(),
})
export type ChatMessageItem = z.infer<typeof ChatMessageItem>

export const ChatMessagesList = z.object({
  messages: z.array(ChatMessageItem),
  hasMore: z.boolean(),
})
export type ChatMessagesList = z.infer<typeof ChatMessagesList>

// A moderator's removal (Admin.fbs ChatRemoved): how many messages went
export const ChatRemoved = z.object({ removed: z.number().int().default(0) })

export const ChatChannelItem = z.object({
  id: z.string(),
  type: z.enum(['square', 'group', 'direct']),
  name: z.string(),
  avatar: z.string().nullable(),
  friend: UserFriendItem.nullable(),
  lastMessage: ChatMessageItem.nullable(),
  unreadCount: z.number(),
  memberCount: z.number().default(0),
})
export type ChatChannelItem = z.infer<typeof ChatChannelItem>

export const ChatChannelsList = z.object({
  channels: z.array(ChatChannelItem),
})
export type ChatChannelsList = z.infer<typeof ChatChannelsList>

export const ChatUnreadCount = z.object({
  count: z.number(),
})
export type ChatUnreadCount = z.infer<typeof ChatUnreadCount>

export const ChatGroupMemberItem = z.object({
  id: z.number(),
  username: z.string(),
  displayName: z.string(),
  avatar: z.string().nullable(),
  role: z.enum(['owner', 'member']),
  online: z.boolean(),
})
export type ChatGroupMemberItem = z.infer<typeof ChatGroupMemberItem>

export const ChatGroupMembersList = z.object({
  members: z.array(ChatGroupMemberItem),
})
export type ChatGroupMembersList = z.infer<typeof ChatGroupMembersList>
