#! /bin/bash

# Build all portable-tpcc components from the repository root:
#   - C++ shared libraries, DBMS adapters, and app binaries via ./ya make
#   - Go orchestrator via the standard Go toolchain (not ya make)
#
# The YDB adapter/binary (tpcc/app/ydb) requires CUDA to be disabled in the
# ya make graph; this script always passes:
#   -DHAVE_CUDA=no -DCUDA_VERSION=11.4
# Equivalent standalone command:
#   ./ya make -r -DHAVE_CUDA=no -DCUDA_VERSION=11.4 tpcc/app/ydb
#
# This script is a thin convenience wrapper. It does not introduce an
# alternate root build system; see docs/specification.md §12 and AGENTS.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RUN_TESTS=0
BUILD_TPCC=1
BUILD_MIND_TPCC=1
YA_BUILD_FLAG=()
YA_THREADS=()
YA_EXTRA=()
# Required for tpcc/app/ydb (and thus for the full tpcc/ recurse).
YA_CUDA_FLAGS=(-DHAVE_CUDA=no -DCUDA_VERSION=11.4)

usage() {
  cat <<'EOF'
Usage: ./build.sh [options] [-- ya-make-args...]

Build all portable-tpcc components:
  ./ya make -DHAVE_CUDA=no -DCUDA_VERSION=11.4 tpcc
  go -C mind build ./cmd/mind-tpcc

The CUDA defines are always passed: the YDB target (tpcc/app/ydb) must be
built without CUDA. Standalone equivalent:
  ./ya make -r -DHAVE_CUDA=no -DCUDA_VERSION=11.4 tpcc/app/ydb

Options:
  -r, --release       Release build for C++ targets (-r)
  -d, --debug         Debug build for C++ targets (-d; ya default)
  -t, --test          Also run tests (./ya make -t tpcc; go test ./...)
  -j N, --jobs N      Parallelism for ya make
  --tpcc-only         Build only C++ targets under tpcc/
  --mind-tpcc-only    Build only the Go orchestrator
  -h, --help          Show this help

Arguments after -- are forwarded to ./ya make.

Examples:
  ./build.sh
  ./build.sh -r
  ./build.sh -t -j8
  ./build.sh --tpcc-only -- -v
  ./build.sh --mind-tpcc-only
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -r|--release)
      YA_BUILD_FLAG=(-r)
      shift
      ;;
    -d|--debug)
      YA_BUILD_FLAG=(-d)
      shift
      ;;
    -t|--test)
      RUN_TESTS=1
      shift
      ;;
    -j|--jobs)
      if [[ $# -lt 2 ]]; then
        echo "Option $1 requires a value" >&2
        usage >&2
        exit 1
      fi
      YA_THREADS=(-j "$2")
      shift 2
      ;;
    -j*)
      YA_THREADS=(-j "${1#-j}")
      shift
      ;;
    --tpcc-only)
      BUILD_TPCC=1
      BUILD_MIND_TPCC=0
      shift
      ;;
    --mind-tpcc-only)
      BUILD_TPCC=0
      BUILD_MIND_TPCC=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      YA_EXTRA+=("$@")
      break
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ "$BUILD_TPCC" -eq 0 && "$BUILD_MIND_TPCC" -eq 0 ]]; then
  echo "Nothing to build" >&2
  exit 1
fi

if [[ ! -x ./ya ]]; then
  echo "./ya launcher not found or not executable in $SCRIPT_DIR" >&2
  exit 1
fi

if [[ "$BUILD_MIND_TPCC" -eq 1 ]] && ! command -v go >/dev/null 2>&1; then
  echo "go is required to build mind-tpcc" >&2
  exit 1
fi

build_tpcc() {
  local -a ya_args=(make tpcc)
  ya_args+=("${YA_BUILD_FLAG[@]}")
  ya_args+=("${YA_CUDA_FLAGS[@]}")
  ya_args+=("${YA_THREADS[@]}")
  if [[ "$RUN_TESTS" -eq 1 ]]; then
    ya_args+=(-t)
  fi
  ya_args+=("${YA_EXTRA[@]}")

  echo "==> Building C++ components: ./ya ${ya_args[*]}"
  ./ya "${ya_args[@]}"
}

build_mind_tpcc() {
  echo "==> Building Go orchestrator: go -C mind build ./cmd/mind-tpcc"
  go -C mind build ./cmd/mind-tpcc

  if [[ "$RUN_TESTS" -eq 1 ]]; then
    echo "==> Testing Go orchestrator: go -C mind test ./..."
    go -C mind test ./...
  fi
}

if [[ "$BUILD_TPCC" -eq 1 ]]; then
  build_tpcc
fi

if [[ "$BUILD_MIND_TPCC" -eq 1 ]]; then
  build_mind_tpcc
fi

echo "Done."
if [[ "$BUILD_TPCC" -eq 1 ]]; then
  echo "C++ binaries (symlinks after ya make):"
  echo "  tpcc/app/pgsql/tpcc-pgsql"
  echo "  tpcc/app/ydb/tpcc-ydb"
  echo "  tpcc/app/oceanbase/tpcc-oceanbase"
fi
if [[ "$BUILD_MIND_TPCC" -eq 1 ]]; then
  echo "Go binary: mind/mind-tpcc"
fi
