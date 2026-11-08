// TanStack Query hooks around the API: the session (who is logged in and the CSRF token), and the
// admin's reads and writes. Public pages get their data from route loaders instead (prerendered).

import { type QueryClient, useInfiniteQuery, useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useId } from 'react'
import { z } from 'zod'
import { api, setCsrfToken, uploadFile } from './api'
import { drivePath } from './drive'
import {
  AdminDrives,
  AdminStats,
  DriveDeleted,
  DriveFile,
  DriveFolder,
  DriveFolders,
  DriveListing,
  DriveShare,
  DriveShares,
  DriveTrash,
  type ShareExpiry,
  ShareAccess,
  ShareView,
  AdminCommentList,
  AdminFriend,
  AdminFriendList,
  AdminSite,
  AdminPost,
  PostEditors,
  Person,
  PostRevision,
  type PostRight,
  AdminPostList,
  AdminUserDetails,
  AdminUserList,
  type DeniableAction,
  Captcha,
  Comment,
  type CommentAnchorInput,
  CommentPage,
  type CommentTarget,
  PostRevisions,
  IpBan,
  IpBanList,
  type Lang,
  IdentityList,
  Me,
  OAuthProviders,
  OAuthStart,
  OAuthTotpLogin,
  Ok,
  OnlineCounts,
  Project,
  ProjectList,
  ReactionList,
  type ReactionKind,
  ReactionState,
  SearchResults,
  ServerStatus,
  Tag,
  TagList,
  TotpSetup,
  UserFriendsList,
  UserFriendRequests,
  UserFriendshipStatus,
  ChatChannelsList,
  ChatChannelItem,
  ChatMessagesList,
  ChatMessageItem,
  ChatRemoved,
  ChatUnreadCount,
  ChatGroupMembersList,
  type Role,
  type User,
} from './schemas'
import { mergeStatus } from './status'
import { isSearchable } from './text'
import { useLiveStore } from '~/stores/live'

export const queryKeys = {
  me: ['me'] as const,
  oauthProviders: ['oauth-providers'] as const,
  identities: ['identities'] as const,
  adminPosts: (status: string, page: number) => ['admin', 'posts', status, page] as const,
  adminPost: (id: number) => ['admin', 'post', id] as const,
  tags: ['tags'] as const,
  comments: (target: string) => ['comments', target] as const,
}

function storeMe(queryClient: QueryClient, me: Me) {
  setCsrfToken(me.csrfToken)
  queryClient.setQueryData(queryKeys.me, me)
}

export function useMe() {
  return useQuery({
    queryKey: queryKeys.me,
    queryFn: async () => {
      const me = await api('/api/auth/me', Me)
      setCsrfToken(me.csrfToken)
      return me
    },
    staleTime: 60_000,
  })
}

export function useCurrentUser(): User | null | undefined {
  return useMe().data?.user
}

export interface LoginInput {
  login: string
  password: string
  totp?: string
}

export function useLogin() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: LoginInput) => api('/api/auth/login', Me, { method: 'POST', body: input }),
    onSuccess: (me) => storeMe(queryClient, me),
  })
}

export function useLogout() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: () => api('/api/auth/logout', Ok, { method: 'POST' }),
    onSettled: () => {
      storeMe(queryClient, { user: null, csrfToken: null })
      queryClient.removeQueries({ queryKey: ['admin'] })
    },
  })
}

// What a request that mails someone carries of the picture captcha (useCaptcha)
export interface CaptchaAnswer {
  captchaId?: string
  captcha?: string
}

// A picture for one form. active: fetch it only from then on (a form on every page asks for one when
// someone starts filling it in). refresh() after every answer, the server takes each picture once
export function useCaptcha(active = true) {
  const key = useId()
  const query = useQuery({
    queryKey: ['captcha', key],
    queryFn: () => api('/api/captcha', Captcha),
    enabled: active,
    staleTime: Infinity,
    gcTime: 0,
    retry: false,
    refetchOnWindowFocus: false,
    refetchOnReconnect: false,
  })
  const captcha = query.data
  return {
    // Unknown until the first answer: a field is shown meanwhile, the server usually has it on
    enabled: captcha?.enabled ?? true,
    id: captcha?.enabled ? captcha.id : undefined,
    image: captcha?.enabled ? captcha : undefined,
    isLoading: query.isFetching,
    isError: query.isError,
    refresh: () => void query.refetch(),
  }
}

export interface RegisterInput extends CaptchaAnswer {
  username: string
  displayName: string
  email: string
  password: string
  locale: Lang
}

export function useRegister() {
  return useMutation({ mutationFn: (input: RegisterInput) => api('/api/auth/register', Ok, { method: 'POST', body: input }) })
}

export function useVerifyEmail() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (token: string) => api('/api/auth/verify-email', Ok, { method: 'POST', body: { token } }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.me }),
  })
}

export function useResendVerification() {
  return useMutation({ mutationFn: () => api('/api/auth/resend-verification', Ok, { method: 'POST' }) })
}

export function useForgotPassword() {
  return useMutation({
    mutationFn: (input: { email: string } & CaptchaAnswer) => api('/api/auth/forgot-password', Ok, { method: 'POST', body: input }),
  })
}

export function useResetPassword() {
  return useMutation({
    mutationFn: (input: { token: string; password: string }) => api('/api/auth/reset-password', Ok, { method: 'POST', body: input }),
  })
}

export function useUpdateProfile() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: { displayName?: string; locale?: Lang; replyMails?: boolean }) =>
      api('/api/auth/me', z.object({ user: Me.shape.user.unwrap() }), { method: 'PATCH', body: input }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.me }),
  })
}

export function useChangePassword() {
  const queryClient = useQueryClient()
  return useMutation({
    // No current password when the account has none yet (made by a GitHub or Google login)
    mutationFn: (input: { currentPassword?: string; newPassword: string }) =>
      api('/api/auth/change-password', Me, { method: 'POST', body: input }),
    onSuccess: (me) => storeMe(queryClient, me),
  })
}

// GitHub and Google: the ones set up on the server ([] when none)
export function useOAuthProviders() {
  return useQuery({ queryKey: queryKeys.oauthProviders, queryFn: () => api('/api/auth/providers', OAuthProviders), staleTime: Infinity })
}

// Leaves for the provider: a login, or with link a link to the logged in account. next: the page to
// come back to
export function useStartOAuth() {
  return useMutation({
    mutationFn: ({ provider, ...input }: { provider: string; lang: Lang; next?: string; link?: boolean }) =>
      api(`/api/auth/oauth/${provider}`, OAuthStart, { method: 'POST', body: input }),
    onSuccess: ({ url }) => window.location.assign(url),
  })
}

// The code of an account with two-step login, after its GitHub or Google login
export function useOAuthTotp() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (totp: string) => api('/api/auth/oauth-totp', OAuthTotpLogin, { method: 'POST', body: { totp } }),
    onSuccess: (me) => storeMe(queryClient, me),
  })
}

export function useIdentities(enabled = true) {
  return useQuery({ queryKey: queryKeys.identities, queryFn: () => api('/api/auth/identities', IdentityList), enabled })
}

// The account's picture: an uploaded one (squareAvatar's), a provider's, or none (its initials)
export function useSetAvatar() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (source: { picture: Blob } | { provider: string } | { remove: true }) => {
      const response = z.object({ user: Me.shape.user.unwrap() })
      if ('remove' in source) return api('/api/auth/avatar', response, { method: 'DELETE' })
      if ('provider' in source) return api(`/api/auth/avatar/${source.provider}`, response, { method: 'POST' })
      const form = new FormData()
      form.append('file', source.picture, source.picture.type === 'image/webp' ? 'avatar.webp' : 'avatar.png')
      return api('/api/auth/avatar', response, { method: 'POST', body: form })
    },
    onSuccess: ({ user }) => {
      queryClient.setQueryData<Me>(queryKeys.me, (me) => (me ? { ...me, user } : me))
      // The comments show it too
      void queryClient.invalidateQueries({ queryKey: ['comments'] })
    },
  })
}

export function useUnlinkIdentity() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (provider: string) => api(`/api/auth/identities/${provider}`, Ok, { method: 'DELETE' }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.identities }),
  })
}

export function useTotpSetup() {
  return useMutation({ mutationFn: () => api('/api/auth/totp/setup', TotpSetup, { method: 'POST' }) })
}

export function useTotpEnable() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (code: string) => api('/api/auth/totp/enable', Ok, { method: 'POST', body: { code } }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.me }),
  })
}

export function useTotpDisable() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (password: string) => api('/api/auth/totp/disable', Ok, { method: 'POST', body: { password } }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.me }),
  })
}

// Admin

export function useAdminPosts(status: string, page: number, enabled = true) {
  return useQuery({
    queryKey: queryKeys.adminPosts(status, page),
    queryFn: () => api(`/api/admin/posts?status=${encodeURIComponent(status)}&page=${page}`, AdminPostList),
    enabled,
  })
}

export function useAdminPost(id: number | null) {
  return useQuery({
    queryKey: queryKeys.adminPost(id ?? 0),
    queryFn: () => api(`/api/admin/posts/${id}`, AdminPost),
    enabled: id !== null,
  })
}

export interface PostInput {
  slug: string
  status: 'draft' | 'published'
  originalLang: Lang
  cover: string
  commentsEnabled: boolean
  tags: string[]
  // null removes a translation (updates only)
  translations: Partial<Record<Lang, { title: string; summary: string; body: string } | null>>
  // A draft published by the server then (ISO); null: not
  publishAt: string | null
}

export function useSavePost() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ id, input }: { id: number | null; input: PostInput }) =>
      id === null
        ? api('/api/admin/posts', AdminPost, { method: 'POST', body: input })
        : api(`/api/admin/posts/${id}`, AdminPost, { method: 'PATCH', body: input }),
    onSuccess: (post) => {
      queryClient.setQueryData(queryKeys.adminPost(post.id), post)
      void queryClient.invalidateQueries({ queryKey: ['admin', 'posts'] })
      void queryClient.invalidateQueries({ queryKey: ['posts'] })
      void queryClient.invalidateQueries({ queryKey: queryKeys.tags })
    },
  })
}

export function useDeletePost() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (id: number) => api(`/api/admin/posts/${id}`, Ok, { method: 'DELETE' }),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: ['admin', 'posts'] })
      void queryClient.invalidateQueries({ queryKey: ['posts'] })
      void queryClient.invalidateQueries({ queryKey: queryKeys.tags })
    },
  })
}

export function useTags() {
  return useQuery({ queryKey: queryKeys.tags, queryFn: async () => (await api('/api/tags', TagList)).tags })
}

export function useSaveTag() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ creating, tag }: { creating: boolean; tag: { slug: string; nameZh: string; nameEn: string } }) =>
      creating
        ? api('/api/admin/tags', Tag, { method: 'POST', body: tag })
        : api(`/api/admin/tags/${tag.slug}`, Tag, { method: 'PATCH', body: { nameZh: tag.nameZh, nameEn: tag.nameEn } }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.tags }),
  })
}

export function useDeleteTag() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (slug: string) => api(`/api/admin/tags/${slug}`, Ok, { method: 'DELETE' }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.tags }),
  })
}

// Comments, in the browser only: they change too often for the prerendered pages

export function useComments(target: CommentTarget) {
  return useInfiniteQuery({
    queryKey: queryKeys.comments(target),
    queryFn: ({ pageParam }) =>
      api(`/api/comments?target=${encodeURIComponent(target)}${pageParam ? `&cursor=${encodeURIComponent(pageParam)}` : ''}`, CommentPage),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.nextCursor,
  })
}

// The threads on passages of a post in a language, all of them (its markers need every one); under the
// comments' key, so whatever changes those changes these
export function useAnchoredComments(target: CommentTarget, lang: Lang, enabled = true) {
  return useQuery({
    queryKey: [...queryKeys.comments(target), 'anchored', lang],
    queryFn: () => api(`/api/comments/anchored?target=${encodeURIComponent(target)}&lang=${lang}`, CommentPage),
    enabled,
  })
}

export function useCreateComment(target: CommentTarget) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: { body: string; replyTo?: number; anchor?: CommentAnchorInput }) =>
      api('/api/comments', Comment, { method: 'POST', body: { target, ...input } }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.comments(target) }),
  })
}

// Its text, or its passage chosen again
export function useUpdateComment(target: CommentTarget) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ id, ...change }: { id: number; body?: string; anchor?: CommentAnchorInput }) =>
      api(`/api/comments/${id}`, Comment, { method: 'PATCH', body: change }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.comments(target) }),
  })
}

export function useDeleteComment(target: CommentTarget) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (id: number) => api(`/api/comments/${id}`, Ok, { method: 'DELETE' }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: queryKeys.comments(target) }),
  })
}

// In the page's language; allLangs in every one, each result in the language it was found in
export function useSearch(lang: Lang, query: string, allLangs = false) {
  return useQuery({
    queryKey: ['search', lang, query, allLangs],
    queryFn: () => api(`/api/posts/search?lang=${lang}&q=${encodeURIComponent(query)}${allLangs ? '&all=1' : ''}`, SearchResults),
    enabled: isSearchable(query),
    placeholderData: (previous) => previous,
  })
}

// Admin moderation

// passage: '' all; any, those on a passage of a post; orphaned, those whose passage their post lost
export function useAdminComments(status: string, passage: '' | 'any' | 'orphaned' = '') {
  return useInfiniteQuery({
    queryKey: ['admin', 'comments', status, passage],
    queryFn: ({ pageParam }) =>
      api(
        `/api/admin/comments?status=${encodeURIComponent(status)}&passage=${passage}${pageParam ? `&cursor=${pageParam}` : ''}`,
        AdminCommentList,
      ),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.nextCursor,
  })
}

// A post's versions, newest first, a page at a time
export function usePostRevisions(id: number | null) {
  return useInfiniteQuery({
    queryKey: ['admin', 'post-revisions', id],
    queryFn: ({ pageParam }) => api(`/api/admin/posts/${id}/revisions${pageParam ? `?before=${pageParam}` : ''}`, PostRevisions),
    initialPageParam: 0,
    getNextPageParam: (page) => (page.hasMore ? page.revisions.at(-1)?.id : undefined),
    enabled: id !== null,
  })
}

export function usePostRevision(id: number, revision: number | null) {
  return useQuery({
    queryKey: ['admin', 'post-revision', id, revision],
    queryFn: () => api(`/api/admin/posts/${id}/revisions/${revision}`, PostRevision),
    enabled: revision !== null,
    staleTime: Infinity,
  })
}

// Its working copy as a version, of each language changed since its last one
export function useSavePostVersion(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (note: string) => api(`/api/admin/posts/${id}/revisions`, PostRevisions, { method: 'POST', body: { note } }),
    onSuccess: () => void queryClient.invalidateQueries({ queryKey: ['admin', 'post-revisions', id] }),
  })
}

// A version back in the working copy; its editors' pages open it again (lib/collab.ts)
export function useRestorePostRevision(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (revision: number) => api(`/api/admin/posts/${id}/revisions/${revision}/restore`, AdminPost, { method: 'POST', body: {} }),
    onSuccess: (post) => queryClient.setQueryData(queryKeys.adminPost(post.id), post),
  })
}

// Who edits a post besides its owner; every change answers the whole list
export function usePostEditors(id: number | null) {
  return useQuery({
    queryKey: ['admin', 'post-editors', id],
    queryFn: () => api(`/api/admin/posts/${id}/editors`, PostEditors),
    enabled: id !== null,
  })
}

// A post's settings, its text left to its editors' working copy (lib/collab.ts); a language null is
// deleted, with its working copy
export type PostSettings = Partial<Omit<PostInput, 'translations'>> & { translations?: Partial<Record<Lang, null>> }

export function useSavePostSettings(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: PostSettings) => api(`/api/admin/posts/${id}`, AdminPost, { method: 'PATCH', body: input }),
    onSuccess: (post) => {
      queryClient.setQueryData(queryKeys.adminPost(post.id), post)
      void queryClient.invalidateQueries({ queryKey: ['admin', 'posts'] })
    },
  })
}

// What its editors wrote, to readers
export function usePublishPost(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: () => api(`/api/admin/posts/${id}/publish`, AdminPost, { method: 'POST', body: {} }),
    onSuccess: (post) => {
      queryClient.setQueryData(queryKeys.adminPost(post.id), post)
      void queryClient.invalidateQueries({ queryKey: ['admin', 'posts'] })
      void queryClient.invalidateQueries({ queryKey: ['posts'] })
      void queryClient.invalidateQueries({ queryKey: ['admin', 'post-revisions', id] })
      useLiveStore.getState().contentChanged(`post:${post.id}`)
    },
  })
}

// No longer asked to be published: the admin says no, or who asked takes it back
export function useWithdrawReview(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: () => api(`/api/admin/posts/${id}`, AdminPost, { method: 'PATCH', body: { reviewRequested: false } }),
    onSuccess: (post) => {
      queryClient.setQueryData(queryKeys.adminPost(post.id), post)
      void queryClient.invalidateQueries({ queryKey: ['admin', 'posts'] })
    },
  })
}

// The admin's: the post to another owner (a user name)
export function useTransferPost(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (owner: string) => api(`/api/admin/posts/${id}`, AdminPost, { method: 'PATCH', body: { owner } }),
    onSuccess: (post) => {
      queryClient.setQueryData(queryKeys.adminPost(post.id), post)
      void queryClient.invalidateQueries({ queryKey: ['admin', 'post-editors', id] })
      void queryClient.invalidateQueries({ queryKey: ['admin', 'posts'] })
      useLiveStore.getState().contentChanged(`post:${post.id}`)
    },
  })
}

export function useSetPostEditor(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: { username: string; rights: PostRight[] }) =>
      api(`/api/admin/posts/${id}/editors`, PostEditors, { method: 'PUT', body: input }),
    onSuccess: (editors) => {
      queryClient.setQueryData(['admin', 'post-editors', id], editors)
      void queryClient.invalidateQueries({ queryKey: queryKeys.adminPost(id) })
      useLiveStore.getState().contentChanged(`post:${id}`)
    },
  })
}

export function useRemovePostEditor(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (user: number) => api(`/api/admin/posts/${id}/editors/${user}`, PostEditors, { method: 'DELETE' }),
    onSuccess: (editors) => {
      queryClient.setQueryData(['admin', 'post-editors', id], editors)
      void queryClient.invalidateQueries({ queryKey: queryKeys.adminPost(id) })
      useLiveStore.getState().contentChanged(`post:${id}`)
    },
  })
}

export function useModerateComment() {
  const queryClient = useQueryClient()
  return useMutation({
    // Its status, or its lost passage left so (kept)
    mutationFn: ({ id, ...change }: { id: number; status?: 'visible' | 'hidden'; passage?: 'kept' }) =>
      api(`/api/admin/comments/${id}`, Comment, { method: 'PATCH', body: change }),
    onSuccess: (comment) => {
      void queryClient.invalidateQueries({ queryKey: ['admin', 'comments'] })
      void queryClient.invalidateQueries({ queryKey: queryKeys.comments(comment.target) })
    },
  })
}

// The list's filters, each off when empty
export type UserFilters = { q: string; role: '' | Role; status: '' | 'online' | 'banned' | 'deleted'; sort: '' | 'lastSeen'; page: number }

export function useAdminUsers(filters: UserFilters) {
  const params = new URLSearchParams(
    Object.entries(filters)
      .filter(([, value]) => value !== '')
      .map(([key, value]) => [key, String(value)]),
  )
  return useQuery({
    queryKey: ['admin', 'users', filters],
    queryFn: () => api(`/api/admin/users?${params}`, AdminUserList),
    placeholderData: (previous) => previous,
    // Who is online changes by itself
    refetchInterval: 30_000,
  })
}

export function useOnlineCounts() {
  return useQuery({ queryKey: ['admin', 'online'], queryFn: () => api('/api/admin/online', OnlineCounts), refetchInterval: 10_000 })
}

export function useAdminUser(id: number) {
  return useQuery({
    queryKey: ['admin', 'user', id],
    queryFn: () => api(`/api/admin/users/${id}`, AdminUserDetails),
    refetchInterval: 30_000,
  })
}

export type AccountInput = {
  username?: string
  displayName?: string
  email?: string
  emailVerified?: boolean
  locale?: Lang
  role?: Exclude<Role, 'admin'>
  denied?: DeniableAction[]
  password?: string
}

export function useCreateUser() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: AccountInput) => api('/api/admin/users', z.object({ id: z.number() }), { method: 'POST', body: input }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['admin', 'users'] }),
  })
}

export function useUpdateUser(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: AccountInput) => api(`/api/admin/users/${id}`, AdminUserDetails, { method: 'PATCH', body: input }),
    onSuccess: (user) => {
      queryClient.setQueryData(['admin', 'user', id], user)
      void queryClient.invalidateQueries({ queryKey: ['admin', 'users'] })
    },
  })
}

export function useDeleteUser() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ id, mode }: { id: number; mode: 'anonymize' | 'purge' }) =>
      api(`/api/admin/users/${id}?mode=${mode}`, Ok, { method: 'DELETE' }),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: ['admin', 'users'] })
      void queryClient.invalidateQueries({ queryKey: ['admin', 'comments'] })
    },
  })
}

// The admin's actions on one account's logins: a reset link, two-step login off, sessions ended
export function useUserSecurity(id: number) {
  const queryClient = useQueryClient()
  const refresh = () => queryClient.invalidateQueries({ queryKey: ['admin', 'user', id] })
  return {
    mailReset: useMutation({ mutationFn: () => api(`/api/admin/users/${id}/password-reset`, Ok, { method: 'POST', body: {} }) }),
    disableTotp: useMutation({ mutationFn: () => api(`/api/admin/users/${id}/totp`, Ok, { method: 'DELETE' }), onSuccess: refresh }),
    endSessions: useMutation({ mutationFn: () => api(`/api/admin/users/${id}/sessions`, Ok, { method: 'DELETE' }), onSuccess: refresh }),
    endSession: useMutation({
      mutationFn: (session: string) => api(`/api/admin/users/${id}/sessions/${session}`, Ok, { method: 'DELETE' }),
      onSuccess: refresh,
    }),
  }
}

export function useBanUser() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ id, days, reason }: { id: number; days: number | null; reason: string }) =>
      api(`/api/admin/users/${id}/ban`, z.object({ ok: z.literal(true) }).loose(), { method: 'POST', body: { days, reason } }),
    onSuccess: (_, { id }) => {
      void queryClient.invalidateQueries({ queryKey: ['admin', 'users'] })
      void queryClient.invalidateQueries({ queryKey: ['admin', 'user', id] })
      void queryClient.invalidateQueries({ queryKey: ['admin', 'comments'] })
    },
  })
}

export function useUnbanUser() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (id: number) => api(`/api/admin/users/${id}/ban`, Ok, { method: 'DELETE' }),
    onSuccess: (_, id) => {
      void queryClient.invalidateQueries({ queryKey: ['admin', 'users'] })
      void queryClient.invalidateQueries({ queryKey: ['admin', 'user', id] })
      void queryClient.invalidateQueries({ queryKey: ['admin', 'comments'] })
    },
  })
}

export function useIpBans() {
  return useQuery({ queryKey: ['admin', 'ip-bans'], queryFn: async () => (await api('/api/admin/ip-bans', IpBanList)).bans })
}

export function useAddIpBan() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: { ip: string; reason: string; days: number | null }) =>
      api('/api/admin/ip-bans', IpBan, { method: 'POST', body: input }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['admin', 'ip-bans'] }),
  })
}

export function useDeleteIpBan() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (ip: string) => api(`/api/admin/ip-bans/${encodeURIComponent(ip)}`, Ok, { method: 'DELETE' }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['admin', 'ip-bans'] }),
  })
}

// Multipart, so not through api()'s JSON body
export function useUpload(postId?: number) {
  return useMutation({ mutationFn: (file: File) => uploadFile(file, postId) })
}

// The site's profile, friends and projects

export function usePerson(username: string, lang: Lang) {
  return useQuery({
    queryKey: ['person', username, lang],
    queryFn: () => api(`/api/people/${encodeURIComponent(username)}?lang=${lang}`, Person),
  })
}

export function useAdminSite() {
  return useQuery({ queryKey: ['admin', 'site'], queryFn: () => api('/api/admin/site', AdminSite.partial()) })
}

export function useSaveSite() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (site: AdminSite) => api('/api/admin/site', AdminSite, { method: 'PUT', body: site }),
    onSuccess: (site) => queryClient.setQueryData(['admin', 'site'], site),
  })
}

export function useAdminFriends(status: string) {
  return useQuery({
    queryKey: ['admin', 'friends', status],
    queryFn: async () => (await api(`/api/admin/friends?status=${encodeURIComponent(status)}`, AdminFriendList)).friends,
  })
}

export interface FriendInput {
  name: string
  url: string
  avatar: string
  description: string
  sort?: number
}

export function useSaveFriend() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ id, input }: { id: number | null; input: FriendInput }) =>
      id === null
        ? api('/api/admin/friends', AdminFriend, { method: 'POST', body: input })
        : api(`/api/admin/friends/${id}`, AdminFriend, { method: 'PATCH', body: input }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['admin', 'friends'] }),
  })
}

export function useReviewFriend() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ id, status, rejectReason }: { id: number; status: 'approved' | 'rejected'; rejectReason?: string }) =>
      api(`/api/admin/friends/${id}`, AdminFriend, { method: 'PATCH', body: { status, rejectReason: rejectReason ?? '' } }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['admin', 'friends'] }),
  })
}

export function useDeleteFriend() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (id: number) => api(`/api/admin/friends/${id}`, Ok, { method: 'DELETE' }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['admin', 'friends'] }),
  })
}

export function useApplyFriend() {
  return useMutation({
    mutationFn: (input: FriendInput & { message: string }) =>
      api('/api/friends', z.object({ id: z.number(), status: z.string() }), { method: 'POST', body: input }),
  })
}

export function useProjects() {
  return useQuery({ queryKey: ['projects'], queryFn: async () => (await api('/api/projects', ProjectList)).projects })
}

export type ProjectInput = Omit<Project, 'id'>

export function useSaveProject() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ id, input }: { id: number | null; input: ProjectInput }) =>
      id === null
        ? api('/api/admin/projects', Project, { method: 'POST', body: input })
        : api(`/api/admin/projects/${id}`, Project, { method: 'PATCH', body: input }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['projects'] }),
  })
}

export function useDeleteProject() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (id: number) => api(`/api/admin/projects/${id}`, Ok, { method: 'DELETE' }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['projects'] }),
  })
}

// Reactions of several targets in one request (the API takes 100 at most, more are split): a list of
// comments asks for all of them at once
export function useReactions(targets: string[]) {
  const sorted = [...targets].sort()
  return useQuery({
    queryKey: ['reactions', ...sorted],
    queryFn: async () => {
      const chunks = []
      for (let i = 0; i < sorted.length; i += 100) chunks.push(sorted.slice(i, i + 100))
      const pages = await Promise.all(
        chunks.map((chunk) => api(`/api/reactions?targets=${encodeURIComponent(chunk.join(','))}`, ReactionList)),
      )
      return Object.assign({}, ...pages.map((page) => page.reactions)) as Record<string, ReactionState>
    },
    enabled: sorted.length > 0,
    staleTime: 60_000,
  })
}

export function useReact() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: { target: string; kind: ReactionKind; on: boolean }) =>
      api('/api/reactions', ReactionState, { method: 'POST', body: input }),
    // Every list that has the target gets its new state
    onSuccess: (state, { target }) =>
      queryClient.setQueriesData<Record<string, ReactionState>>({ queryKey: ['reactions'] }, (old) =>
        old && target in old ? { ...old, [target]: state } : old,
      ),
  })
}

// Mail subscriptions

export function useSubscribe() {
  return useMutation({
    mutationFn: (input: { email: string; lang: Lang } & CaptchaAnswer) => api('/api/subscribe', Ok, { method: 'POST', body: input }),
  })
}

export function useConfirmSubscription() {
  return useMutation({ mutationFn: (token: string) => api('/api/subscribe/confirm', Ok, { method: 'POST', body: { token } }) })
}

export function useUnsubscribe() {
  return useMutation({ mutationFn: (token: string) => api('/api/unsubscribe', Ok, { method: 'POST', body: { token } }) })
}

const Subscription = z.object({ subscribed: z.boolean() })

// The status page's: the day once, then every 10 seconds what came since
export function useServerStatus() {
  const queryClient = useQueryClient()
  return useQuery({
    queryKey: ['status'],
    queryFn: async () => {
      const before = queryClient.getQueryData<ServerStatus>(['status'])
      const since = before?.series.at.at(-1)
      return mergeStatus(before, await api(`/api/status${since ? `?since=${since}` : ''}`, ServerStatus))
    },
    refetchInterval: 10_000,
  })
}

export function useSubscription(enabled: boolean) {
  return useQuery({ queryKey: ['subscription'], queryFn: () => api('/api/auth/subscription', Subscription), enabled })
}

export function useSetSubscription() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (subscribed: boolean) => api('/api/auth/subscription', Subscription, { method: 'PUT', body: { subscribed } }),
    onSuccess: (state) => queryClient.setQueryData(['subscription'], state),
  })
}

export function useAdminStats(days: number) {
  return useQuery({ queryKey: ['admin', 'stats', days], queryFn: () => api(`/api/admin/stats?days=${days}`, AdminStats) })
}

export function useAdminDrives() {
  return useQuery({ queryKey: ['admin', 'drives'], queryFn: () => api('/api/admin/drives', AdminDrives) })
}

// User friendships (social) between readers

export function useUserFriends(enabled = true) {
  return useQuery({
    queryKey: ['user', 'friends'],
    queryFn: () => api('/api/user/friends', UserFriendsList),
    enabled,
  })
}

export function useUserFriendRequests(enabled = true) {
  return useQuery({
    queryKey: ['user', 'friends', 'requests'],
    queryFn: () => api('/api/user/friends/requests', UserFriendRequests),
    enabled,
  })
}

export function useUserFriendshipStatus(username: string, enabled = true) {
  return useQuery({
    queryKey: ['user', 'friends', 'status', username],
    queryFn: () => api(`/api/user/friends/status?username=${encodeURIComponent(username)}`, UserFriendshipStatus),
    enabled: enabled && !!username,
  })
}

export function useSendFriendRequest() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ to, message }: { to: string; message?: string }) =>
      api('/api/user/friends/requests', Ok, { method: 'POST', body: { to, message: message ?? '' } }),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: ['user', 'friends'] })
    },
  })
}

export function useAcceptFriendRequest() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (requestId: number) => api(`/api/user/friends/requests/${requestId}/accept`, Ok, { method: 'POST' }),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: ['user', 'friends'] })
    },
  })
}

export function useRejectFriendRequest() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (requestId: number) => api(`/api/user/friends/requests/${requestId}`, Ok, { method: 'DELETE' }),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: ['user', 'friends'] })
    },
  })
}

export function useRemoveFriend() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (friendId: number) => api(`/api/user/friends/${friendId}`, Ok, { method: 'DELETE' }),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: ['user', 'friends'] })
    },
  })
}

// Chat system (Square & Direct Friend chats)

export function useChatChannels(enabled = true) {
  return useQuery({
    queryKey: ['chat', 'channels'],
    queryFn: () => api('/api/chat/channels', ChatChannelsList),
    enabled,
    // The WebSocket tells about new messages and channels, this only catches what a reconnect missed
    refetchInterval: 30_000,
  })
}

// The latest messages of a channel, kept current by the WebSocket (components/live.tsx)
export function useChatMessages(channelId: string | null, enabled = true) {
  return useQuery({
    queryKey: ['chat', 'messages', channelId, undefined],
    queryFn: () => api(`/api/chat/channels/${encodeURIComponent(channelId!)}/messages`, ChatMessagesList),
    enabled: enabled && !!channelId,
  })
}

// Older ones, for "load older": kept while the channel is open (gone with it), under the channel's key
// so that a moderator's removal reaches them too (components/live.tsx)
export function useOlderChatMessages(channelId: string) {
  const queryClient = useQueryClient()
  const queryKey = ['chat', 'messages', channelId, 'older']
  const query = useQuery<ChatMessagesList | null>({ queryKey, queryFn: () => null, enabled: false, initialData: null, gcTime: 0 })
  const load = async (before: number) => {
    const page = await api(`/api/chat/channels/${encodeURIComponent(channelId)}/messages?before=${before}`, ChatMessagesList)
    queryClient.setQueryData<ChatMessagesList | null>(queryKey, (old) => ({
      messages: [...page.messages, ...(old?.messages ?? [])],
      hasMore: page.hasMore,
    }))
  }
  return { older: query.data, load }
}

// Messages a moderator removed: one, or the channel's all, from every page of it held
export function removeChatMessages(queryClient: QueryClient, channelId: string, messageId?: number) {
  queryClient.setQueriesData<ChatMessagesList | null>({ queryKey: ['chat', 'messages', channelId] }, (old) => {
    if (!old) return old
    return messageId === undefined ? { messages: [], hasMore: false } : { ...old, messages: old.messages.filter((m) => m.id !== messageId) }
  })
  void queryClient.invalidateQueries({ queryKey: ['chat', 'channels'] })
  void queryClient.invalidateQueries({ queryKey: ['chat', 'unread'] })
}

// Moderators: the square's and groups' messages (ChatHandlers.cpp, never a direct chat's)
export function useDeleteChatMessage() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (message: ChatMessageItem) => api(`/api/admin/chat/messages/${message.id}`, ChatRemoved, { method: 'DELETE' }),
    onSuccess: (_removed, message) => removeChatMessages(queryClient, message.channelId, message.id),
  })
}

export function useClearChatChannel() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (channelId: string) =>
      api(`/api/admin/chat/channels/${encodeURIComponent(channelId)}/messages`, ChatRemoved, { method: 'DELETE' }),
    onSuccess: (_removed, channelId) => removeChatMessages(queryClient, channelId),
  })
}

export function useDissolveChatGroup() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (groupId: number) => api(`/api/admin/chat/groups/${groupId}`, ChatRemoved, { method: 'DELETE' }),
    onSuccess: () => void queryClient.invalidateQueries({ queryKey: ['chat'] }),
  })
}

// A message being sent shows at once with a negative id (the server's are positive), replaced by the
// server's copy when the answer or the WebSocket's chat_message comes, whichever is first
let optimisticId = 0

export function useSendChatMessage() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ channelId, content }: { channelId: string; content: string }) =>
      api(`/api/chat/channels/${encodeURIComponent(channelId)}/messages`, ChatMessageItem, {
        method: 'POST',
        body: { channelId, content },
      }),
    onMutate: async ({ channelId, content }) => {
      await queryClient.cancelQueries({ queryKey: ['chat', 'messages', channelId] })
      const user = queryClient.getQueryData<Me>(queryKeys.me)?.user
      if (!user) return { tempId: 0 }
      const optimisticMsg: ChatMessageItem = {
        id: --optimisticId,
        channelId,
        senderId: user.id,
        senderName: user.displayName,
        senderAvatar: user.avatar ?? null,
        content,
        createdAt: new Date().toISOString(),
      }
      queryClient.setQueryData<ChatMessagesList>(['chat', 'messages', channelId, undefined], (old) => {
        if (!old) return old
        return { ...old, messages: [...old.messages, optimisticMsg] }
      })
      return { tempId: optimisticMsg.id }
    },
    onSuccess: (realMsg, { channelId }, context) => {
      queryClient.setQueryData<ChatMessagesList>(['chat', 'messages', channelId, undefined], (old) => {
        if (!old) return old
        const others = old.messages.filter((m) => m.id !== realMsg.id && m.id !== context?.tempId)
        return { ...old, messages: [...others, realMsg].sort(sortChatMessages) }
      })
      queryClient.setQueryData<ChatChannelsList>(['chat', 'channels'], (old) => {
        if (!old) return old
        return {
          ...old,
          channels: old.channels.map((ch) =>
            ch.id === channelId && (!ch.lastMessage || ch.lastMessage.id < realMsg.id) ? { ...ch, lastMessage: realMsg } : ch,
          ),
        }
      })
    },
    onError: (_err, { channelId }, context) => {
      // Only the failed message goes, not what arrived meanwhile
      queryClient.setQueryData<ChatMessagesList>(['chat', 'messages', channelId, undefined], (old) =>
        old ? { ...old, messages: old.messages.filter((m) => m.id !== context?.tempId) } : old,
      )
    },
  })
}

// Server messages in id order, the ones still being sent (negative ids) after them in the order sent
export function sortChatMessages(a: ChatMessageItem, b: ChatMessageItem) {
  if (a.id > 0 && b.id > 0) return a.id - b.id
  if (a.id > 0) return -1
  if (b.id > 0) return 1
  return b.id - a.id
}

export function useMarkChatRead() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ channelId, lastReadId }: { channelId: string; lastReadId: number }) =>
      api(`/api/chat/channels/${encodeURIComponent(channelId)}/read`, Ok, {
        method: 'POST',
        body: { channelId, lastReadId },
      }),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: ['chat', 'channels'] })
      void queryClient.invalidateQueries({ queryKey: ['chat', 'unread'] })
    },
  })
}

export function useChatUnreadCount(enabled = true) {
  return useQuery({
    queryKey: ['chat', 'unread'],
    queryFn: () => api('/api/chat/unread', ChatUnreadCount),
    enabled,
    refetchInterval: 30_000,
  })
}

export function useCreateChatGroup() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: ({ name, memberIds }: { name: string; memberIds: number[] }) =>
      api('/api/chat/groups', ChatChannelItem, {
        method: 'POST',
        body: { name, memberIds },
      }),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: ['chat', 'channels'] })
    },
  })
}

export function useChatGroupMembers(groupId: number | null, enabled = true) {
  return useQuery({
    queryKey: ['chat', 'groups', groupId, 'members'],
    queryFn: () => api(`/api/chat/groups/${groupId}/members`, ChatGroupMembersList),
    enabled: enabled && !!groupId,
  })
}

export function useLeaveChatGroup() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (groupId: number) =>
      api(`/api/chat/groups/${groupId}/leave`, Ok, {
        method: 'POST',
      }),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: ['chat', 'channels'] })
    },
  })
}

export function useSetUserDrive(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: { enabled: boolean; quota: number | null; trashDays: number | null }) =>
      api(`/api/admin/users/${id}/drive`, AdminUserDetails, { method: 'PUT', body: input }),
    onSuccess: (user) => queryClient.setQueryData(['admin', 'user', id], user),
  })
}

export function useSetUserWrite(id: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (enabled: boolean) => api(`/api/admin/users/${id}/write`, AdminUserDetails, { method: 'PUT', body: { enabled } }),
    onSuccess: (user) => queryClient.setQueryData(['admin', 'user', id], user),
  })
}

// A drive (lib/drive.ts): the account's own, or for the admin another's (owner: its id). Every change
// refreshes the folder shown and the usage

export function useDrive(folder: number | null, owner?: number) {
  return useQuery({
    queryKey: ['drive', owner ?? 'own', 'list', folder],
    queryFn: () => api(drivePath(`/api/drive${folder ? `?folder=${folder}` : ''}`, owner), DriveListing),
  })
}

export function useDriveFolders(enabled: boolean, owner?: number) {
  return useQuery({
    queryKey: ['drive', owner ?? 'own', 'folders'],
    queryFn: async () => (await api(drivePath('/api/drive/folders', owner), DriveFolders)).folders,
    enabled,
  })
}

export function useDriveChanges(owner?: number) {
  const queryClient = useQueryClient()
  const refresh = () => queryClient.invalidateQueries({ queryKey: ['drive', owner ?? 'own'] })
  return {
    refresh,
    createFolder: useMutation({
      mutationFn: (input: { parentId: number | null; name: string }) =>
        api(drivePath('/api/drive/folders', owner), DriveFolder, { method: 'POST', body: input }),
      onSuccess: refresh,
    }),
    updateFolder: useMutation({
      mutationFn: ({ id, ...input }: { id: number; name?: string; parentId?: number | null }) =>
        api(drivePath(`/api/drive/folders/${id}`, owner), DriveFolder, { method: 'PATCH', body: input }),
      onSuccess: refresh,
    }),
    deleteFolder: useMutation({
      mutationFn: (id: number) => api(drivePath(`/api/drive/folders/${id}`, owner), DriveDeleted, { method: 'DELETE' }),
      onSuccess: refresh,
    }),
    updateFile: useMutation({
      mutationFn: ({ id, ...input }: { id: number; name?: string; folderId?: number | null }) =>
        api(drivePath(`/api/drive/files/${id}`, owner), DriveFile, { method: 'PATCH', body: input }),
      onSuccess: refresh,
    }),
    deleteFile: useMutation({
      mutationFn: (id: number) => api(drivePath(`/api/drive/files/${id}`, owner), DriveDeleted, { method: 'DELETE' }),
      onSuccess: refresh,
    }),
    restore: useMutation({
      mutationFn: ({ kind, id }: { kind: 'file' | 'folder'; id: number }) =>
        api(drivePath(`/api/drive/trash/${kind}/${id}/restore`, owner), z.object({ name: z.string() }), { method: 'POST' }),
      onSuccess: refresh,
    }),
    purge: useMutation({
      mutationFn: ({ kind, id }: { kind: 'file' | 'folder'; id: number }) =>
        api(drivePath(`/api/drive/trash/${kind}/${id}`, owner), Ok, { method: 'DELETE' }),
      onSuccess: refresh,
    }),
    emptyTrash: useMutation({
      mutationFn: () => api(drivePath('/api/drive/trash', owner), Ok, { method: 'DELETE' }),
      onSuccess: refresh,
    }),
  }
}

export function useDriveTrash(owner?: number) {
  return useQuery({
    queryKey: ['drive', owner ?? 'own', 'trash'],
    queryFn: () => api(drivePath('/api/drive/trash', owner), DriveTrash),
  })
}

export function useDriveShares(owner?: number) {
  return useQuery({
    queryKey: ['drive', owner ?? 'own', 'shares'],
    queryFn: async () => (await api(drivePath('/api/drive/shares', owner), DriveShares)).shares,
  })
}

export function useCreateShare(owner?: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (input: { fileId?: number; folderId?: number; expires: ShareExpiry; password?: string }) =>
      api(drivePath('/api/drive/shares', owner), DriveShare, { method: 'POST', body: input }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['drive', owner ?? 'own', 'shares'] }),
  })
}

export function useDeleteShare(owner?: number) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (id: number) => api(drivePath(`/api/drive/shares/${id}`, owner), Ok, { method: 'DELETE' }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['drive', owner ?? 'own', 'shares'] }),
  })
}

export function useShareAccess(id: number, owner?: number) {
  return useQuery({
    queryKey: ['drive', owner ?? 'own', 'shares', id, 'access'],
    queryFn: async () => (await api(drivePath(`/api/drive/shares/${id}/access`, owner), ShareAccess)).access,
  })
}

// A link someone gave: what it shows, in its folder (null: the link's own)
export function useShare(token: string, folder: number | null, enabled: boolean) {
  return useQuery({
    queryKey: ['share', token, folder],
    queryFn: () => api(`/api/shares/${token}${folder ? `?folder=${folder}` : ''}`, ShareView),
    enabled,
    retry: false,
  })
}

export function useUnlockShare(token: string) {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (password: string) => api(`/api/shares/${token}/unlock`, Ok, { method: 'POST', body: { password } }),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['share', token] }),
  })
}
