// Client-side checks matching the server's (apps/blog/server/Validation.cpp), so most mistakes show
// before a request. The server checks again either way
import { z } from 'zod'
import { Lang } from './schemas'

const characters = (text: string) => [...text].length

export const username = z
  .string()
  .trim()
  .min(1, 'required')
  .regex(/^[A-Za-z0-9_]{3,20}$/, 'invalid')
export const displayName = z
  .string()
  .trim()
  .min(1, 'required')
  .refine((v) => characters(v) <= 40, 'too_long')
export const email = z.string().trim().min(1, 'required').max(254, 'too_long').email('invalid')
export const password = z
  .string()
  .refine((v) => characters(v) >= 8, 'too_short')
  .refine((v) => characters(v) <= 128, 'too_long')
export const slug = (max: number) =>
  z
    .string()
    .trim()
    .min(1, 'required')
    .max(max, 'too_long')
    .regex(/^[a-z0-9]+(?:-[a-z0-9]+)*$/, 'invalid')

export const tagForm = z.object({
  slug: slug(50),
  nameZh: z.string().trim().min(1, 'required').max(50, 'too_long'),
  nameEn: z.string().trim().min(1, 'required').max(50, 'too_long'),
})

// The characters of the picture; the server says when they are missing (it may have the captcha off)
export const captcha = z.string().trim().max(16, 'too_long')

export const registerForm = z.object({ username, displayName, email, password, locale: Lang, captcha })
export const loginForm = z.object({
  login: z.string().trim().min(1, 'required'),
  password: z.string().min(1, 'required'),
  totp: z.string().trim().optional(),
})
export const forgotForm = z.object({ email, captcha })
export const resetForm = z.object({ password })
export const profileForm = z.object({ displayName, locale: Lang })
export const changePasswordForm = z.object({ currentPassword: z.string().min(1, 'required'), newPassword: password })
// The first password of an account made by a GitHub or Google login: the same form, nothing current
export const setPasswordForm = z.object({ currentPassword: z.string(), newPassword: password })
export const totpCodeForm = z.object({
  code: z
    .string()
    .trim()
    .regex(/^\d{6}$/, 'invalid'),
})
export const passwordOnlyForm = z.object({ password: z.string().min(1, 'required') })
