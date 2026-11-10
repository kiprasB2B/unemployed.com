#!/usr/bin/env bash
# Integration tests for the static server.
# Usage: tests/server_test.sh [path/to/server]   (needs bash, curl, timeout)
set -u

BIN=$(realpath "${1:-build/server}")
REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
PIDS=()
PASS=0
FAIL=0

cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  wait 2>/dev/null
  rm -rf -- "$TMP"
}
trap cleanup EXIT

check() { # name expected actual
  if [[ "$2" == "$3" ]]; then PASS=$((PASS + 1)); echo "PASS $1"
  else FAIL=$((FAIL + 1)); echo "FAIL $1 — expected [$2], got [$3]"; fi
}

free_port() {
  local p
  for _ in $(seq 1 50); do
    p=$((20000 + RANDOM % 20000))
    (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null || { echo "$p"; return; }
  done
  echo 18080
}

start() { # root port  -> starts a server in the background
  HOST=127.0.0.1 PORT="$2" "$BIN" "$1" 2>>"$TMP/server.log" &
  PIDS+=($!)
  for _ in $(seq 1 100); do
    curl -fs -o /dev/null "http://127.0.0.1:$2/healthz" && return 0
    sleep 0.05
  done
  echo "server did not start on port $2"; cat "$TMP/server.log"; exit 1
}

# ---------- fixture ----------
R="$TMP/root"
mkdir -p "$R/sub" "$R/nodir"
printf '<!doctype html><h1>fixture</h1>\n' > "$R/index.html"
printf '<h1>sub</h1>\n' > "$R/sub/index.html"
printf 'body{color:red}\n' > "$R/style.css"
printf 'spaced\n' > "$R/a b.txt"
printf 'hidden\n' > "$R/.secret"
printf 'TOPSECRET\n' > "$TMP/outside.txt"
ln -s ../outside.txt "$R/leak"
ln -s "$TMP" "$R/leakdir"
ln -s style.css "$R/inlink.css"

PORT=$(free_port)
B="http://127.0.0.1:$PORT"
start "$R" "$PORT"

code() { curl -s -o /dev/null -w '%{http_code}' --path-as-is "$@"; }
hdr()  { curl -s -D - -o /dev/null --path-as-is "${@:2}" | tr -d '\r' | grep -i "^$1:" | head -1 | cut -d' ' -f2-; }
raw()  { # raw request bytes -> status code of the response
  { exec 3<>"/dev/tcp/127.0.0.1/$PORT"; printf '%b' "$1" >&3; timeout 3 cat <&3; exec 3>&-; } 2>/dev/null \
    | head -1 | cut -d' ' -f2
}

# ---------- basic serving ----------
check "GET / → 200"                     200 "$(code "$B/")"
check "GET / body = index.html"         "$(cat "$R/index.html")" "$(curl -s "$B/")"
check "index Content-Type"              "text/html; charset=utf-8" "$(hdr Content-Type "$B/")"
check "index Cache-Control no-cache"    "no-cache" "$(hdr Cache-Control "$B/")"
check "HEAD / → 200"                    200 "$(code -I "$B/")"
check "HEAD / has no body"              0 "$(curl -s -I "$B/" -o /dev/null -w '%{size_download}')"
check "HEAD / Content-Length"           "$(wc -c < "$R/index.html" | tr -d ' ')" "$(hdr Content-Length -I "$B/")"
check "CSS Content-Type"                "text/css; charset=utf-8" "$(hdr Content-Type "$B/style.css")"
check "percent-encoded name"            "spaced" "$(curl -s "$B/a%20b.txt")"
check "query string ignored"            200 "$(code "$B/?utm=1")"
check "symlink inside root allowed"     200 "$(code "$B/inlink.css")"

# ---------- health ----------
check "/healthz → 200"                  200 "$(code "$B/healthz")"
check "/healthz body"                   "ok" "$(curl -s "$B/healthz")"
check "/healthz no-store"               "no-store" "$(hdr Cache-Control "$B/healthz")"

# ---------- security headers ----------
check "nosniff"                         "nosniff" "$(hdr X-Content-Type-Options "$B/")"
check "X-Frame-Options"                 "DENY" "$(hdr X-Frame-Options "$B/")"
check "Referrer-Policy"                 "strict-origin-when-cross-origin" "$(hdr Referrer-Policy "$B/")"
check "HSTS"                            "max-age=31536000" "$(hdr Strict-Transport-Security "$B/")"
check "CSP frame-ancestors"             yes "$(hdr Content-Security-Policy "$B/" | grep -q "frame-ancestors 'none'" && echo yes)"
check "Permissions-Policy"              yes "$(hdr Permissions-Policy "$B/" | grep -q 'camera=()' && echo yes)"
check "security headers on 404 too"     "nosniff" "$(hdr X-Content-Type-Options "$B/nope")"

# ---------- methods ----------
check "POST → 405"                      405 "$(code -X POST "$B/")"
check "PUT → 405"                       405 "$(code -X PUT "$B/")"
check "405 Allow header"                "GET, HEAD" "$(hdr Allow -X DELETE "$B/")"

# ---------- path safety ----------
check "/../outside.txt → 400"           400 "$(code "$B/../outside.txt")"
check "%2e%2e traversal → 400"          400 "$(code "$B/%2e%2e/outside.txt")"
check "encoded slash traversal → 400"   400 "$(code "$B/sub/..%2f..%2foutside.txt")"
check "symlink file escape → 403"       403 "$(code "$B/leak")"
check "symlink dir escape → 403"        403 "$(code "$B/leakdir/outside.txt")"
check "secret never leaks"              "" "$(curl -s --path-as-is "$B/leak" "$B/leakdir/outside.txt" "$B/../outside.txt" | grep TOPSECRET)"
check "dotfile → 404"                   404 "$(code "$B/.secret")"
check "missing → 404"                   404 "$(code "$B/missing.html")"
check "%00 → 400"                       400 "$(code "$B/index.html%00.css")"
check "bad escape → 400"                400 "$(code "$B/%zz")"
check "backslash → 400"                 400 "$(raw 'GET /\\evil.com HTTP/1.1\r\nHost: x\r\n\r\n')"

# ---------- directories ----------
check "dir without slash → 301"         301 "$(code "$B/sub")"
check "301 Location"                    "/sub/" "$(hdr Location "$B/sub")"
check "// does not open-redirect"       "/sub/" "$(hdr Location "$B//sub")"
check "dir with slash → index"          "<h1>sub</h1>" "$(curl -s "$B/sub/")"
check "dir without index → 404"         404 "$(code "$B/nodir/")"

# ---------- malformed requests ----------
check "oversized headers → 431"         431 "$(raw "GET / HTTP/1.1\r\nX: $(head -c 9000 /dev/zero | tr '\0' a)\r\n\r\n")"
check "HTTP/2.0 → 505"                  505 "$(raw 'GET / HTTP/2.0\r\n\r\n')"
check "garbage → 400"                   400 "$(raw 'GARBAGE\r\n\r\n')"
check "absolute-form → 400"             400 "$(raw 'GET http://evil/ HTTP/1.1\r\nHost: x\r\n\r\n')"
check "HTTP/1.0 works"                  200 "$(raw 'GET / HTTP/1.0\r\n\r\n')"

# ---------- concurrency ----------
( for _ in $(seq 1 30); do curl -s -o /dev/null -w '%{http_code}\n' "$B/" & done; wait ) > "$TMP/par.txt"
check "30 parallel requests → 200"      30 "$(grep -c '^200$' "$TMP/par.txt")"
check "server still alive"              200 "$(code "$B/healthz")"

# ---------- configuration ----------
check "invalid PORT → exit 2"           2 "$(PORT=abc "$BIN" "$R" 2>/dev/null; echo $?)"
check "missing root → exit 2"           2 "$(PORT=1 "$BIN" "$TMP/nope" 2>/dev/null; echo $?)"

check "invalid HOST → exit 2"           2 "$(HOST=nope PORT=1 "$BIN" "$R" 2>/dev/null; echo $?)"

# ---------- default bind: dual-stack "::", or 0.0.0.0 without IPv6 ----------
P3=$(free_port)
env -u HOST PORT="$P3" "$BIN" "$R" 2>"$TMP/default.log" &
PIDS+=($!)
for _ in $(seq 1 100); do curl -fs -o /dev/null "http://127.0.0.1:$P3/healthz" && break; sleep 0.05; done
check "default HOST reachable over IPv4" 200 "$(code "http://127.0.0.1:$P3/healthz")"
if [[ -e /proc/net/if_inet6 ]] && grep -q ' lo$' /proc/net/if_inet6; then
  check "default HOST is dual-stack"    yes "$(grep -q 'http://\[::\]' "$TMP/default.log" && echo yes)"
  check "default HOST reachable over IPv6" 200 "$(code -g "http://[::1]:$P3/healthz")"
else
  check "no IPv6: falls back to 0.0.0.0" yes "$(grep -q 'http://0.0.0.0' "$TMP/default.log" && echo yes)"
fi

# ---------- real site ----------
P2=$(free_port)
start "$REPO/public" "$P2"
check "public/ index served"            yes "$(curl -s "http://127.0.0.1:$P2/" | grep -q '<title>unemployed' && echo yes)"

echo "----"
echo "server tests: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
