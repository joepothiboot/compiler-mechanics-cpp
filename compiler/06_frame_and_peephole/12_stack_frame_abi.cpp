// 12_stack_frame_abi.cpp — activation records and the System V AMD64 ABI.
//
// WHY STACK FRAMES: RECURSION
//   Early FORTRAN gave every function's locals a fixed (static) address. That
//   is fast, but each function has exactly one copy of its locals, so a
//   recursive call overwrites its caller's variables (factStatic() below
//   returns 1 for any n). An ACTIVATION RECORD (stack frame) per call gives
//   every invocation private storage, at the cost of a stack pointer adjustment.
//
// THE FRAME (x86-64, growing downward), for a non-leaf function with a frame
// pointer:
//
//     higher addresses
//   ┌──────────────────────────┐
//   │ caller's stack args 7+   │  [rbp+16], [rbp+24], ...   (first at [rsp+8] on entry)
//   ├──────────────────────────┤
//   │ return address           │  pushed by `call`: on entry rsp ≡ 8 (mod 16)
//   ├──────────────────────────┤ ◀── rbp after `push rbp; mov rbp, rsp`
//   │ saved rbp                │
//   │ saved rbx, r12..r15      │  callee-saved registers this function clobbers
//   │ locals / spill slots     │  (10's spill slots live here)
//   │ padding                  │  so that rsp ≡ 0 (mod 16) at every call
//   │ outgoing stack args      │  [rsp], [rsp+8], ...
//   └──────────────────────────┘ ◀── rsp
//   │ red zone: 128 bytes      │  leaf functions may use [rsp-128, rsp) without
//   └──────────────────────────┘  moving rsp: signal handlers won't clobber it
//     lower addresses
//
// THE RULES THIS FILE IMPLEMENTS (all checked against clang -O2 --target=x86_64-linux-gnu):
//   * Integer/pointer args: rdi, rsi, rdx, rcx, r8, r9. FP args: xmm0–xmm7.
//     Anything else goes on the stack, 8-byte slots, first one at [rsp+8].
//   * Aggregates ≤ 16 bytes are split into EIGHTBYTES. Each one is INTEGER if
//     any field in it is an integer, otherwise SSE. {double, long} → xmm0 + rdi.
//     Larger aggregates are MEMORY (passed on the stack).
//   * All-or-nothing: if an aggregate needs 2 GPRs but only 1 is left, the
//     WHOLE aggregate goes on the stack, and a later scalar still gets that
//     last register.
//   * MEMORY return values: the caller passes a hidden pointer in rdi (every
//     other argument shifts by one), and the callee returns it in rax.
//   * Caller-saved (volatile): rax rcx rdx rsi rdi r8–r11, xmm0–15.
//     Callee-saved: rbx rbp r12–r15. The register allocator prefers callee-
//     saved registers for values live across calls: one push/pop pair in the
//     prologue/epilogue beats a save/restore around every call.
//   * rsp must be 16-byte aligned at every `call` (SSE spills use movaps).
//   * Red zone: a function that makes no calls may use the 128 bytes below
//     rsp without `sub rsp`. Kernels compile with -mno-red-zone, because
//     interrupts arrive on the same stack and would trash it.

#include "test_support.h"

#include <algorithm>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace abi {

// --- Argument classification ----------------------------------------------------

enum class Kind { Int, Float };
struct Field {
  Kind kind;
  int size; // 1, 2, 4, 8; naturally aligned
};
using Type = std::vector<Field>; // a scalar is a one-field type

enum class Class { Integer, Sse, Memory };

constexpr int alignTo(int x, int a) { return (x + a - 1) / a * a; }

int sizeOf(const Type &t) {
  int off = 0, align = 1;
  for (const Field &f : t) {
    off = alignTo(off, f.size) + f.size;
    align = std::max(align, f.size);
  }
  return alignTo(off, align);
}

// One class per eightbyte, or {Memory}.
std::vector<Class> classify(const Type &t) {
  const int size = sizeOf(t);
  if (size > 16)
    return {Class::Memory};
  std::vector<Class> eb(alignTo(size, 8) / 8, Class::Sse);
  int off = 0;
  for (const Field &f : t) {
    off = alignTo(off, f.size);
    if (f.kind == Kind::Int)
      eb[off / 8] = Class::Integer; // INTEGER wins over SSE within an eightbyte
    off += f.size;
  }
  return eb;
}

struct Location {
  std::vector<std::string> regs;  // empty → on the stack
  std::optional<int> stackOffset; // relative to rsp at function entry
};

struct CallLowering {
  std::vector<Location> args;
  bool hiddenReturnPointer = false;
};

CallLowering lowerCall(const std::vector<Type> &params, const Type &ret = {}) {
  static const char *const gprs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
  CallLowering out;
  int nextGpr = 0, nextSse = 0, stack = 8; // [rsp] holds the return address

  if (!ret.empty() && classify(ret).front() == Class::Memory) {
    out.hiddenReturnPointer = true;
    nextGpr = 1; // rdi carries the return buffer
  }

  for (const Type &t : params) {
    const auto cls = classify(t);
    const auto needGpr = std::ranges::count(cls, Class::Integer);
    const auto needSse = std::ranges::count(cls, Class::Sse);
    Location loc;
    if (cls.front() != Class::Memory && nextGpr + needGpr <= 6 && nextSse + needSse <= 8) {
      for (Class c : cls)
        loc.regs.push_back(c == Class::Integer ? gprs[nextGpr++]
                                               : "xmm" + std::to_string(nextSse++));
    } else {
      loc.stackOffset = stack; // all-or-nothing: the whole argument goes to memory
      stack += alignTo(sizeOf(t), 8);
    }
    out.args.push_back(std::move(loc));
  }
  return out;
}

// --- Frame layout ---------------------------------------------------------------

struct FrameRequest {
  std::vector<int> localSizes; // 8-byte aligned slots for simplicity
  std::vector<std::string> calleeSaved;
  bool makesCalls = false;
  bool framePointer = false;
  bool redZoneAllowed = true; // false = -mno-red-zone (kernel code)
  int outgoingArgBytes = 0;
};

struct FrameLayout {
  int subRsp = 0;
  bool usesRedZone = false;
  std::vector<int> localOffsets; // relative to rsp after the prologue
  std::vector<std::string> prologue, epilogue;
  int rspMod16AtCalls = -1;      // must be 0 if the function makes calls
};

FrameLayout layoutFrame(const FrameRequest &r) {
  FrameLayout f;
  const int localBytes =
      std::accumulate(r.localSizes.begin(), r.localSizes.end(), 0,
                      [](int acc, int s) { return acc + alignTo(s, 8); });

  if (r.framePointer)
    f.prologue = {"push rbp", "mov rbp, rsp"};
  for (const auto &reg : r.calleeSaved)
    f.prologue.push_back("push " + reg);
  const int pushes = (r.framePointer ? 1 : 0) + static_cast<int>(r.calleeSaved.size());

  f.usesRedZone = !r.makesCalls && r.redZoneAllowed && localBytes <= 128;
  if (f.usesRedZone) {
    int off = 0; // locals live below rsp
    for (int s : r.localSizes)
      f.localOffsets.push_back(-(off += alignTo(s, 8)));
  } else {
    int n = localBytes + r.outgoingArgBytes;
    if (r.makesCalls) {
      // At entry rsp ≡ 8 (mod 16). After `pushes` pushes and `sub n`, we need
      // (8 + 8*pushes + n) ≡ 0 (mod 16).
      while ((8 + 8 * pushes + n) % 16 != 0)
        n += 8;
      f.rspMod16AtCalls = (8 + 8 * pushes + n) % 16;
    }
    f.subRsp = n;
    int off = r.outgoingArgBytes;
    for (int s : r.localSizes) {
      f.localOffsets.push_back(off);
      off += alignTo(s, 8);
    }
    if (n > 0)
      f.prologue.push_back("sub rsp, " + std::to_string(n));
  }

  if (f.subRsp > 0)
    f.epilogue.push_back("add rsp, " + std::to_string(f.subRsp));
  for (auto it = r.calleeSaved.rbegin(); it != r.calleeSaved.rend(); ++it)
    f.epilogue.push_back("pop " + *it);
  if (r.framePointer)
    f.epilogue.push_back("pop rbp");
  f.epilogue.push_back("ret");
  return f;
}

// --- Static allocation vs activation records --------------------------------------

long factStatic(long n) {
  static long slot; // one copy of "n" shared by every invocation
  slot = n;
  if (slot <= 1)
    return 1;
  const long r = factStatic(slot - 1); // the recursive call overwrites slot...
  return slot * r;                     // ...so this reads 1, not n
}

long factStack(long n) { return n <= 1 ? 1 : n * factStack(n - 1); }

} // namespace abi

using namespace abi;

const Type i32{{Kind::Int, 4}}, i64{{Kind::Int, 8}};
const Type f32{{Kind::Float, 4}}, f64{{Kind::Float, 8}};

void testScalarArguments() {
  const auto c = lowerCall({i32, f64, i64, f32});
  CHECK(c.args[0].regs == std::vector<std::string>{"rdi"});
  CHECK(c.args[1].regs == std::vector<std::string>{"xmm0"}); // separate counters
  CHECK(c.args[2].regs == std::vector<std::string>{"rsi"});
  CHECK(c.args[3].regs == std::vector<std::string>{"xmm1"});
}

void testSeventhIntegerArgumentOnStack() {
  const auto c = lowerCall({i64, i64, i64, i64, i64, i64, i64, i64});
  CHECK(c.args[5].regs == std::vector<std::string>{"r9"});
  CHECK(c.args[6].stackOffset == 8);  // [rsp+8]: just above the return address
  CHECK(c.args[7].stackOffset == 16);
}

void testAggregateClassification() {
  const Type dl{{Kind::Float, 8}, {Kind::Int, 8}};
  CHECK(classify(dl) == (std::vector<Class>{Class::Sse, Class::Integer}));
  CHECK(lowerCall({dl}).args[0].regs == (std::vector<std::string>{"xmm0", "rdi"}));

  // {float, float} share eightbyte 0 (SSE); {int, int} share eightbyte 1 (INTEGER).
  const Type ffii{{Kind::Float, 4}, {Kind::Float, 4}, {Kind::Int, 4}, {Kind::Int, 4}};
  CHECK(lowerCall({ffii}).args[0].regs == (std::vector<std::string>{"xmm0", "rdi"}));

  // {float, int} in ONE eightbyte: INTEGER wins.
  CHECK(classify({{Kind::Float, 4}, {Kind::Int, 4}}) == std::vector<Class>{Class::Integer});

  const Type big{{Kind::Int, 8}, {Kind::Int, 8}, {Kind::Int, 8}};
  CHECK(classify(big) == std::vector<Class>{Class::Memory});
  CHECK(lowerCall({big}).args[0].stackOffset == 8);
}

void testAllOrNothingRule() {
  // spill(long a..e, LL s, long f): s needs 2 GPRs, but only r9 is left.
  const Type ll{{Kind::Int, 8}, {Kind::Int, 8}};
  const auto c = lowerCall({i64, i64, i64, i64, i64, ll, i64});
  CHECK(c.args[5].regs.empty());
  CHECK(c.args[5].stackOffset == 8);                       // whole struct on the stack
  CHECK(c.args[6].regs == std::vector<std::string>{"r9"}); // the later scalar gets r9
}

void testHiddenReturnPointer() {
  const Type big{{Kind::Int, 8}, {Kind::Int, 8}, {Kind::Int, 8}};
  const auto c = lowerCall({i64}, big);
  CHECK(c.hiddenReturnPointer);
  CHECK(c.args[0].regs == std::vector<std::string>{"rsi"}); // rdi is taken
}

void testNonLeafAlignment() {
  // Matches clang's `sub rsp, 24` for a non-leaf with 16 bytes of locals.
  const auto f = layoutFrame({.localSizes = {8, 8}, .makesCalls = true});
  CHECK(f.subRsp == 24);
  CHECK(f.rspMod16AtCalls == 0);

  // Frame pointer + 3 callee-saved pushes + 20 bytes of locals.
  const auto g = layoutFrame({.localSizes = {20},
                              .calleeSaved = {"rbx", "r12", "r13"},
                              .makesCalls = true,
                              .framePointer = true});
  CHECK(g.subRsp == 24); // 8 + 4*8 + 24 = 64 ≡ 0
  CHECK(g.rspMod16AtCalls == 0);
  CHECK(g.prologue == (std::vector<std::string>{"push rbp", "mov rbp, rsp", "push rbx",
                                                "push r12", "push r13", "sub rsp, 24"}));
  CHECK(g.epilogue == (std::vector<std::string>{"add rsp, 24", "pop r13", "pop r12",
                                                "pop rbx", "pop rbp", "ret"}));
}

void testAlignmentInvariantHoldsForAllShapes() {
  for (int pushes = 0; pushes <= 6; ++pushes)
    for (int locals = 0; locals <= 64; locals += 8)
      for (int outgoing : {0, 8, 16, 24}) {
        FrameRequest r{.makesCalls = true, .outgoingArgBytes = outgoing};
        r.calleeSaved.assign(pushes, "rbx");
        if (locals)
          r.localSizes = {locals};
        CHECK(layoutFrame(r).rspMod16AtCalls == 0);
      }
}

void testRedZone() {
  const auto leaf = layoutFrame({.localSizes = {8, 8, 8, 8, 8}});
  CHECK(leaf.usesRedZone);
  CHECK(leaf.subRsp == 0);
  CHECK(leaf.prologue.empty());
  CHECK(leaf.localOffsets.back() == -40); // like clang's [rsp - 8] .. [rsp - 40]

  const auto tooBig = layoutFrame({.localSizes = {200}});
  CHECK(!tooBig.usesRedZone);
  CHECK(tooBig.subRsp == 200);

  const auto kernel = layoutFrame({.localSizes = {8}, .redZoneAllowed = false});
  CHECK(!kernel.usesRedZone);
  CHECK(kernel.subRsp == 8);
}

void testRecursionNeedsActivationRecords() {
  CHECK(factStack(5) == 120);
  CHECK(factStatic(5) == 1); // every frame shared one "n"
}

int main() {
  testScalarArguments();
  testSeventhIntegerArgumentOnStack();
  testAggregateClassification();
  testAllOrNothingRule();
  testHiddenReturnPointer();
  testNonLeafAlignment();
  testAlignmentInvariantHoldsForAllShapes();
  testRedZone();
  testRecursionNeedsActivationRecords();
  return ts::report("12_stack_frame_abi");
}

// ---------------------------------------------------------------------------
// CHECKPOINT
//
// Q1. A function keeps a value live across 3 calls. Option A: put it in a
//     caller-saved register and save/restore it around each call (3 stores +
//     3 loads). Option B: put it in rbx (push in the prologue, pop in the
//     epilogue). When is A better? (Hint: what if all 3 calls are on a cold
//     path and the hot path makes none? This is "shrink-wrapping".)
//
// Q2. Why may a leaf function use the red zone, but a function that calls
//     another may not use it across that call? And why is the red zone 128
//     bytes on x86-64 and nonexistent (by default) on Windows x64?
//
// CHALLENGE: add `alignas(32)` locals (AVX spill slots). With 32-byte
//     alignment, rsp-relative offsets aren't enough when the frame size isn't
//     known statically (alloca / VLAs). Implement "stack realignment": keep
//     rbp as the frame pointer, `and rsp, -32`, and address the aligned
//     locals from rsp and the incoming args from rbp. Test that every 32-byte
//     local ends up 32-byte aligned for all entry alignments.
