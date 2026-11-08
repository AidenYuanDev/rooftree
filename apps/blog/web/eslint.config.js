import js from '@eslint/js'
import { defineConfig, globalIgnores } from 'eslint/config'
import reactHooks from 'eslint-plugin-react-hooks'
import reactRefresh from 'eslint-plugin-react-refresh'
import globals from 'globals'
import tseslint from 'typescript-eslint'

export default defineConfig([
  globalIgnores(['build', '.react-router', 'playwright-report', 'test-results']),
  {
    files: ['**/*.{ts,tsx}'],
    extends: [
      js.configs.recommended,
      tseslint.configs.recommended,
      reactHooks.configs.flat['recommended-latest'],
      reactRefresh.configs.vite,
    ],
    languageOptions: {
      globals: { ...globals.browser, ...globals.node },
    },
    rules: {
      // Route modules export these next to the component, React Router handles their HMR
      'react-refresh/only-export-components': [
        'warn',
        { allowExportNames: ['meta', 'links', 'loader', 'clientLoader', 'HydrateFallback', 'ErrorBoundary', 'Layout'] },
      ],
      '@typescript-eslint/no-unused-vars': ['error', { argsIgnorePattern: '^_', varsIgnorePattern: '^_' }],
    },
  },
  {
    // shadcn/ui's generated components export their variants next to them
    files: ['app/components/ui/**'],
    rules: { 'react-refresh/only-export-components': 'off' },
  },
])
