#!/usr/bin/env bash
# Daily backup (blog-backup.timer): the database as one consistent dump, the uploads as a tarball,
# into $BACKUP_DIR; keeps $KEEP_DAYS days. Copy $BACKUP_DIR somewhere else too, a backup on the same
# disk only helps against mistakes.
#
# Environment: DATABASE_URL (blog.env), BACKUP_DIR (/srv/blog/backups), UPLOAD_DIR (/srv/blog/uploads),
# KEEP_DAYS (14)

set -euo pipefail

BACKUP_DIR=${BACKUP_DIR:-/srv/blog/backups}
UPLOAD_DIR=${UPLOAD_DIR:-/srv/blog/uploads}
KEEP_DAYS=${KEEP_DAYS:-14}

mkdir -p "$BACKUP_DIR"
stamp=$(date -u +%Y%m%dT%H%M%SZ)

# A consistent snapshot, without locking anyone out. DATABASE_URL is dbmate's, pg_dump takes it as it is
pg_dump --no-owner --no-privileges "$DATABASE_URL" | gzip > "$BACKUP_DIR/blog-$stamp.sql.gz.part"
mv "$BACKUP_DIR/blog-$stamp.sql.gz.part" "$BACKUP_DIR/blog-$stamp.sql.gz"

if [[ -d $UPLOAD_DIR ]]; then
  tar -czf "$BACKUP_DIR/uploads-$stamp.tar.gz.part" -C "$(dirname "$UPLOAD_DIR")" "$(basename "$UPLOAD_DIR")"
  mv "$BACKUP_DIR/uploads-$stamp.tar.gz.part" "$BACKUP_DIR/uploads-$stamp.tar.gz"
fi

find "$BACKUP_DIR" -maxdepth 1 -name 'blog-*.sql.gz' -mtime +"$KEEP_DAYS" -delete
find "$BACKUP_DIR" -maxdepth 1 -name 'uploads-*.tar.gz' -mtime +"$KEEP_DAYS" -delete
echo "backup.sh: $BACKUP_DIR/blog-$stamp.sql.gz"
