#!/usr/bin/env bash
# Create the local database if needed and apply numbered schema migrations in order.
set -euo pipefail

DB=${DB:-vantagepulse}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

createdb "$DB" 2>/dev/null || true
for migration in "$ROOT"/sql/*.sql; do
    [ -f "$migration" ] || continue
    printf 'Applying %s\n' "${migration##*/}"
    psql "$DB" -v ON_ERROR_STOP=1 -f "$migration"
done
echo "schema migrations applied to $DB"

