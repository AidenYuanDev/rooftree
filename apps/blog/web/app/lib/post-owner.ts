import { createContext } from 'react'

// The owner of the post the comments are on (components/article): it hides and deletes them, like a
// moderator, there only
export const PostOwnerContext = createContext<number | null>(null)
