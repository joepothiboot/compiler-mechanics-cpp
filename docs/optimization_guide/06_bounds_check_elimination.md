# 6. Safety and Range Checks: Bounds-Check Elimination

Bounds checks (`std::vector::at`, `std::span` with hardening, `gsl::at`, Rust-style `operator[]` wrappers, libc++ `_LIBCPP_HARDENING_MODE`) are just ordinary branches to a `[[noreturn]]` failure path. The optimizer doesn't treat them specially. It removes them with the same machinery it uses for any other branch: **value-range analysis plus dominating conditions**. The practical lesson is that a safe container can cost nothing, as long as the check's condition is *implied by something the compiler can already see*.

## Mechanism

- **Branch propagation / correlated value propagation.** When control passes through `br (i < n), %then, %else`, every block dominated by `%then` knows `i <u n`. LLVM's `CorrelatedValuePropagation` (built on `LazyValueInfo`) and `ConstraintElimination` collect these facts from dominating branches and `assume`s, and then fold any later compare they imply. `ConstraintElimination` solves systems of linear inequalities, so it handles `i < j && j <= n ⇒ i < n`.
- **SSA is what makes it precise.** `i` is one SSA value, so a fact about `i` learned at a branch applies to *every* use of `i` in the dominated region. There's no question of whether `i` was reassigned in between.
- **Loops: SCEV ranges.** For `for (i = 0; i < size; ++i)`, SCEV knows `i ∈ [0, size)` inside the body. A check `i < size` is then provably true and folds away. `IndVarSimplify` and LoopVectorize both use this.
- **Loops whose bound is different from the check's bound: versioning.** If the loop runs to `n` but checks against `size`, the compiler can test `n <= size` *once* before the loop, run a check-free (and vectorizable) copy when it holds, and fall back to the checked loop when it doesn't. This is the same versioning idea as alias runtime checks (ch. 5). Java and .NET JITs call this *range-check elimination by loop predication*.
- **Overflow is the main obstacle.** For unsigned `size_t`, `i + 1 < n` does **not** imply `i < n`: if `i == SIZE_MAX`, then `i + 1 == 0`. Signed arithmetic with `nsw` (no signed wrap, because overflow is UB) gives the compiler more room.

---

## Case 1: loop bound equals the checked bound, so all checks go away

### Before
```cpp
int sum_at(const std::vector<int>& v) {
    int s = 0;
    for (size_t i = 0; i < v.size(); ++i)
        s += v.at(i);              // throws std::out_of_range if i >= size()
    return s;
}
```

### After — ✓ verified (`-O2`)
- The optimized IR has **zero references to `out_of_range` or `throw`**. The check `i < size()` is identical to the loop condition, which dominates the body.
- The loop is then **vectorized** (width 4, interleave 4), and that's only possible *because* the check is gone: a loop that might throw mid-iteration can't be widened.
- LICM also hoisted the loads of `v.__begin_` and `v.__end_` out of the loop (remarks: `hoisting load`).

The same result holds for a hand-written checked span:
```cpp
[[noreturn]] void oob();
struct Span {
    int* data; size_t size;
    int& operator[](size_t i) const { if (i >= size) oob(); return data[i]; }
};
int sum_span(Span s) { int t = 0; for (size_t i = 0; i < s.size; ++i) t += s[i]; return t; }
```
✓ verified: 0 calls to `oob` remain, and the loop is vectorized 4×4.

---

## Case 2: a dominating branch implies one check but not the other

```cpp
int pair_sum(Span s, size_t i) {
    if (i + 1 < s.size)
        return s[i] + s[i + 1];    // two checks: i < size, i+1 < size
    return 0;
}
```
✓ verified: the **`s[i+1]` check is removed** (it's exactly the dominating condition), but **the `s[i]` check stays**:
```llvm
  %6 = add i64 %i, 1
  %7 = icmp ult i64 %6, %size          ; the if-condition
  br i1 %7, label %then, label %ret0
then:
  %9 = icmp ult i64 %i, %size          ; s[i] check: still here!
  br i1 %9, label %ok, label %fail
fail:
  call void @oob()
```
**Why:** `size_t` wraps. If `i == SIZE_MAX`, then `i + 1 == 0 < size`, but `i` itself is out of bounds. The compiler is right to keep the check. Write the guard as `if (i < s.size && i + 1 < s.size)`, or as `if (s.size >= 2 && i <= s.size - 2)`, and both checks go away.

---

## Case 3: different loop bound, so the compiler versions the loop

```cpp
int sum_n(Span s, size_t n) {
    int t = 0;
    for (size_t i = 0; i < n; ++i) t += s[i];   // n vs s.size: unrelated
    return t;
}
```
✓ verified (`-O2`): the IR tests `size > n - 1` once in the preheader.
- **True:** a check-free, vectorized 4×4 loop (plus a vectorized epilogue).
- **False:** the original scalar loop, with the per-iteration check and the `oob()` call.

To get a single version, and to fail fast before any partial work happens, hoist the check yourself:
```cpp
int sum_n_hoisted(Span s, size_t n) {
    if (n > s.size) oob();                      // one check, before the loop
    int t = 0;
    for (size_t i = 0; i < n; ++i) t += s[i];   // dominated by n <= size, so check-free
    return t;
}
```
✓ verified: one `oob` call (the hoisted one), and the loop is vectorized.

---

## Hardware impact
- A well-predicted bounds check costs ~1 cycle of issue bandwidth: a compare and a not-taken branch, about 2 µops. In scalar code that's often in the noise, which is the argument for "always-on hardening".
- **The real cost is what the check blocks.** A potential exit in the middle of the loop body stops vectorization (an early exit, ch. 4), blocks reordering of memory operations around it (the exception must be thrown *before* later side effects), and keeps values live for the failure path. Removing the check is what *unlocks* the 4–16× SIMD speedup.
- Failure paths are moved out of line (`[[noreturn]]` and `cold` attributes) so they don't consume I-cache in the hot loop.

## Controls
| Control | Effect |
|---|---|
| Iterate with the container's own bound (`i < v.size()`, range-for, `std::span`) | The check becomes provably redundant, so it's free |
| Hoist validation: `if (n > s.size()) fail();` before the loop | Dominates the body, so each per-element check is removed |
| `[[noreturn]]` on the failure function | Lets the optimizer treat the failure path as an exit, so facts after the check are known to hold |
| `__builtin_assume(i < n)` (Clang), `[[assume(i < n)]]` (C++23), `__builtin_unreachable()` | Injects a fact without a check. **UB if false** |
| `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST` (libc++ ≥ 18) | Cheap bounds checks on `operator[]`, `span`, `string_view`, … |
| `-D_GLIBCXX_ASSERTIONS` (libstdc++) | The libstdc++ equivalent (bounds checks on `operator[]` etc.) |
| `-fsanitize=bounds` / `-fsanitize=array-bounds` | Compiler-inserted checks on fixed-size arrays (optimized by the same passes) |
| `-mllvm -enable-constraint-elimination` | Already on by default in modern LLVM |
| `-Rpass-missed=loop-vectorize` | If a checked loop doesn't vectorize, the remark often names the early exit |
