// 13_peephole.cpp — post-pass peephole optimization over machine code.
//
// After instruction selection, register allocation and phi lowering, the code
// still contains local inefficiencies those passes created or couldn't see:
// self-moves left by coalescing, round-trips through spill slots, jumps to
// the very next block. A PEEPHOLE pass slides a small window (1–3 instructions)
// over the code and replaces patterns with cheaper equivalents, repeating
// until nothing changes (one rewrite often enables another).
//
//   window ──▶ [ mov [rbp-8], rax ]          [ mov [rbp-8], rax ]
//              [ mov rcx, [rbp-8] ]    ═▶    [ mov rcx, rax     ]   store→load forwarding
//
// RULES IN THIS FILE (x86-64, Intel syntax)
//   mov r, 0            → xor r32, r32    shorter encoding (2–3 bytes vs 7), and
//                                          the renamer treats it as a dependency-
//                                          breaking zeroing idiom (no execution
//                                          unit). Writing r32 zero-extends to r64.
//   add/sub r, 0        → (deleted)
//   imul r, 2^k         → shl r, k        1-cycle shift instead of a 3-cycle multiply
//   imul r, 1           → (deleted)
//   mov r64, r64 (same) → (deleted)
//   mov a, b ; mov b, a → mov a, b        the second move is redundant
//   mov [m], r ; mov r2, [m] → mov [m], r ; mov r2, r
//   jmp L ; L:          → L:              jump to the fall-through block
//
// HARD TRUTHS: TWO WAYS TO GET A PEEPHOLE WRONG
//   1. FLAGS. `xor`, `add`, `shl` write EFLAGS; `mov` doesn't. Rewriting
//          cmp rbx, rcx
//          mov rax, 0        ← xor here would destroy the cmp's result
//          jne .L1
//      is a miscompile. Every flag-writing rewrite must first prove the flags
//      are DEAD: scanning forward, a flag writer must come before any flag
//      reader (jcc, setcc, cmovcc, adc, sbb). Compilers usually hoist the
//      xor above the cmp instead.
//   2. "OBVIOUS" NO-OPS THAT AREN'T. `mov eax, eax` is NOT a no-op on x86-64:
//      writing a 32-bit register zeroes bits 63:32. Compilers emit it on
//      purpose to zero-extend. Only same-register 64-bit moves can go.
//
// Superoptimizers (Massalin 1987, STOKE, Souper for LLVM IR) search for the
// optimal sequence for a window by brute force or SMT solving. LLVM's own
// machine-level peepholes live in PeepholeOptimizer, MachineCopyPropagation
// and per-target passes; InstCombine plays the same role at the IR level.

#include "test_support.h"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace peephole {

struct Instr {
  std::string op;                // "mov", "add", ...; labels use op "label"
  std::vector<std::string> args; // operands as written
  bool operator==(const Instr &) const = default;
};

// "mov rax, 0" → {mov, [rax, 0]};  ".L1:" → {label, [.L1]}
Instr parse(std::string_view text) {
  if (text.ends_with(':'))
    return {"label", {std::string(text.substr(0, text.size() - 1))}};
  const auto sp = text.find(' ');
  Instr i{std::string(text.substr(0, sp)), {}};
  if (sp == std::string_view::npos)
    return i;
  std::stringstream rest{std::string(text.substr(sp + 1))};
  for (std::string arg; std::getline(rest, arg, ',');) {
    arg.erase(0, arg.find_first_not_of(' '));
    i.args.push_back(arg);
  }
  return i;
}

std::string print(const Instr &i) {
  if (i.op == "label")
    return i.args[0] + ":";
  std::string s = i.op;
  for (std::size_t k = 0; k < i.args.size(); ++k)
    s += (k ? ", " : " ") + i.args[k];
  return s;
}

std::vector<Instr> parseAll(std::initializer_list<std::string_view> lines) {
  std::vector<Instr> out;
  for (auto l : lines)
    out.push_back(parse(l));
  return out;
}

std::vector<std::string> printAll(const std::vector<Instr> &code) {
  std::vector<std::string> out;
  for (const Instr &i : code)
    out.push_back(print(i));
  return out;
}

// --- Machine facts --------------------------------------------------------------

constexpr std::array<std::pair<std::string_view, std::string_view>, 16> kGpr32{{
    {"rax", "eax"}, {"rbx", "ebx"}, {"rcx", "ecx"}, {"rdx", "edx"},
    {"rsi", "esi"}, {"rdi", "edi"}, {"rbp", "ebp"}, {"rsp", "esp"},
    {"r8", "r8d"},   {"r9", "r9d"},   {"r10", "r10d"}, {"r11", "r11d"},
    {"r12", "r12d"}, {"r13", "r13d"}, {"r14", "r14d"}, {"r15", "r15d"},
}};

std::optional<std::string_view> low32(std::string_view r64) {
  for (auto [r, e] : kGpr32)
    if (r == r64)
      return e;
  return std::nullopt;
}
bool isReg64(std::string_view s) { return low32(s).has_value(); }
bool isMem(std::string_view s) { return s.starts_with('['); }

bool readsFlags(const Instr &i) {
  return (i.op.starts_with('j') && i.op != "jmp") || i.op.starts_with("set") ||
         i.op.starts_with("cmov") || i.op == "adc" || i.op == "sbb";
}
bool writesFlags(const Instr &i) {
  static constexpr std::array<std::string_view, 11> writers{
      "add", "sub", "and", "or", "xor", "cmp", "test", "shl", "shr", "imul", "neg"};
  return std::ranges::contains(writers, i.op);
}

// Flags are dead after `pos` if, along the fall-through path, a writer comes
// before any reader. Labels don't read anything, so the scan walks through
// them. An unconditional `jmp` leaves the path, so we don't know what its
// target reads and answer "maybe live". `ret` and the end of the function
// mean dead (EFLAGS is never live across a return).
bool flagsDeadAfter(std::span<const Instr> code, std::size_t pos) {
  for (std::size_t k = pos + 1; k < code.size(); ++k) {
    if (readsFlags(code[k]) || code[k].op == "jmp")
      return false;
    if (writesFlags(code[k]) || code[k].op == "ret")
      return true;
  }
  return true;
}

std::optional<int> log2Exact(std::string_view s) {
  long v = 0;
  try {
    v = std::stol(std::string(s));
  } catch (...) {
    return std::nullopt;
  }
  if (v <= 0 || (v & (v - 1)) != 0)
    return std::nullopt;
  return std::countr_zero(static_cast<unsigned long>(v));
}

// --- Rules ------------------------------------------------------------------------

// A rule looks at code[pos...] and either returns nothing, or the number of
// instructions it consumes plus their replacement.
struct Rewrite {
  std::size_t consumed;
  std::vector<Instr> replacement;
};
using Rule = std::optional<Rewrite> (*)(std::span<const Instr>, std::size_t);

std::optional<Rewrite> zeroIdiom(std::span<const Instr> c, std::size_t p) {
  const Instr &i = c[p];
  if (i.op == "mov" && i.args.size() == 2 && i.args[1] == "0" && isReg64(i.args[0]) &&
      flagsDeadAfter(c, p)) {
    const std::string r(*low32(i.args[0]));
    return Rewrite{1, {{"xor", {r, r}}}};
  }
  return std::nullopt;
}

std::optional<Rewrite> algebraicIdentity(std::span<const Instr> c, std::size_t p) {
  const Instr &i = c[p];
  if (i.args.size() != 2 || !flagsDeadAfter(c, p))
    return std::nullopt;
  if ((i.op == "add" || i.op == "sub") && i.args[1] == "0")
    return Rewrite{1, {}};
  if (i.op == "imul" && i.args[1] == "1")
    return Rewrite{1, {}};
  if (i.op == "imul")
    if (auto k = log2Exact(i.args[1]))
      return Rewrite{1, {{"shl", {i.args[0], std::to_string(*k)}}}};
  return std::nullopt;
}

std::optional<Rewrite> selfMove(std::span<const Instr> c, std::size_t p) {
  const Instr &i = c[p];
  // Only 64-bit: `mov eax, eax` zero-extends and must stay.
  if (i.op == "mov" && i.args.size() == 2 && i.args[0] == i.args[1] && isReg64(i.args[0]))
    return Rewrite{1, {}};
  return std::nullopt;
}

std::optional<Rewrite> redundantMoveBack(std::span<const Instr> c, std::size_t p) {
  if (p + 1 >= c.size())
    return std::nullopt;
  const Instr &a = c[p], &b = c[p + 1];
  if (a.op == "mov" && b.op == "mov" && a.args.size() == 2 && b.args.size() == 2 &&
      a.args[0] == b.args[1] && a.args[1] == b.args[0] && isReg64(a.args[0]) &&
      isReg64(a.args[1]))
    return Rewrite{2, {a}};
  return std::nullopt;
}

std::optional<Rewrite> storeLoadForward(std::span<const Instr> c, std::size_t p) {
  if (p + 1 >= c.size())
    return std::nullopt;
  const Instr &st = c[p], &ld = c[p + 1];
  if (st.op == "mov" && ld.op == "mov" && st.args.size() == 2 && ld.args.size() == 2 &&
      isMem(st.args[0]) && isReg64(st.args[1]) && ld.args[1] == st.args[0] &&
      isReg64(ld.args[0]))
    return Rewrite{2, {st, {"mov", {ld.args[0], st.args[1]}}}};
  return std::nullopt;
}

std::optional<Rewrite> jumpToNext(std::span<const Instr> c, std::size_t p) {
  if (p + 1 < c.size() && c[p].op == "jmp" && c[p + 1].op == "label" &&
      c[p].args[0] == c[p + 1].args[0])
    return Rewrite{1, {}};
  return std::nullopt;
}

constexpr std::array<Rule, 6> kRules{zeroIdiom,         algebraicIdentity, selfMove,
                                     redundantMoveBack, storeLoadForward,  jumpToNext};

// One left-to-right sweep per round; rounds repeat until a fixed point.
std::vector<Instr> optimize(std::vector<Instr> code, int *rounds = nullptr) {
  int r = 0;
  for (bool changed = true; changed; ++r) {
    changed = false;
    std::vector<Instr> out;
    for (std::size_t p = 0; p < code.size();) {
      std::optional<Rewrite> rw;
      for (Rule rule : kRules)
        if ((rw = rule(code, p)))
          break;
      if (rw && !(rw->consumed == 1 && rw->replacement == std::vector{code[p]})) {
        out.insert(out.end(), rw->replacement.begin(), rw->replacement.end());
        p += rw->consumed;
        changed = true;
      } else {
        out.push_back(code[p++]);
      }
    }
    code = std::move(out);
  }
  if (rounds)
    *rounds = r;
  return code;
}

} // namespace peephole

using namespace peephole;
using Lines = std::vector<std::string>;

void testParseRoundTrip() {
  for (auto s : {"mov rax, [rbp - 8]", "ret", ".L1:", "imul rcx, 8"})
    CHECK(print(parse(s)) == s);
}

void testZeroIdiom() {
  CHECK(printAll(optimize(parseAll({"mov rax, 0", "ret"}))) == (Lines{"xor eax, eax", "ret"}));
  CHECK(printAll(optimize(parseAll({"mov r9, 0"}))) == Lines{"xor r9d, r9d"});
}

void testZeroIdiomRespectsLiveFlags() {
  // xor between the cmp and the jne would change which way the branch goes.
  const auto code = parseAll({"cmp rbx, rcx", "mov rax, 0", "jne .L1", "ret", ".L1:"});
  CHECK(printAll(optimize(code)) == printAll(code));

  // A flag writer before the reader kills the cmp's flags: now it's safe.
  CHECK(printAll(optimize(parseAll({"mov rax, 0", "cmp rbx, rcx", "jne .L1"}))) ==
        (Lines{"xor eax, eax", "cmp rbx, rcx", "jne .L1"}));
}

void testAlgebraicRules() {
  CHECK(printAll(optimize(parseAll({"imul rcx, 8", "ret"}))) == (Lines{"shl rcx, 3", "ret"}));
  CHECK(printAll(optimize(parseAll({"imul rcx, 6", "ret"}))) == (Lines{"imul rcx, 6", "ret"}));
  CHECK(printAll(optimize(parseAll({"add rdx, 0", "imul rdx, 1", "ret"}))) == Lines{"ret"});
}

void testSelfMoveOnlyFor64Bit() {
  CHECK(printAll(optimize(parseAll({"mov rax, rax", "ret"}))) == Lines{"ret"});
  // mov eax, eax clears the upper 32 bits of rax: it must survive.
  CHECK(printAll(optimize(parseAll({"mov eax, eax", "ret"}))) == (Lines{"mov eax, eax", "ret"}));
}

void testStoreLoadForwarding() {
  CHECK(printAll(optimize(parseAll({"mov [rbp - 8], rax", "mov rcx, [rbp - 8]"}))) ==
        (Lines{"mov [rbp - 8], rax", "mov rcx, rax"}));
  // Reloading into the same register: forwarding yields a self-move, which a
  // second round deletes.
  int rounds = 0;
  CHECK(printAll(optimize(parseAll({"mov [rbp - 8], rax", "mov rax, [rbp - 8]"}), &rounds)) ==
        Lines{"mov [rbp - 8], rax"});
  CHECK(rounds == 3); // forward, delete self-move, confirm fixed point
}

void testCascadeToFixedPoint() {
  // Typical post-regalloc debris: a copy pair from phi lowering, an identity
  // add, and a jump to the next block. `add rbx, 0` can only go once the jmp
  // is gone (round 1), because flagsDeadAfter can't see past a jmp (round 2).
  const auto code = parseAll({"mov rax, rbx", "mov rbx, rax", "add rbx, 0", "jmp .L1", ".L1:",
                              "mov rax, 0", "ret"});
  CHECK(printAll(optimize(code)) == (Lines{"mov rax, rbx", ".L1:", "xor eax, eax", "ret"}));
}

int main() {
  testParseRoundTrip();
  testZeroIdiom();
  testZeroIdiomRespectsLiveFlags();
  testAlgebraicRules();
  testSelfMoveOnlyFor64Bit();
  testStoreLoadForwarding();
  testCascadeToFixedPoint();
  return ts::report("13_peephole");
}

// ---------------------------------------------------------------------------
// CHECKPOINT
//
// Q1. storeLoadForward keeps the store. When could the peephole also delete
//     it, and why can't a 2-instruction window ever prove that? (Hint: who
//     else might read [rbp - 8]? Think about escape analysis from the guide's
//     ch. 5 and about the window size.)
//
// Q2. `add rax, 1` → `inc rax` is a shorter encoding, but some compilers avoid
//     it when tuning for older Intel cores. inc leaves CF unchanged; why does
//     a *partial* flags update cost anything on an out-of-order core? What
//     extra condition would the rule need to be safe and fast?
//
// CHALLENGE: add the rule `lea r, [r + imm]` → `add r, imm` (shorter, but it
//     writes flags, so check flagsDeadAfter), and its inverse for when flags
//     are live: `add r, imm` → `lea r, [r + imm]`, which doesn't touch flags.
//     Write a test where each direction fires, and one where neither may.
