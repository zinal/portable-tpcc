#! /bin/bash

# Refresh vendored Arcadia/YDB externals used by portable-tpcc from a YDB
# checkout. Source may be a local path or a sparse clone of
# https://github.com/ydb-platform/ydb.
#
# Only directories already present in this repository are updated. Local-only
# vendored trees (for example contrib/libs/libpqxx) are skipped when missing
# from the YDB source.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

YDB_REPO_URL="${YDB_REPO_URL:-https://github.com/ydb-platform/ydb.git}"
YDB_REF="${YDB_REF:-main}"
YDB_ROOT="${YDB_ROOT:-}"
FROM_GITHUB=0
KEEP_CLONE=0
TMP_CLONE=""

usage() {
  cat <<'EOF'
Usage: ./refresh_externals.sh [options]

Refresh vendored build/util/contrib/library/tools/ydb/certs trees from YDB.

Options:
  --root PATH       Path to an existing YDB repository checkout
  --from-github     Sparse-clone YDB from GitHub (default if --root / YDB_ROOT
                    is omitted)
  --ref REF         Git branch or tag for --from-github (default: main)
  --repo URL        Override YDB git URL
                    (default: https://github.com/ydb-platform/ydb.git)
  --keep-clone      Keep the temporary GitHub clone and print its path
  -h, --help        Show this help

Environment:
  YDB_ROOT          Same as --root
  YDB_REF           Same as --ref
  YDB_REPO_URL      Same as --repo

Examples:
  ./refresh_externals.sh --from-github
  ./refresh_externals.sh --from-github --ref stable-25-1
  ./refresh_externals.sh --root /path/to/ydb
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --root)
      YDB_ROOT="$2"
      shift 2
      ;;
    --from-github)
      FROM_GITHUB=1
      shift
      ;;
    --ref)
      YDB_REF="$2"
      shift 2
      ;;
    --repo)
      YDB_REPO_URL="$2"
      shift 2
      ;;
    --keep-clone)
      KEEP_CLONE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -n "$YDB_ROOT" && "$FROM_GITHUB" -eq 1 ]]; then
  echo "Specify either --root or --from-github, not both" >&2
  exit 1
fi

if [[ -z "$YDB_ROOT" ]]; then
  FROM_GITHUB=1
fi

copy_dir() {
  local src="$1"
  local dst="$2"

  if [[ ! -d "$src" ]]; then
    echo "SKIP (missing in source): $dst"
    return 0
  fi

  echo "Refresh: $dst"
  rm -rf "$dst"
  mkdir -p "$(dirname "$dst")"
  cp -a "$src" "$dst"
}

refresh_children() {
  local rel="$1"
  local child name

  if [[ ! -d "$rel" ]]; then
    return 0
  fi

  for child in "$rel"/*; do
    [[ -e "$child" ]] || continue
    [[ -d "$child" ]] || continue
    name="$(basename "$child")"
    copy_dir "$YDB_ROOT/$rel/$name" "$rel/$name"
  done
}

# Paths to materialize in a sparse GitHub clone, based on what this repo vendors.
collect_sparse_paths() {
  echo "ya"
  echo "ya.bat"
  echo "ya.conf"
  echo "build"
  echo "util"
  echo "certs"

  local rel child
  for rel in \
    contrib/libs \
    contrib/restricted \
    contrib/tools \
    contrib/proto \
    library/cpp \
    tools \
    ydb/library \
    ydb/public
  do
    if [[ -d "$rel" ]]; then
      for child in "$rel"/*; do
        [[ -d "$child" ]] || continue
        echo "$child"
      done
    fi
  done
}

cleanup() {
  if [[ -n "$TMP_CLONE" && "$KEEP_CLONE" -eq 0 ]]; then
    rm -rf "$TMP_CLONE"
  fi
}
trap cleanup EXIT

if [[ "$FROM_GITHUB" -eq 1 ]]; then
  if ! command -v git >/dev/null 2>&1; then
    echo "git is required for --from-github" >&2
    exit 1
  fi

  mapfile -t SPARSE_PATHS < <(collect_sparse_paths)

  TMP_CLONE="$(mktemp -d "${TMPDIR:-/tmp}/portable-tpcc-ydb.XXXXXX")"
  echo "Sparse-cloning $YDB_REPO_URL @ $YDB_REF into $TMP_CLONE"
  git clone --depth 1 --filter=blob:none --sparse --branch "$YDB_REF" \
    "$YDB_REPO_URL" "$TMP_CLONE"

  (
    cd "$TMP_CLONE"
    git sparse-checkout init --no-cone
    printf '%s\n' "${SPARSE_PATHS[@]}" | git sparse-checkout set --stdin
  )

  YDB_ROOT="$TMP_CLONE"
fi

if [[ ! -d "$YDB_ROOT" ]]; then
  echo "YDB_ROOT is not a directory: $YDB_ROOT" >&2
  exit 1
fi

echo "YDB_ROOT: $YDB_ROOT"

cp "$YDB_ROOT/ya" .
cp "$YDB_ROOT/ya.bat" .
cp "$YDB_ROOT/ya.conf" .

copy_dir "$YDB_ROOT/build" build
copy_dir "$YDB_ROOT/util" util
copy_dir "$YDB_ROOT/certs" certs

refresh_children contrib/libs
refresh_children contrib/restricted
refresh_children contrib/tools
refresh_children contrib/proto
refresh_children library/cpp
refresh_children tools
refresh_children ydb/library
refresh_children ydb/public

echo "Done."
if [[ "$KEEP_CLONE" -eq 1 && -n "$TMP_CLONE" ]]; then
  echo "Kept clone at: $TMP_CLONE"
fi
