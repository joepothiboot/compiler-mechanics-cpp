# 4. Strength Reduction and Auto-Vectorization

Both techniques are built on **Scalar Evolution (SCEV)**. SCEV turns the SSA φ-cycle of an induction variable into a closed-form *add recurrence*: `{start,+,step}<loop>`. An address like `&a[i*stride]` becomes `{a,+,4*stride}`, which literally says "a pointer that starts at `a` and advances by `4*stride` bytes per iteration." Strength reduction materializes that recurrence directly. The vectorizer uses it to tell whether accesses are contiguous, strided or random, and how far apart dependent accesses are.

---

## 4.1 Strength Reduction and Induction Variable Elimination

### Mechanism
- **Strength reduction** replaces an expensive operation with a cheaper equivalent:
  - In loops (**Loop Strength Reduction, LSR**): `base + i*stride*4` recomputed every iteration becomes a pointer `p` with `p += stride*4` each iteration. The multiply is replaced by an add.
  - In plain expressions: `x * 8` → `x << 3`, `x / 7` (unsigned) → multiply by a "magic" reciprocal plus shifts, `x % 8` → `x & 7` (unsigned; signed needs a fix-up).
- **IV elimination:** after LSR, the original counter `i` often has only one remaining use, the exit compare `i < n`. `IndVarSimplify` and LSR rewrite the exit test in terms of another IV, usually a down-counter compared to zero, and `i` is removed by DCE.
- **IV widening:** `int i` used as an array index would need a sign extension (`sext i32 → i64`) on every iteration on a 64-bit target. Signed overflow is undefined behavior, so the compiler can assume `i` never wraps and widens it to an `i64` IV once. *This is one of the main practical benefits of signed-overflow UB.*
- LSR is target-aware: it uses the target's addressing modes (`[x0, x1, lsl #2]` on AArch64, `[rax + rcx*4]` on x86) to decide which IVs to keep.

### Before
```cpp
void fill_strided(float* a, int n, int stride) {
    for (int i = 0; i < n; ++i)
        a[i * stride] = 0.5f;     // a multiply per iteration, plus a sign-extend
}
```

### After — ✓ verified (`-O2 -fno-vectorize`, arm64 asm)
```asm
    sbfiz   x8, x2, #2, #32     ; x8 = sext(stride) * 4   (hoisted, once)
    mov     w9, #0x3f000000     ; 0.5f bit pattern
    mov     w10, w1             ; down-counter = n
loop:
    str     w9, [x0]            ; *p = 0.5f
    add     x0, x0, x8          ; p += stride*4    ← strength-reduced: no mul
    subs    x10, x10, #1        ; --counter        ← i eliminated
    b.ne    loop                ; compare against zero is free (flags from subs)
```
`i` and `i * stride` are both gone. Each iteration is a store, an add, and a fused decrement-and-branch.

With vectorization enabled, the compiler *also* **versions** the loop: ✓ verified, the IR contains `%ident.check.not = icmp eq i32 %stride, 1`. If `stride == 1` at runtime it takes a contiguous 4×4 vectorized path, and otherwise it runs the scalar loop above.

### Division by a constant — ✓ verified
```cpp
unsigned div7(unsigned x) { return x / 7; }
```
```asm
    mov   w8, #0x4925
    movk  w8, #0x2492, lsl #16   ; magic = 0x24924925  (≈ 2^32 / 7, rounded up)
    umull x8, w0, w8             ; 64-bit product
    lsr   x8, x8, #32            ; high half
    sub   w9, w0, w8             ; fix-up for the 33-bit magic
    add   w8, w8, w9, lsr #1
    lsr   w0, w8, #2
```
Six cheap instructions (multiply latency ~3 cycles) instead of `udiv`, which takes ~7–12 cycles on Apple M-series and 20–40+ on older x86 cores.

**Hardware impact:** multiplies and divides compete for one or two ports. Adds and shifts can issue on 4–6 ALUs per cycle. Address arithmetic disappears into the load/store addressing mode. Fewer live IVs means lower register pressure.

### Controls
| Control | Effect |
|---|---|
| Use `int`/`std::ptrdiff_t` loop counters | Signed overflow UB permits IV widening. An `unsigned` counter may wrap, so it can need a `zext`/check each iteration |
| `size_t` counters | Already 64-bit, no widening needed; also fine |
| `-fwrapv` | Makes signed overflow defined (wrapping), which *disables* IV widening and some LSR. Use it knowingly |
| Divide by `constexpr`/literal divisors | Enables the magic-number reciprocal. A runtime divisor can't be reduced (see libdivide for that) |
| `-mllvm -disable-lsr` | Disables LSR (debugging only) |
| `-fivopts` (GCC, default on) | GCC's IV optimization pass |

---

## 4.2 SIMD Auto-Vectorization

### Mechanism
LLVM has two vectorizers:
- **Loop Vectorizer (LV):** widens a whole loop. Iteration `i` of the scalar loop becomes lane `i % VF` of a vector iteration. It also *interleaves* (unrolls the vector body), so `VF=4, IC=4` processes 16 elements per iteration using 4 independent vector registers.
- **SLP Vectorizer** (Superword-Level Parallelism): packs independent, isomorphic *straight-line* scalar operations into vector ops, e.g. `a.x+b.x; a.y+b.y; a.z+b.z; a.w+b.w` → one 4-wide add. It doesn't need a loop.

The Loop Vectorizer's pipeline:
1. **Legality:** a single-entry loop in canonical form, a computable trip count (or an exit it can handle), no calls without vector variants, and **no dependence between iterations that is shorter than VF** (§4.3).
2. **Memory checks:** if it can't prove that pointers don't overlap, it emits **runtime checks** (`vector.memcheck` blocks) and keeps the scalar loop as a fallback. This is loop versioning.
3. **Cost model:** it chooses the VF and IC for the target, or decides vectorizing isn't profitable.
4. **Epilogue:** it handles the `n % (VF*IC)` leftover iterations, often with a smaller *vectorized epilogue* (`vec.epilog.*` blocks), then a scalar remainder.

SSA makes "widening" mechanical. Every scalar `%x = fadd float %a, %b` becomes `%x.vec = fadd <4 x float> %a.vec, %b.vec`. Values that are uniform across lanes get a *broadcast* (splat). φ nodes for reductions become vector φs plus a final horizontal reduce.

### Before
```cpp
void saxpy(float* a, const float* b, float s, int n) {
    for (int i = 0; i < n; ++i)
        a[i] += s * b[i];
}
```

### After: shape of the vectorized IR — ✓ verified (`vectorization width: 4, interleaved count: 4`)
```text
entry:            if n < 4 → scalar loop
vector.memcheck:  if [a, a+n) overlaps [b, b+n) → scalar loop     (runtime alias check)
vector.body:      16 floats per iteration: 4 × (load <4 x float> b, fmul, load a, fadd, store)
vec.epilog.*:     a 4-wide (no interleave) loop for the remainder ≥ 4
scalar loop:      last 0–3 elements, and the aliasing fallback
```
With `float* __restrict__ a, const float* __restrict__ b`, the `vector.memcheck` block disappears (ch. 5).

**Hardware impact:**
- NEON (AArch64) and SSE (x86) registers are 128-bit = 4 `float`s. AVX2 is 256-bit (8 floats) and AVX-512 is 512-bit (16 floats). SVE is length-agnostic, 128 to 2048 bits.
- **Interleave count** matters as much as width. FP add/FMA latency is ~3–4 cycles but throughput is 2–4 per cycle, so 4 independent chains keep all the pipes busy.
- Compute-bound loops get close to VF× speedup. **Memory-bound loops** (anything streaming more than L2) are limited by bandwidth and gain far less. Vectorizing a loop that's waiting on DRAM mostly just makes it wait more efficiently.

### Controls
| Control | Effect |
|---|---|
| `-O2` (Clang), `-O2` (GCC ≥ 12, cheap model), `-O3` (GCC, full model) | Enables the vectorizers |
| `-fno-vectorize` / `-fno-slp-vectorize` (Clang), `-fno-tree-vectorize` (GCC) | Disables them |
| `-march=native`, `-mavx2`, `-mcpu=apple-m1`, `-march=armv9-a+sve2` | Wider and better ISA. **The default x86-64 target is SSE2-only (128-bit)** |
| `#pragma clang loop vectorize(enable) vectorize_width(8) interleave_count(2)` | Per-loop control |
| `#pragma omp simd` (+ `-fopenmp-simd`) | Portable "vectorize this, I promise it's safe". Overrides the dependence check |
| `#pragma GCC ivdep` | GCC: ignore assumed loop-carried dependencies |
| `__restrict__` | Removes the runtime alias checks (ch. 5) |
| `#pragma omp declare simd` (both, with `-fopenmp-simd`), `__attribute__((simd))` (GCC only; Clang ignores it) | Asks the compiler to generate a vector variant of a function so calls inside loops can vectorize |
| `-Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize` | **The** diagnostic tool: it tells you exactly why a loop didn't vectorize |
| `-fopt-info-vec-missed` (GCC) | GCC equivalent |

---

## 4.3 Loop-Carried Dependencies

### Mechanism
A **loop-carried dependency** means iteration `i` needs a result from iteration `i - d`. Vectorizing with width `VF` runs iterations `i…i+VF-1` simultaneously. That's legal only if every true dependence has **distance `d ≥ VF`**: no lane may need a value another lane in the same vector is still computing.

The dependence analysis computes `d` symbolically from SCEV. For `a[i]` written and `a[i-4]` read, the store is at `{a,+,4}` and the load at `{a-16,+,4}`. Subtracting gives a constant distance of 16 bytes = 4 elements.

### Case A: distance 1 (prefix sum), so it can't vectorize directly
```cpp
void prefix_sum(float* a, int n) {
    for (int i = 1; i < n; ++i)
        a[i] += a[i - 1];
}
```
✓ verified: `loop not vectorized: unsafe dependent memory operations in loop`.
Each element needs the one just computed. This is a true recurrence. It can be computed in parallel with a scan algorithm (log-step shifts within a register, then carry propagation across registers), but no compiler derives that automatically. `std::inclusive_scan` with `std::execution::unseq` or a hand-written SIMD scan is the way to go.

### Case B: distance 4, so it vectorizes at VF ≤ 4
```cpp
void lag4(float* a, int n) {
    for (int i = 4; i < n; ++i)
        a[i] = a[i - 4] * 2.0f;
}
```
✓ verified: `vectorized loop (vectorization width: 4, interleaved count: 1)`. The compiler picked exactly `VF = 4` and **disabled interleaving**, since interleaving would make 8 or 16 iterations simultaneous and break the distance-4 dependence.

### Case C: reductions are a special, breakable loop-carried dependency
```cpp
int   sum_i(const int*   a, int n) { int   s = 0; for (int i = 0; i < n; ++i) s += a[i]; return s; }
float sum_f(const float* a, int n) { float s = 0; for (int i = 0; i < n; ++i) s += a[i]; return s; }
```
`s` is carried across every iteration (distance 1), but it's a **reduction**: an associative op folding into one value. The vectorizer can keep `VF×IC` partial sums and combine them at the end. Whether that's *legal* depends on the type.

| | IR — ✓ verified (`-O2`) | Why |
|---|---|---|
| `sum_i` | `add <4 x i32>` per vector, then `addv.4s` horizontal sum at the end (arm64 asm) | Integer `+` is associative, so reordering is exact |
| `sum_f` (default) | `call float @llvm.vector.reduce.fadd.v4f32(float %acc, <4 x float> %v)` **per vector**, as an *ordered* (in-order) reduction | FP `+` is **not associative**: `(a+b)+c ≠ a+(b+c)` in general. The compiler must preserve the exact left-to-right order. The loads are vectorized, but the adds remain one serial chain |
| `sum_f` with `-ffast-math` | `fadd fast <4 x float>` into 4 independent vector accumulators | `-fassociative-math` (part of `-ffast-math`) permits reordering, so this is a true parallel reduction |

The in-order version is still bound by FP-add latency (`n × 3–4` cycles). The reassociated version is bound by throughput, roughly 4–16× faster for large `n`, **and gives a slightly different result**.

### Controls
| Control | Effect |
|---|---|
| `-fassociative-math -fno-signed-zeros -fno-trapping-math` | The minimal subset of `-ffast-math` needed for FP reductions |
| `#pragma clang loop vectorize(assume_safety)` | Treat memory dependences as safe (*your* responsibility) |
| `#pragma omp simd reduction(+:s)` | Explicitly permits reassociating this one reduction. Portable and scoped, so better than global fast-math |
| Manual multiple accumulators | `s0 += a[i]; s1 += a[i+1]; …`, deterministic, with no flags at all |
| `std::reduce` (unordered) vs `std::accumulate` (ordered) | `std::reduce` is *allowed* to reassociate, `std::accumulate` is not |

---

## 4.4 Early Exits

### Mechanism
A loop with a data-dependent `break`/`return` (an *uncountable* exit) has no trip count known on entry. To vectorize it, the compiler has to:
1. Load a full vector of `VF` elements, **possibly past the element that triggers the exit**.
2. Compare all lanes, and exit if any lane matches (`umaxv`/`ptest`/`movemask`).
3. Find the first matching lane (count-trailing-zeros on the mask).

Step 1 is the problem. Reading `a[i+1..i+3]` when `a[i]` was the last valid element could cross into an unmapped page and **fault**, even though the scalar code never touches that memory. LLVM recently added early-exit vectorization, which fires only when it can prove the speculative loads are **dereferenceable**.

### Unknown-size pointer: not vectorized
```cpp
int find_ptr(const int* a, int n, int key) {
    for (int i = 0; i < n; ++i)
        if (a[i] == key) return i;
    return -1;
}
```
✓ verified: `loop not vectorized: Cannot vectorize potentially faulting early exit loop`.

### Known-size array: vectorized
```cpp
int find_fixed(const int (&a)[256], int key) {
    for (int i = 0; i < 256; ++i)
        if (a[i] == key) return i;
    return -1;
}
```
✓ verified (`-O3`): `vectorized loop (vectorization width: 4, interleaved count: 1)`, plus `Interleaving not supported for loops with uncountable early exits`. The reference-to-array type proves all 1024 bytes are dereferenceable, so the speculative loads are safe.

**Hardware impact:** a vectorized search compares 4 (NEON), 8 (AVX2) or 16 (AVX-512) elements per compare, and turns `n` hard-to-predict branches into `n/VF` mostly-not-taken branches. This is how `memchr`/`strlen` in libc work, with hand-written aligned loads. An aligned 16-byte load can never cross a page boundary, so it can't fault.

### Controls / workarounds
| Approach | Effect |
|---|---|
| Pass `std::array<T,N>&` / `T(&)[N]`, or `std::span<T, N>` | Gives the compiler a dereferenceable extent |
| Sentinel or padding | Allocate `n + VF - 1` elements, so over-reading is legal by construction |
| "Search then confirm" | Vectorize a branch-free `any_of` over fixed chunks, then scan the matching chunk with scalar code |
| `std::find`, `memchr`, `std::ranges::find` | Library implementations are often hand-vectorized |
| `__builtin_assume_dereferenceable(p, bytes)` (Clang ≥ 21) | Asserts that the range is safe to read. ✓ verified: it compiles, but in Apple clang 21 it did **not** yet enable early-exit vectorization of `find_ptr`, so prefer the array-reference form |
