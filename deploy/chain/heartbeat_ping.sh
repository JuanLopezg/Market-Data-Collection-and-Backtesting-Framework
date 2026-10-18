#!/bin/sh
# One "VPS alive" ping to healthchecks.io, run by algotrading-heartbeat.service every minute.
# The credential (secrets/heartbeat.curl) may hold the bare ping URL or a curl config line
# url = "https://hc-ping.com/<uuid>". The URL reaches curl on stdin (printf is a shell
# builtin), so it never appears in a command line, the environment or the logs.
set -eu
file="${CREDENTIALS_DIRECTORY:?run by algotrading-heartbeat.service}/heartbeat"

# First non-empty line, without CR, an optional `url =` prefix and optional quotes.
url=$(sed -n '/[^[:space:]]/{p;q;}' "$file" | tr -d '\r' \
    | sed -E 's/^[[:space:]]*(url[[:space:]]*=[[:space:]]*)?"?//; s/"?[[:space:]]*$//')
case "$url" in
    https://*) ;;
    *) echo "heartbeat: the credential holds no https:// ping URL" >&2; exit 2 ;;
esac

printf 'url = "%s"\n' "$url" \
    | /usr/bin/curl --config - --fail --silent --show-error --max-time 10 --retry 2 --output /dev/null
