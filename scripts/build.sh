#!/usr/bin/env bash
# Build Valkey -> wasm32-wasip1 reactor. Networking + event loop are bridged to
# the host (node:net) via imports; see src/wasi/. No fork, no threads.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${WASI_SDK:-/Users/michaelkremenetsky/Work/compiles/strapkit-rust/os/toolchain/wasi-sdk}"
CC="$SDK/bin/clang"
SYSROOT="$SDK/share/wasi-sysroot"
SRC="$ROOT/src/src"
DEPS="$ROOT/src/deps"
WASI="$ROOT/src/wasi"
OUT="$ROOT/build"
# Start from a clean object dir: the link step globs obj/*.o, so a stale object
# from a previous (broader) build would otherwise sneak into the image and clash.
rm -rf "$OUT/obj"
mkdir -p "$OUT/obj"

TARGET=wasm32-wasip1
# Reactor (stays resident, exported entry points; no _start main loop).
CFLAGS=(
  --target=$TARGET --sysroot="$SYSROOT"
  -O2 -fno-strict-aliasing -Wno-implicit-function-declaration
  -Wno-int-conversion -Wno-macro-redefined -Wno-unused
  -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_GETPID
  -DVALKEY_WASM=1 -DHAVE_WASI_AE=1
  # 9.x builds Lua as a static module (the default: BUILD_LUA unset -> STATIC_LUA=1).
  # server.c only registers the "lua" scripting engine when these are set; without
  # them EVAL fails with "Could not find scripting engine 'lua'".
  -DLUA_ENABLED -DSTATIC_LUA=1
  # persistence/threads off at compile time where the source allows
  -I"$SRC" -I"$DEPS/lua/src" -I"$DEPS/hiredis" -I"$DEPS/hdr_histogram"
  -I"$DEPS/fpconv" -I"$DEPS/fast_float" -I"$WASI"
  # 9.x: hiredis is gone; sentinel.c talks to the new bundled client via
  # <valkey/...> headers. We don't build libvalkey, so sentinel's client calls
  # stay undefined imports (harmless — sentinel mode is never entered here).
  -I"$DEPS/libvalkey/include"
  # force-include the compat header ahead of every TU
  -include "$WASI/wasm_compat.h"
)
LDFLAGS=(
  --target=$TARGET --sysroot="$SYSROOT"
  -mexec-model=reactor
  -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-mman -lwasi-emulated-getpid
  # wasi-libc disables long-double printf/scanf/strtold by default (they trap to
  # abort()). Valkey parses long doubles (INCRBYFLOAT, ZADD scores, BLPOP/BZPOPMIN
  # timeouts via strtold), so pull in the real implementations.
  -lc-printscan-long-double
  -Wl,--allow-undefined            # host imports resolved at instantiation
  -Wl,--export-dynamic
  -Wl,-z,stack-size=1048576
  # sock_shim.c provides __wrap_* for the fd-multiplexed libc calls; host fds
  # (>= HOST_FD_BASE) go to the bridge, everything else to real wasi-libc.
  -Wl,--wrap=read,--wrap=write,--wrap=writev,--wrap=close,--wrap=fcntl
)

# ---- Lua (bundled 5.1 + cjson/cmsgpack/struct — the BullMQ script libs) ----
LUA_SRC=(lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
  lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib
  ldblib liolib lmathlib loslib ltablib lstrlib loadlib linit lua_cjson
  lua_cmsgpack lua_struct lua_bit fpconv fpconv_dtoa strbuf)

# ---- Valkey server objects: EXACTLY the server target's object set
#      (ENGINE_SERVER_OBJ in src/src/Makefile). Using the precise list (instead
#      of every .o mentioned in the Makefile) keeps out the CLI/benchmark/tls
#      objects that pull in hiredis (sds redefinitions) or duplicate the command
#      table (cli_commands.o vs commands.o).
#      Excluded here: the pthread TUs (bio, io_threads, threads_mngr) — wasip1
#      has no threads; bio jobs run inline and io-threads is forced to 1. Their
#      API surfaces are implemented synchronously in src/wasi/stubs.c.
# 9.1.1 changed the Makefile: ENGINE_SERVER_OBJ is now a multi-line list ("VAR = \"
# continued with trailing backslashes), the objects live partly in a trace/
# subdir, and the trace set is appended via `ENGINE_SERVER_OBJ+=$(ENGINE_TRACE_OBJ)`.
# Pull both the SERVER and TRACE blocks (each: from "NAME = \" to the first line
# that does NOT end in a backslash), strip the var name / backslashes / .o suffix.
SERVER_SRC=$( { \
    sed -n '/^ENGINE_SERVER_OBJ = /,/[^\\]$/p' "$SRC/Makefile"; \
    sed -n '/^ENGINE_TRACE_OBJ = /,/[^\\]$/p' "$SRC/Makefile"; \
  } | sed 's/^ENGINE_[A-Z_]* = *//; s/\\//g; s/\.o//g' | tr -s ' \t' '\n' | grep -v '^$' )
#      tls: its no-OpenSSL form crashes this wasi-sdk's LLVM codegen, and TLS is
#      unused; the one symbol connection.c needs (RedisRegisterConnectionTypeTLS)
#      is provided in stubs.c.
#      rdma (new in 9.x): its non-Linux stub form crashes this wasi-sdk's LLVM
#      codegen (same failure mode as tls); RDMA is unused. The one symbol
#      connection.c needs (RegisterConnectionTypeRdma) is provided in stubs.c.
EXCLUDE_TU=" bio io_threads threads_mngr tls rdma "

compile() {
  local dir="$1" f="$2" extra="${3:-}"
  local o="$OUT/obj/$(basename "$f").o"
  "$CC" "${CFLAGS[@]}" $extra -c "$dir/$f.c" -o "$o" 2>>"$OUT/compile.err" \
    && echo "  ok  $f" || echo "  ERR $f"
}

: > "$OUT/compile.err"
: > "$OUT/link.err"
# Lua uses setjmp/longjmp for error handling (ldo.c). wasm has no native
# setjmp; enable the LLVM SjLj-over-wasm-exceptions lowering. Node's V8
# implements the wasm Exception-handling proposal, so this runs.
# -DENABLE_CJSON_GLOBAL: register the `cjson` table as a Lua global (Valkey's
# deps/Makefile sets it; without it luaopen_cjson returns the table but scripts
# can't reach `cjson.*`). LUA_ANSI/MKSTEMP match the vendored deps/Makefile.
LUA_CFLAGS="-DLUA_ANSI -DENABLE_CJSON_GLOBAL -DLUA_USE_MKSTEMP -mllvm -wasm-enable-sjlj"
echo "[lua]"
for f in "${LUA_SRC[@]}"; do
  [ -f "$DEPS/lua/src/$f.c" ] && compile "$DEPS/lua/src" "$f" "$LUA_CFLAGS"
  [ -f "$DEPS/fpconv/$f.c" ]  && compile "$DEPS/fpconv" "$f"
done
echo "[deps]"
# hdr_histogram: the latency/commandlog histograms (latency.c). Valkey builds it
# with its own allocator shim (hdr_redis_malloc.h) via HDR_MALLOC_INCLUDE.
compile "$DEPS/hdr_histogram" "hdr_histogram" '-DHDR_MALLOC_INCLUDE="hdr_redis_malloc.h"'

echo "[valkey]"
for f in $SERVER_SRC; do
  case "$EXCLUDE_TU" in *" $f "*) continue;; esac
  [ -f "$SRC/$f.c" ] && compile "$SRC" "$f"
done
# NB: ae_wasi.c is #include'd into ae.c (like ae_select.c), never compiled alone.
# ---- Lua scripting engine, now a static MODULE (src/modules/lua/*.c) ----
# 9.x split the Lua engine (script_lua/engine_lua/function_lua/debug_lua/list)
# into a module force-loaded at startup. It exports ValkeyModule_OnLoad_lua,
# which our dlsym shim (stubs.c) hands to moduleLoadStatic("lua"). Compiled with
# the module's own -I plus the SjLj flag (script_lua uses Lua setjmp/longjmp).
echo "[lua module]"
LUA_MOD_DIR="$SRC/modules/lua"
# valkeymodule.h tags the ValkeyModule_* API function pointers with
# __attribute__((__common__)) so a real .so can merge the tentative definitions
# from its TUs. Two problems here: (1) this wasi-sdk's LLVM crashes emitting a
# common-linkage global (AsmPrinter::emitGlobalVariable), and (2) we link the
# module TUs straight into the main image, where -fno-common would otherwise
# make each TU a strong def -> duplicate symbols. Redefining the attribute to
# `weak` solves both: wasm supports weak globals, and the linker merges the
# per-TU weak defs into the single set of pointers ValkeyModule_Init fills in.
LUA_MOD_CFLAGS="-I$LUA_MOD_DIR -std=gnu11 -DVALKEYMODULE_ATTR_COMMON=__attribute__((weak)) $LUA_CFLAGS"
for f in script_lua engine_lua function_lua debug_lua list; do
  compile "$LUA_MOD_DIR" "$f" "$LUA_MOD_CFLAGS"
done

echo "[wasi compat]"
for f in sock_shim stubs reactor; do
  compile "$WASI" "$f"
done
# The setjmp/longjmp runtime uses __builtin_wasm_throw, so it needs the same
# wasm-exception codegen the Lua TUs use.
compile "$WASI" "wasm_sjlj" "$LUA_CFLAGS"

echo "[link]"
# The wasm setjmp/longjmp lowering throws/catches the `__c_longjmp` exception
# tag, which (like Emscripten) is an IMPORTED tag: the host supplies it as a
# WebAssembly.Tag under env.__c_longjmp (see bridge/valkey-server.mjs). It stays
# an import here on purpose — that's why --allow-undefined is set.
"$CC" "${LDFLAGS[@]}" "$OUT"/obj/*.o -o "$OUT/valkey.wasm" 2>>"$OUT/link.err" \
  && echo "LINK OK -> $OUT/valkey.wasm" || { echo "LINK FAILED (see build/link.err)"; tail -30 "$OUT/link.err"; exit 1; }
ls -la "$OUT/valkey.wasm"
