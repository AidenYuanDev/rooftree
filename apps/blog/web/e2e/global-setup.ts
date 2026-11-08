import { writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { startServer } from '../../tests/server.mjs'

export const stateFile = join(import.meta.dirname, '.state.json')

export default async function globalSetup() {
  const server = await startServer({ origins: ['http://127.0.0.1:5174'] })
  writeFileSync(stateFile, JSON.stringify({ adminTotpSecret: server.adminTotpSecret, outbox: server.outbox }))
  return async () => {
    await server.stop()
  }
}
