#!/usr/bin/env bash
# Rebuilds the prerendered site and puts it live, for Blog.Rebuild.Command (the API server runs it
# after a published post changed) and for deploying by hand.
#
#   1. builds apps/blog/web against the running API (react-router build prerenders every post)
#   2. checks the result: the pages that must exist, code highlighted in every post
#   3. compresses the text files next to them (name.gz), the server answers those to browsers that
#      take gzip and never compresses while serving
#   4. copies it to $SITE_DIR/releases/<time> and swaps the $SITE_DIR/current symlink to it, in one
#      rename, so a release is never seen half copied
#   5. keeps the last $KEEP releases
#
# Anything failing leaves current as it was and exits non-zero, which the server logs. When the server
# ran it, it loads the new release into memory afterwards; after a run by hand, reload it
# (systemctl reload blog).
#
# Environment (defaults for the layout of README.md):
#   BLOG_WEB_DIR        apps/blog/web of a checkout with its node_modules installed
#   SITE_DIR            /srv/blog/site
#   BLOG_API_URL        http://127.0.0.1:8100
#   BLOG_PUBLIC_ORIGIN  https://blog.example.com
#   KEEP                5

set -euo pipefail

BLOG_WEB_DIR=${BLOG_WEB_DIR:-$(cd "$(dirname "$0")/../web" && pwd)}
SITE_DIR=${SITE_DIR:-/srv/blog/site}
KEEP=${KEEP:-5}
export BLOG_API_URL=${BLOG_API_URL:-http://127.0.0.1:8100}
export BLOG_PUBLIC_ORIGIN=${BLOG_PUBLIC_ORIGIN:-https://blog.example.com}

mkdir -p "$SITE_DIR/releases"

# One build at a time: the server never starts two, a deploy by hand might
exec 9>"$SITE_DIR/.rebuild.lock"
flock -w 600 9

cd "$BLOG_WEB_DIR"
rm -rf build
pnpm exec react-router build

client=build/client
fail() {
  echo "rebuild.sh: $*, the live site stays as it is" >&2
  exit 1
}

for page in __spa-fallback.html zh/index.html en/index.html; do
  [[ -f $client/$page ]] || fail "the build has no $page"
done

# Posts are highlighted at build time; a code block without Shiki's markup means highlighting broke
while IFS= read -r -d '' page; do
  if grep -q '<pre><code' "$page"; then
    fail "$page has a code block that wasn't highlighted"
  fi
done < <(find "$client" -path '*/posts/*/index.html' -print0)

# -n: no name and time in the header, the same file compresses to the same bytes
find "$client" -type f \( -name '*.html' -o -name '*.js' -o -name '*.css' -o -name '*.svg' -o -name '*.data' \
  -o -name '*.json' -o -name '*.xml' -o -name '*.txt' \) -size +1k -exec gzip -k -9 -n {} +

release="$SITE_DIR/releases/$(date -u +%Y%m%dT%H%M%SZ)"
cp -a "$client" "$release"

# rename(2) over the old link: atomic
ln -sfn "$release" "$SITE_DIR/current.new"
mv -T "$SITE_DIR/current.new" "$SITE_DIR/current"
echo "rebuild.sh: $release is live"

# Old releases, oldest first, all but the newest $KEEP
mapfile -t releases < <(find "$SITE_DIR/releases" -mindepth 1 -maxdepth 1 -type d | sort)
for ((i = 0; i < ${#releases[@]} - KEEP; ++i)); do
  rm -rf "${releases[i]}"
done
