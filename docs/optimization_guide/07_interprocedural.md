# 7. Function-Level and Whole-Program Passes

Everything in chapters 1–6 works *within* a function. A call to an unknown function is a wall: the optimizer must assume it reads and writes all escaped memory, never returns a predictable value, and might throw. Interprocedural optimization (IPO) breaks down those walls. **Inlining** removes the wall entirely, **attributes and inferred function effects** make it thinner, and **LTO and PGO** widen what the compiler can see.

LLVM runs IPO bottom-up over the **call graph** in strongly-connected-component (SCC) order, so callees are optimized (and their attributes inferred) before callers decide whether to inline them.

---

## 7.1 Inlining

### Mechanism
- The call is replaced by a copy of the callee's body. The callee's parameters, which are SSA arguments, are substituted with the caller's actual SSA values, and the callee's `ret` becomes a branch to the continuation, with a φ if there are several returns.
- **That substitution is the payoff.** A constant argument becomes a constant inside the inlined body, so SCCP, folding and DCE fire again. A known function pointer becomes a direct call that can itself be inlined. A pointer that was "escaping into a call" is now visible, so alias analysis improves.
- **Cost model:** the inliner estimates the callee's cost after simplification, *given these specific arguments*. Instructions that would fold away count as free. The bonus `cost=-15030` in the remark below comes from the loop disappearing once `e = 3` is known. The cost is compared against a threshold of 225 by default at `-O2`, 250 at `-O3`, and less for cold call sites. Example from ch. 3: ✓ verified: `'pw' inlined into 'cube' with (cost=-15030, threshold=337)`.
- A `static` function with a single call site is almost always inlined, because the original copy can be deleted afterward, so the code size doesn't grow.

### Before / after: the typical C++ abstraction cascade
```cpp
struct Vec { float x, y; };
inline Vec   operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
inline float dot(Vec a, Vec b)       { return a.x * b.x + a.y * b.y; }

float energy(Vec p, Vec v) { return dot(p + v, p + v); }
```
After inlining, SROA splits each `Vec` into two SSA floats, CSE merges the two `p + v`, and the result is two adds, a multiply and an FMA, with no stack traffic at all. Without inlining, each call goes through an ABI shuffle (on AArch64 a two-float struct passes in `s0`/`s1` as a homogeneous aggregate, but every call still clobbers caller-saved registers).

**Hardware impact:**
- **Pro:** no call/return overhead (`bl`/`ret`, save/restore of callee-saved registers, stack adjustment), no return-address-stack pressure, and, by far the biggest effect, *all the follow-on optimizations*, including vectorizing loops that contained calls.
- **Con:** code growth. Inlining a large function at many call sites bloats the I-cache and the µop cache, and front-end stalls (iTLB, I-cache misses) are a top bottleneck in large server binaries. This is exactly why PGO (§7.5) matters for inlining decisions.

### Controls
| Control | Effect |
|---|---|
| `inline` keyword | **Linkage only** (permits multiple definitions, as in ODR). It barely affects the inliner. Header-defined functions are inlinable because their *body is visible*, not because of the keyword |
| `[[gnu::always_inline]]` / `__attribute__((always_inline))` | Forces inlining (an error or warning if impossible) |
| `[[gnu::noinline]]` / `__attribute__((noinline))` | Prevents inlining. Use it to keep cold code out of hot paths or for benchmarks |
| `[[gnu::flatten]]` | Inlines *everything* called from this function, recursively (GCC; Clang supports it too) |
| `[[clang::always_inline]]` on a call statement (Clang) | Forces inlining at one call site |
| `[[gnu::cold]]` / `[[unlikely]]` | Lowers the inline threshold at that site |
| `-mllvm -inline-threshold=N` (Clang), `--param max-inline-insns-single=N`, `-finline-limit=N` (GCC) | Global threshold |
| `-fno-inline`, `-fno-inline-functions` | Disables inlining (keeps `always_inline`) |
| `-Rpass=inline`, `-Rpass-missed=inline` | Reports every decision, with its cost and threshold |

---

## 7.2 Function Specialization (Cloning)

### Mechanism
- When a function is called with a **constant argument** at some call sites, especially a **function pointer**, IPO can create a *clone* with that parameter replaced by the constant, then redirect those call sites to the clone. This gets most of the benefit of inlining without copying the body into every caller.
- LLVM's `FunctionSpecialization` runs as part of `IPSCCP`. It scores each candidate by how much the constant would fold inside the clone (a known function pointer becomes a direct call that can be inlined, which then lets the loop vectorize), and weighs that against code growth. GCC's equivalent is `-fipa-cp-clone`.
- SSA makes this a clean value replacement: substitute the constant for the `Argument` in the clone, then run SCCP on it.

### Before
```cpp
static float add(float a, float b) { return a + b; }
static float mul(float a, float b) { return a * b; }

[[gnu::noinline]] static float fold(const float* v, int n,
                                    float (*op)(float, float), float init) {
    float acc = init;
    for (int i = 0; i < n; ++i) acc = op(acc, v[i]);   // indirect call per element
    return acc;
}
float sum(const float* v, int n)  { return fold(v, n, add, 0.0f); }
float prod(const float* v, int n) { return fold(v, n, mul, 1.0f); }
```

### After — ✓ verified, with caveats
- **Default `-O2`/`-O3`: not specialized.** `fold` keeps its function-pointer parameter, and each iteration makes an indirect call. The cost model judged it not worth it, mainly because `fold` is small.
- **With `-mllvm -force-specialization`:** two clones appear, each with the `op` and `init` parameters gone:
  ```llvm
  define internal fastcc float @_ZL4foldPKfiPFfffEf.specialized.1(ptr %v, i32 %n)
  define internal fastcc float @_ZL4foldPKfiPFfffEf.specialized.2(ptr %v, i32 %n)
  ```

**The C++ way: templates guarantee specialization.**
```cpp
template <class Op>
float fold_t(const float* v, int n, Op op, float init) {
    float acc = init;
    for (int i = 0; i < n; ++i) acc = op(acc, v[i]);
    return acc;
}
float sum_t(const float* v, int n) {
    return fold_t(v, n, [](float a, float b) { return a + b; }, 0.0f);
}
```
✓ verified (`-O2 -ffast-math`): `vectorized loop (vectorization width: 4, interleaved count: 4)`. Each lambda has its own *type*, so `fold_t` is instantiated per operation. That's specialization done by the front end, with no heuristics involved. This is why `std::sort` with a lambda is faster than `qsort` with a function pointer.

**Hardware impact:** an indirect call per element means a `blr` the branch predictor must target-predict, a full call boundary that blocks vectorization, and register spills around it. A specialized or templated loop turns into 4–16 elements per instruction.

### Controls
| Control | Effect |
|---|---|
| Templates + lambdas / function objects | Guaranteed specialization at compile time |
| `if constexpr` on template params | Specialization of control flow |
| `static` linkage | The compiler sees all call sites, so it can specialize and delete the generic version |
| `-fipa-cp-clone` (GCC, on at `-O3`) | GCC's constant-propagation cloning |
| `-mllvm -force-specialization`, `-mllvm -funcspec-min-function-size=N` | Push LLVM's specializer |
| `-flto` | Specialization across translation units |

---

## 7.3 `__attribute__((pure))` and `__attribute__((const))`

### Mechanism
- An opaque call normally clobbers all escaped memory and can't be CSE'd, hoisted or deleted. These attributes promise:
  - **`const`**: the result depends *only* on the argument values. The function reads no memory (except constant memory) and writes none. In LLVM this is `memory(none)`.
  - **`pure`**: no side effects, but it may *read* memory, such as globals or pointer arguments. The result can change if that memory changes. In LLVM this is `memory(read)`.
- Given those promises, calls become ordinary SSA values: CSE/GVN can merge duplicate calls, LICM can hoist them out of loops (with `pure` only if nothing in the loop writes memory), and DCE can delete calls whose result is unused.
- **The compiler infers these itself** for functions whose bodies it can see. The `memory(none)` and `memory(argmem: readwrite)` annotations on the functions in this guide's IR dumps were all inferred by `FunctionAttrs`. The explicit attribute matters for **declarations** whose bodies live in another TU, a shared library, or assembly.

### Before / after — ✓ verified (`-O2`)
```cpp
[[gnu::const]] int sq_c(int);     // declaration only: body elsewhere
int            sq_plain(int);

int use_const(int x) { return sq_c(x) + sq_c(x); }
int use_plain(int x) { return sq_plain(x) + sq_plain(x); }
```
```llvm
define i32 @use_const(i32 %x) {
  %call = tail call i32 @sq_c(i32 %x)
  %add  = shl nsw i32 %call, 1          ; ONE call; c + c → c << 1
  ret i32 %add
}
define i32 @use_plain(i32 %x) {
  %call  = tail call i32 @sq_plain(i32 %x)
  %call1 = tail call i32 @sq_plain(i32 %x)   ; must call twice: it might have side effects
  %add   = add nsw i32 %call1, %call
  ret i32 %add
}
```

**Hardware impact:** this saves an entire call each time, plus the caller-saved register spills and reloads around it. In a loop, hoisting a `const` call can turn `O(n)` calls into one.

> **Danger:** like `restrict`, these are unchecked promises. Marking a function `const` when it reads a global, or `pure` when it logs, leads to silently wrong results after CSE or hoisting. `const` on a function taking a pointer and reading through it is a classic bug: use `pure` for that.

### Controls
| Control | Effect |
|---|---|
| `[[gnu::const]]`, `__attribute__((const))` | No memory access at all |
| `[[gnu::pure]]`, `__attribute__((pure))` | Reads only |
| `[[gnu::nothrow]]` / `noexcept` | No unwinding, so the compiler can drop landing pads and move code more freely around the call |
| `[[gnu::leaf]]` (GCC) | Doesn't call back into this TU, so the TU's static globals stay unclobbered |
| `[[nodiscard]]` | Warning only, no optimization effect |
| `[[unsequenced]]` / `[[reproducible]]` (C23, *not* C++) | C's standardized near-equivalents of `const` and `pure`. In C++, use the `gnu::` spellings |
| `-fno-math-errno` | Makes `<cmath>` functions `const`-like, since they no longer write `errno` |
| `constexpr` functions | Not the same thing: `constexpr` permits compile-time evaluation but says nothing about runtime effects |

---

## 7.4 Link-Time Optimization (LTO)

### Mechanism
- Normally each `.cpp` is optimized alone, and a call into another TU is opaque. With `-flto`, the compiler writes **IR (LLVM bitcode or GCC GIMPLE)** into the `.o` files instead of machine code. At link time, the linker plugin merges the IR and runs the optimizer on the **whole program**, then does code generation.
- Now cross-TU calls can be inlined, cross-TU constants propagate (IPSCCP), unused functions are removed (global DCE), function attributes are inferred across TUs, and `GlobalsAA` sees every user of a global.
- **Full LTO** (`-flto`) merges everything into one module. It's maximally powerful but serial, slow and memory-hungry.
- **ThinLTO** (`-flto=thin`) builds a compact summary index for each module. The thin link step decides *which* functions to import into which modules, and then the backends run in parallel, one per module, with only the imported functions added. It gets most of full LTO's benefit and scales to Chrome-sized codebases. GCC's equivalent partitioning is `-flto=auto` / `-flto-partition`.

### Before / after — ✓ verified (`-O2`, two TUs)
```cpp
// lib.cpp
int scale(int x) { return x * 3; }

// main.cpp
int scale(int);
int main(int argc, char**) {
    int s = 0;
    for (int i = 0; i < argc * 1000; ++i) s += scale(i);
    return s & 0xff;
}
```
| Build | Calls to `scale` in `main`'s disassembly |
|---|---|
| `clang++ -O2 lib.cpp main.cpp` | **1** (a real `bl`, executed every iteration) |
| `clang++ -O2 -flto lib.cpp main.cpp` | **0** (inlined: the loop body is now plain arithmetic, open to every loop optimization) |

**Hardware impact:** typically 5–15% on large C++ applications, mostly from cross-TU inlining (small getters and setters defined in `.cpp` files), dead-code removal (a smaller binary means fewer I-cache and iTLB misses), and devirtualization (if LTO sees every subclass, a virtual call with one possible target becomes a direct call).

### Controls
| Control | Effect |
|---|---|
| `-flto` / `-flto=full` | Full LTO (Clang and GCC) |
| `-flto=thin` | ThinLTO (Clang). Use this for large projects |
| `-flto=auto` (GCC) | Parallel LTO using make's jobserver or the CPU count |
| `-fwhole-program-vtables -fvisibility=hidden` (Clang, with LTO) | Whole-program devirtualization |
| `-fvisibility=hidden` + explicit exports | Lets LTO treat non-exported functions as internal |
| `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` | CMake's portable switch |
| `-Wl,-cache_path_lto,<dir>` (ld64), `-Wl,--thinlto-cache-dir=<dir>` (lld) | Incremental ThinLTO caching |
| Compiler and linker must match | Clang LTO needs `lld` or `ld64` (macOS) or the LLVMgold plugin. GCC LTO needs `gcc-ar`/`gcc-ranlib` for static libraries |

---

## 7.5 Profile-Guided Optimization (PGO)

### Mechanism
- Static heuristics guess which branches are taken and which functions are hot. PGO **measures** them. You build an instrumented binary, run it on representative workloads, and feed the counts back.
- The profile is attached to the IR as `!prof` **branch-weight metadata** and **function entry counts**. Then nearly every pass uses it:
  - **Inlining:** hot call sites get much higher thresholds, and cold ones essentially none.
  - **Block layout:** the hot path becomes the fall-through, straight-line path, and cold blocks move to the end of the function or into a separate `.text.unlikely`/`.cold` section (*hot/cold splitting*). Taken branches are what cost front-end bandwidth.
  - **Register allocation:** spills go into cold blocks.
  - **Loop opts:** unroll and vectorize only loops that actually run many iterations.
  - **Indirect call promotion:** if a virtual or function-pointer call usually goes to one target, the compiler emits `if (fp == likely_target) likely_target(...) /* inlinable */ else fp(...)`.
  - **Switch lowering:** the hottest cases are tested first.
- **Sample-based PGO** (AutoFDO, `perf` + `create_llvm_prof`) uses hardware sampling of a normal production binary instead of instrumentation. That means no slow instrumented build and real-world profiles, but less precision.
- **Post-link optimization** (LLVM **BOLT**, **Propeller**) reorders the *final binary's* basic blocks and functions using sampled profiles. It catches layout decisions the compiler can't make (cross-function ordering), with a typical extra 5–15% on top of PGO+LTO for large server binaries.

### Before
```cpp
int parse(const char* s) {
    if (*s == '#') return handle_comment(s);   // 0.1% of inputs
    if (*s == '"') return handle_string(s);    // 2%
    return handle_token(s);                    // 97.9%: but static heuristics don't know
}
```

### After (with a profile)
```cpp
int parse(const char* s) {
    if (*s != '#' && *s != '"') [[likely]]
        return handle_token(s);                // inlined, fall-through, hot section
    // handle_string / handle_comment: kept as calls, moved to .text.cold
    ...
}
```

### Workflow (Clang)
```bash
# 1. Instrumented build
clang++ -O2 -fprofile-generate=./prof app.cpp -o app_instr
# 2. Representative run(s)
./app_instr < typical_workload.txt
# 3. Merge the raw profiles
llvm-profdata merge -o app.profdata ./prof     # on macOS: xcrun llvm-profdata ...
# 4. Optimized build
clang++ -O2 -fprofile-use=app.profdata app.cpp -o app
```
GCC: `-fprofile-generate`, run, then `-fprofile-use` (`.gcda` files, no merge step). For sample-based PGO: `-fprofile-sample-use=` (Clang), `-fauto-profile=` (GCC).

**Hardware impact:** typically **10–30%** on branchy, call-heavy code such as compilers, interpreters, databases and browsers (Chrome, Firefox, CPython and Clang itself all ship PGO builds). The gains come mostly from the front end: fewer taken branches, a denser hot I-cache and iTLB footprint, better branch prediction, and smarter inlining. Tight numeric loops gain little.

> **Pitfall:** a profile from an *unrepresentative* workload actively hurts. For example, if code paths only seen in tests are made hot, the real hot path is pushed out of line.

### Controls
| Control | Effect |
|---|---|
| `-fprofile-generate` / `-fprofile-use` | IR-level instrumentation PGO (Clang and GCC) |
| `-fprofile-instr-generate` / `-fprofile-instr-use` | Front-end instrumentation (Clang; the same one used for coverage) |
| `-fprofile-sample-use=file.prof` (Clang), `-fauto-profile` (GCC) | Sample-based PGO |
| `-fcs-profile-generate` (Clang) | Context-sensitive PGO: a second profiling pass *after* inlining |
| `-Wno-profile-instr-out-of-date`, `-Wprofile-instr-missing` | Profile staleness diagnostics |
| `[[likely]]`, `[[unlikely]]`, `__builtin_expect`, `[[gnu::hot]]`, `[[gnu::cold]]` | Manual static hints when PGO isn't feasible |
| `llvm-bolt app -o app.bolt -data=perf.fdata -reorder-blocks=ext-tsp -reorder-functions=cdsort` | Post-link optimization (ELF/Linux) |
