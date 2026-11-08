import { setupServer } from 'msw/node'

// Every test adds the handlers it needs with server.use(); an unhandled request fails the test
export const server = setupServer()
