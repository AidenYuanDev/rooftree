#!/bin/sh
# The image's entry point, blog.service's two steps: the migrations, then the server (in place of this
# shell, so docker kill -s HUP reaches it)
set -e

blog=/opt/blog/src/apps/blog
# The schema first: the server refuses to start while a migration is missing
"$blog/node_modules/.bin/dbmate" --migrations-dir "$blog/db/migrations" --no-dump-schema --wait up
exec /opt/blog/bin/blog --config /etc/blog/blog.yaml "$@"
