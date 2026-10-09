# 5. Alias Analysis and Memory Optimizations

SSA covers **registers**: values with exactly one definition. **Memory is not in SSA.** A store to `*p` may or may not change what a later load from `*q` returns, depending on whether `p` and `q` point to overlapping bytes. **Alias analysis (AA)** answers that question, and its answers are `NoAlias`, `MustAlias`, `PartialAlias` or `MayAlias`. Every memory optimization (load elimination, store elimination, LICM scalar promotion, vectorization without runtime checks) is limited by how precise the AA is.

LLVM's bridge between AA and SSA is **MemorySSA**. It gives memory *its own* SSA form: each store and call is a `MemoryDef`, each load is a `MemoryUse`, and `MemoryPhi`s sit at merges. A load's "reaching store" then becomes a use-def walk, exactly like a register. AA is consulted along the way to skip over defs that `NoAlias` the load.

LLVM chains several AA implementations, and the first definitive answer wins:

| AA | Reasons from |
|---|---|
| `BasicAA` | Distinct allocas/globals, constant offsets from the same base, `noalias` arguments, escape/capture info |
| `TypeBasedAA` (TBAA) | C++ strict-aliasing rules, via `!tbaa` metadata |
| `ScopedNoAliasAA` | `!alias.scope`/`!noalias` metadata from inlined `__restrict__` parameters |
| `GlobalsAA` | Globals whose address never escapes the module |

---

## 5.1 Type-Based Alias Analysis (TBAA)

### Mechanism
- The C++ standard ([basic.lval]/11, the "strict aliasing rule") says that an object may be accessed only through a glvalue of its own type, a signed/unsigned variant of it, `char`/`unsigned char`/`std::byte`, or (in some cases) an aggregate containing it. **Accessing an `int` through a `float*` is undefined behavior.**
- The compiler therefore *assumes* that an `int*` and a `float*` never point to the same object. Clang encodes the type of every load and store as `!tbaa` metadata, a tree rooted at "omnipotent char" with `int`, `float`, `long`, pointer types, and struct-field paths beneath it. Two accesses whose types aren't ancestor and descendant in that tree are `NoAlias`.

### Before
```cpp
int store_both(int* p, float* q) {
    *p = 1;
    *q = 2.0f;       // can this overwrite *p?
    return *p;
}
int store_both_same_type(int* p, int* q) {
    *p = 1;
    *q = 2;
    return *p;
}
```

### After — ✓ verified (`-O2`)
```llvm
; store_both: int vs float → NoAlias by TBAA → load forwarded
  store i32 1, ptr %p,  !tbaa !int
  store float 2.0, ptr %q, !tbaa !float
  ret i32 1                                  ; no reload

; store_both_same_type: int vs int → MayAlias → must reload
  store i32 1, ptr %p, !tbaa !int
  store i32 2, ptr %q, !tbaa !int
  %0 = load i32, ptr %p, !tbaa !int          ; p == q is legal
  ret i32 %0
```
**Hardware impact:** the eliminated load saves an L1 round-trip and, more importantly, removes a *store→load dependency* that the CPU's memory disambiguator would otherwise have to predict. In loops, TBAA lets values stay in registers across stores to unrelated types.

### Type punning done right
```cpp
float f = 1.0f;
int bad  = *reinterpret_cast<int*>(&f);   // UB: TBAA may reorder or delete things around it
int good = std::bit_cast<int>(f);         // C++20: defined behavior, compiles to one fmov
int ok;  std::memcpy(&ok, &f, sizeof ok); // pre-C++20 idiom, also compiles to one fmov
```

### Controls
| Control | Effect |
|---|---|
| `-fstrict-aliasing` | On by default at `-O2`+ in both compilers. Enables TBAA |
| `-fno-strict-aliasing` | Disables TBAA. **The Linux kernel builds with this.** Use it for legacy codebases full of punning |
| `std::bit_cast` (C++20), `std::memcpy` | Legal type punning, optimized to register moves |
| `std::start_lifetime_as<T>` (C++23) | Legally reinterprets raw bytes, e.g. a network buffer, as an object |
| `__attribute__((may_alias))` | Exempts a type from TBAA, as if it were `char` |
| `-Wstrict-aliasing=2` (GCC) | Warns about some violations |
| `-fsanitize=type` (Clang ≥ 20, experimental TySan) | Catches strict-aliasing violations at runtime |

> **Gotcha:** `char*`, `unsigned char*` and `std::byte*` alias *everything*. Code that writes through a `char*` (e.g. `uint8_t*` buffers, which are `unsigned char`) inside a loop forces reloads of every other value. A loop that writes `out[i]` through `uint8_t*` can be dramatically slower than the same loop over `uint16_t`, because `uint8_t` aliases the loop's own `this`, its bounds, and so on.

---

## 5.2 Points-To Analysis and Escape Analysis

### Mechanism
**Points-to analysis** asks "what set of allocations could this pointer refer to?" If two pointers' points-to sets are disjoint, the pointers can't alias. LLVM doesn't run a heavyweight whole-program points-to analysis (Andersen- or Steensgaard-style) in its default pipeline. `BasicAA` uses cheaper local rules that capture most of the value:
- Two *different* `alloca`s or globals never alias each other.
- `base + 4` and `base + 8` with 4-byte accesses don't alias.
- A pointer returned from `malloc`/`operator new` doesn't alias anything that existed before the call.

**Escape (capture) analysis** asks "has this object's address ever leaked somewhere the compiler can't track?" (into a global, to an opaque function, or returned). Using SSA def-use chains, the pass follows every use of the pointer: a load *through* it doesn't capture, while a store *of* it or passing it to an unknown call does. Then:
- A **non-escaped** local can't be touched by any call, so its value survives opaque calls and stays in a register.
- A non-escaped heap allocation that is only written and read locally can be **promoted to the stack or to registers, and the `malloc`/`free` pair deleted**.
- A captured pointer must be assumed modified by every opaque call.

### Before
```cpp
void opaque();
void register_ptr(int*);

int local_noescape() { int x = 5; opaque(); return x; }
int local_escape()   { int x = 5; register_ptr(&x); x = 7; opaque(); return x; }

int heap_roundtrip() {
    int* p = static_cast<int*>(std::malloc(sizeof(int)));
    *p = 41;
    int r = *p + 1;
    std::free(p);
    return r;
}
```

### After — ✓ verified (`-O2`)
```llvm
define i32 @local_noescape() {
  call void @opaque()
  ret i32 5                         ; x never escaped → opaque() can't touch it
}

define i32 @local_escape() {
  %x = alloca i32                    ; stays in memory: its address escaped
  store i32 5, ptr %x
  call void @register_ptr(ptr %x)
  store i32 7, ptr %x
  call void @opaque()                ; might write through the saved pointer
  %0 = load i32, ptr %x              ; must reload
  ret i32 %0
}

define i32 @heap_roundtrip() {
  ret i32 42                         ; malloc, store, load, free: all deleted
}
```
**Hardware impact:** a non-escaped local lives in a register. An escaped one is a stack slot with a store before every call and a load after, and it adds dependencies through memory. Heap elision removes a `malloc`/`free` pair, typically 20–100 ns with lock-free fast paths, more under contention. This is why `std::unique_ptr` to a local object can sometimes cost nothing, and why passing `&local` to a logging function *anywhere* in a hot function can make the whole function slower.

### Controls
| Control | Effect |
|---|---|
| Don't take addresses needlessly | Pass small values by value, not `const int&` (taking a reference can capture) |
| `static` / anonymous namespace for globals | Enables `GlobalsAA`: an unescaped module-local global can't be touched by external calls |
| `__attribute__((noescape))` (Clang) | Promises that a pointer parameter isn't captured |
| `const` member functions | Do **not** help: `const` is a language-level promise and doesn't prevent mutation through other paths (`mutable`, `const_cast`, aliases) |
| `-flto` | Turns opaque calls into analyzable ones, so far fewer pointers "escape" (ch. 7) |

---

## 5.3 `restrict` / `__restrict__`

### Mechanism
- C99 `restrict` (spelled `__restrict__` or `__restrict` in C++, where it's a universally supported extension) promises: *for the lifetime of this pointer, the object it accesses is accessed only through this pointer* (or pointers derived from it).
- Clang lowers a `__restrict__` parameter to the LLVM `noalias` attribute, so `BasicAA` returns `NoAlias` between it and every other pointer not derived from it.
- When a function with `__restrict__` parameters is inlined, the guarantee is kept as `!alias.scope`/`!noalias` metadata, which `ScopedNoAliasAA` consumes.

### Before
```cpp
void add_into(float* a, const float* b, int n) {
    for (int i = 0; i < n; ++i) a[i] += b[i];
}
```
✓ verified: vectorized, but **with a runtime overlap check**. The IR contains a `vector.memcheck` block that compares the address ranges `[a, a+n)` and `[b, b+n)` and falls back to the scalar loop if they overlap.

### After
```cpp
void add_into(float* __restrict__ a, const float* __restrict__ b, int n) {
    for (int i = 0; i < n; ++i) a[i] += b[i];
}
```
✓ verified: the parameters become `ptr noalias`, the loop is vectorized 4×4, and there is **no memcheck block**.

**Hardware impact:** for long loops the runtime check is a fixed cost, and the real cost is elsewhere:
- Without `restrict`, the compiler can't keep values like `*len` or `this->scale` in registers across stores through other pointers inside the loop body. Every iteration reloads them.
- Two versions of every loop (vector plus scalar fallback) double the code size.
- Short loops pay the check every call.

> **Danger:** `restrict` is an *unchecked promise*. Calling `add_into(a, a + 1, n)` with restrict-qualified parameters is undefined behavior, and the vectorized code will read stale values and silently compute garbage.

### Controls
| Control | Effect |
|---|---|
| `T* __restrict__ p` | Function parameters (most effective), local pointers |
| `__restrict__` on member functions (`void f() __restrict__`) | Applies to `this` |
| `#pragma clang loop vectorize(assume_safety)` / `#pragma GCC ivdep` | A loop-scoped alternative: "assume no loop-carried memory dependences" |
| Copy to locals | `const float s = this->scale;` before the loop does manually what restrict would let the compiler do |
| `-fsanitize=address` | Does **not** detect restrict violations; nothing does reliably |

---

## 5.4 Load Forwarding and Redundant Load/Store Elimination

### Mechanism
- **Store-to-load forwarding (compile-time):** a load preceded by a store to a `MustAlias` location, with no possibly-aliasing store or call in between, is replaced by the stored value. In MemorySSA, the load's reaching `MemoryDef` *is* that store, and the replacement is one pointer walk.
- **Load-to-load forwarding:** a second load from the same location, with nothing clobbering it in between, reuses the first (GVN/EarlyCSE, ch. 1).
- **Dead store elimination (DSE):** a store overwritten by a later `MustAlias` store with no read in between is deleted, as is a store to a non-escaped object that's never read again.
- **Memory-to-register promotion:** `SROA` breaks up local aggregates (`struct`s, small arrays, `std::pair`, lambda captures) into their scalar fields and promotes each one to SSA. This is why zero-cost abstractions are actually zero-cost.

### Before
```cpp
int bump(int* p) {
    *p = 10;
    int y = *p;     // read back what we just wrote
    return y + 1;
}

struct Vec2 { float x, y; };
float len2(float a, float b) {
    Vec2 v{a, b};                 // aggregate on the "stack"
    return v.x * v.x + v.y * v.y;
}
```

### After — ✓ verified for `bump` (`-O2`)
```llvm
define i32 @bump(ptr %p) {
  store i32 10, ptr %p
  ret i32 11                    ; load forwarded, then constant-folded
}
```
`len2` has no `alloca` at all: `SROA` splits `v` into two SSA scalars, and the body is two multiplies and an add (or one `fmul` plus one `fmadd`).

**Hardware impact:** CPUs do forward stores to loads in hardware through the store buffer, but it isn't free: ~4–5 cycles of latency, and it *fails* (stalls 10+ cycles) on mismatched sizes or partial overlaps, e.g. writing two `int`s and reading one `int64_t`. Compile-time forwarding removes the round-trip entirely.

### Controls
| Control | Effect |
|---|---|
| Keep aggregates local and small | SROA works on non-escaped allocas |
| `-fstrict-aliasing`, `__restrict__` | More `NoAlias` answers, so more forwarding |
| `volatile` / `std::atomic` | Every access is kept. `std::atomic` loads with `relaxed` order *may* still be combined in theory, but compilers don't in practice |
| `-mllvm -enable-dse-partial-store-merging` etc. | DSE tuning (rarely needed) |
