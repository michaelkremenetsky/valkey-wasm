/* WebAssembly setjmp/longjmp runtime.
 *
 * Building Lua with `-mllvm -wasm-enable-sjlj` lowers its setjmp/longjmp into
 * three helper calls plus a throw of the `__c_longjmp` exception tag. wasi-sdk
 * ships the tag (in libclang_rt.builtins) but NOT these helpers (they normally
 * come from Emscripten's runtime), so they surface as undefined `env` imports.
 * Provide them here. The layout of `struct jmp_buf_impl` and the C_LONGJMP tag
 * index are the fixed ABI the LLVM lowering emits, matching Emscripten's
 * emscripten_setjmp.c. wasi-libc's jmp_buf (setjmp.h) is far larger than this
 * struct, so writing through it is safe. Node's V8 implements the wasm
 * Exception-handling proposal, so the throw/catch runs. */
#include <stdint.h>

struct __WasmLongjmpArgs { void *env; int val; };
struct jmp_buf_impl {
    void *func_invocation_id;
    uint32_t label;
    struct __WasmLongjmpArgs arg;
};

#define C_LONGJMP 1 /* exception-tag index the SjLj lowering throws/catches */

void __wasm_setjmp(void *env, uint32_t label, void *func_invocation_id) {
    struct jmp_buf_impl *jb = env;
    jb->func_invocation_id = func_invocation_id;
    jb->label = label;
}

uint32_t __wasm_setjmp_test(void *env, void *func_invocation_id) {
    struct jmp_buf_impl *jb = env;
    if (jb->func_invocation_id == func_invocation_id) return jb->label;
    return 0;
}

void __wasm_longjmp(void *env, int val) {
    struct jmp_buf_impl *jb = env;
    struct __WasmLongjmpArgs *arg = &jb->arg;
    if (val == 0) val = 1;
    arg->env = env;
    arg->val = val;
    __builtin_wasm_throw(C_LONGJMP, arg);
}
