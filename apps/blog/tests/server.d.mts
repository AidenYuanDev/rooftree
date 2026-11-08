// Types of server.mjs, for the TypeScript e2e tests in web/

export const origin: string
export const baseUrl: string
export const wsUrl: string
export const adminPassword: string

export function totp(base32Secret: string, offset?: number): string

export interface TestServer {
  adminTotpSecret: string
  log: string[]
  outbox: string
  rebuildsFile: string
  console(...lines: string[]): string
  stop(): Promise<void>
}

export function startServer(options?: { origins?: string[]; captcha?: boolean; mailInterval?: number; oauth?: string }): Promise<TestServer>

export function lastMailTo(outbox: string, address: string): { mail: string; text: string; token: string | undefined } | undefined
