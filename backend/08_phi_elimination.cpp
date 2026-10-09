// 08_phi_elimination.cpp — leaving SSA: phi elimination via parallel copies.
//
// Machines have no phi instruction. Before (or during) register allocation, a
//   x = phi [a, %P1], [b, %P2]
// becomes a copy on each incoming edge: "x <- a" at the end of P1, "x <- b" at
// the end of P2. Two classic bugs come from doing this naively:
//
// 1. LOST-COPY PROBLEM (copy placed on a critical edge)
//    An edge P->S is *critical* if P has several successors and S has several
//    predecessors. A copy placed at the end of P runs on EVERY path out of P,
//    including the ones that don't go to S:
//
//        B1: x1 = 0                        B1: x1 = 0
//             |                                 |
//             v                                 v
//        B2: x2 = phi(x1, x3) <--+         B2: x3 = x2 + 1  <----+
//            x3 = x2 + 1         |             if x3<3 ---> [split: x2 <- x3]
//            if x3 < 3 ----------+              |
//             | else                            v
//             v                            B3: return x2   (still the OLD x2)
//        B3: return x2
//
//    Naively putting "x2 <- x3" at the end of B2 also runs it on the way to B3,
//    which then returns the *new* value. The fix is to SPLIT the edge: insert an
//    empty block on it and put the copy there.
//
// 2. SWAP PROBLEM (phis read in parallel, copies written sequentially)
//    All phis at the top of a block read their inputs *simultaneously*:
//        a2 = phi(a1, b2)
//        b2 = phi(b1, a2)        back-edge parallel copy: { a2 <- b2, b2 <- a2 }
//    Emitting "a2 = b2; b2 = a2" makes both equal. A parallel copy has to be
//    SEQUENTIALIZED: emit a move only when no pending move still reads its
//    destination, and break any remaining cycle with a temporary:
//        tmp = a2; a2 = b2; b2 = tmp
//
// After register allocation, many of these copies become "r3 <- r3" (both sides
// got the same physical register, which is exactly what the coalescer aims for).
// Those SELF-MOVES are deleted. LLVM does this phi lowering in PHIElimination
// (plus TwoAddressInstruction), and RegisterCoalescer then removes the copies.
//
// HARD TRUTH: minimizing the copies is NP-hard in general (it's equivalent to
// optimal register coalescing). Production compilers place copies naively and
// rely on aggressive coalescing to delete most of them.

#include "test_support.h"

#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <vector>

namespace phielim {

using Reg = int;
constexpr Reg kNoReg = -1;

struct Move {
  Reg dst, src;
  bool operator==(const Move &) const = default;
};

struct Phi {
  Reg dst;
  std::map<int, Reg> incoming; // predecessor block index -> source register
};

// dst = src + imm, or dst = imm when src == kNoReg.
struct Inst {
  Reg dst;
  Reg src;
  int imm;
};

struct Block {
  std::string name;
  std::vector<Phi> phis;
  std::vector<Inst> insts;
  std::vector<Move> copies; // executed after the branch condition is evaluated
  std::vector<int> succs;   // 0 = return, 1 = jump, 2 = cond ? succs[0] : succs[1]
  Reg condReg = kNoReg;     // branch condition: condReg < condLimit
  int condLimit = 0;
  Reg ret = kNoReg;
};

struct Function {
  std::vector<Block> blocks;

  int numPreds(int b) const {
    return static_cast<int>(std::ranges::count_if(blocks, [b](const Block &p) {
      return std::ranges::find(p.succs, b) != p.succs.end();
    }));
  }
  bool isCritical(int from, int to) const {
    return blocks[from].succs.size() > 1 && numPreds(to) > 1;
  }
};

// --- Parallel-copy sequentialization ---------------------------------------
//
// Precondition: destinations are distinct (a parallel copy writes each register
// at most once). Self-moves are dropped. `tmp` must not appear in `pc`.
std::vector<Move> sequentialize(std::vector<Move> pc, Reg tmp) {
  std::erase_if(pc, [](const Move &m) { return m.dst == m.src; });
  std::vector<Move> out;
  while (!pc.empty()) {
    // A move is safe to emit if nobody still pending needs its dst's old value.
    auto ready = std::ranges::find_if(pc, [&](const Move &m) {
      return std::ranges::none_of(pc, [&](const Move &o) { return o.src == m.dst; });
    });
    if (ready != pc.end()) {
      out.push_back(*ready);
      pc.erase(ready);
      continue;
    }
    // Only cycles remain: every destination is still somebody's source.
    // Save one destination's old value into tmp and redirect its readers.
    const Reg victim = pc.front().dst;
    out.push_back({tmp, victim});
    for (Move &m : pc)
      if (m.src == victim)
        m.src = tmp;
  }
  return out;
}

// --- Phi lowering -------------------------------------------------------------

struct LowerOptions {
  bool splitCriticalEdges = true; // false reproduces the lost-copy bug
  bool parallelCopies = true;     // false reproduces the swap bug
  Reg tmp = 1000;
};

// Returns the number of edges that were split.
int lowerPhis(Function &f, LowerOptions opt = {}) {
  int split = 0;
  if (opt.splitCriticalEdges) {
    // Split any edge into a phi block whose source has several successors.
    // (Edges where the phi block has one predecessor are split too: the copy
    // would otherwise run on the source's other out-edges as well.)
    const int n = static_cast<int>(f.blocks.size());
    for (int p = 0; p < n; ++p) {
      if (f.blocks[p].succs.size() < 2)
        continue;
      // Index, not reference: push_back below may reallocate f.blocks.
      for (std::size_t k = 0; k < f.blocks[p].succs.size(); ++k) {
        const int s = f.blocks[p].succs[k];
        if (f.blocks[s].phis.empty())
          continue;
        const int mid = static_cast<int>(f.blocks.size());
        Block splitBlock{.name = f.blocks[p].name + "." + f.blocks[s].name + ".split",
                         .succs = {s}};
        f.blocks.push_back(std::move(splitBlock));
        for (Phi &phi : f.blocks[s].phis) {
          phi.incoming[mid] = phi.incoming.at(p);
          phi.incoming.erase(p);
        }
        f.blocks[p].succs[k] = mid;
        ++split;
      }
    }
  }

  for (int s = 0; s < static_cast<int>(f.blocks.size()); ++s) {
    // Group incoming values by predecessor: one parallel copy per edge.
    std::map<int, std::vector<Move>> perPred;
    for (const Phi &phi : f.blocks[s].phis)
      for (auto [pred, src] : phi.incoming)
        perPred[pred].push_back({phi.dst, src});
    for (auto &[pred, pc] : perPred) {
      auto seq = opt.parallelCopies ? sequentialize(pc, opt.tmp) : pc;
      auto &dst = f.blocks[pred].copies;
      dst.insert(dst.end(), seq.begin(), seq.end());
    }
    f.blocks[s].phis.clear();
  }
  return split;
}

// --- Post-allocation self-move elimination -------------------------------------

// `assignment` maps virtual register -> physical register. Returns how many
// moves were deleted because both sides landed in the same physical register.
int removeSelfMoves(std::vector<Move> &moves, const std::map<Reg, int> &assignment) {
  const auto before = moves.size();
  std::erase_if(moves, [&](const Move &m) {
    return assignment.at(m.dst) == assignment.at(m.src);
  });
  return static_cast<int>(before - moves.size());
}

// --- Interpreter (works on SSA and on lowered code) ---------------------------

std::optional<int> run(const Function &f, int maxSteps = 10'000) {
  std::map<Reg, int> regs;
  int cur = 0, prev = -1;
  for (int step = 0; step < maxSteps; ++step) {
    const Block &b = f.blocks[cur];
    std::vector<std::pair<Reg, int>> phiVals; // phis read in parallel
    for (const Phi &phi : b.phis)
      phiVals.emplace_back(phi.dst, regs[phi.incoming.at(prev)]);
    for (auto [r, v] : phiVals)
      regs[r] = v;
    for (const Inst &i : b.insts)
      regs[i.dst] = (i.src == kNoReg ? 0 : regs[i.src]) + i.imm;
    if (b.succs.empty())
      return regs[b.ret];
    const int next = b.succs.size() == 1                  ? b.succs[0]
                     : regs[b.condReg] < b.condLimit ? b.succs[0]
                                                          : b.succs[1];
    for (const Move &m : b.copies)
      regs[m.dst] = regs[m.src];
    prev = cur;
    cur = next;
  }
  return std::nullopt;
}

} // namespace phielim

using namespace phielim;

// Registers: x1=1, x2=2, x3=3. Correct result: 2.
Function lostCopyExample() {
  Function f;
  f.blocks = {
      {.name = "B1", .insts = {{1, kNoReg, 0}}, .succs = {1}},
      {.name = "B2",
       .phis = {{2, {{0, 1}, {1, 3}}}},
       .insts = {{3, 2, 1}},
       .succs = {1, 2},
       .condReg = 3,
       .condLimit = 3},
      {.name = "B3", .ret = 2},
  };
  return f;
}

// a1=1 b1=2 i1=0 | loop: a2,b2 swap; i counts to 3 | return a2. Correct: 1.
Function swapExample() {
  enum : Reg { a1 = 1, b1, i1, a2, b2, i2, i3 };
  Function f;
  f.blocks = {
      {.name = "B0", .insts = {{a1, kNoReg, 1}, {b1, kNoReg, 2}, {i1, kNoReg, 0}}, .succs = {1}},
      {.name = "B1",
       .phis = {{a2, {{0, a1}, {1, b2}}}, {b2, {{0, b1}, {1, a2}}}, {i2, {{0, i1}, {1, i3}}}},
       .insts = {{i3, i2, 1}},
       .succs = {1, 2},
       .condReg = i3,
       .condLimit = 3},
      {.name = "B2", .ret = a2},
  };
  return f;
}

void testSequentializeSwap() {
  const auto seq = sequentialize({{1, 2}, {2, 1}}, 99);
  CHECK(seq.size() == 3); // tmp = r1; r1 = r2; r2 = tmp
  CHECK(seq.front() == (Move{99, 1}));
}

void testSequentializeChainNeedsNoTemp() {
  // r3 <- r2, r2 <- r1: emit r3 <- r2 first, otherwise r2's old value is lost.
  const auto seq = sequentialize({{2, 1}, {3, 2}}, 99);
  CHECK(seq == (std::vector<Move>{{3, 2}, {2, 1}}));
  CHECK(sequentialize({{4, 4}}, 99).empty()); // self-move dropped
}

// Property test: sequential execution must equal parallel semantics.
void testSequentializeRandomized() {
  std::mt19937 rng(12345);
  for (int trial = 0; trial < 500; ++trial) {
    std::vector<Reg> dsts{0, 1, 2, 3, 4, 5};
    std::ranges::shuffle(dsts, rng);
    dsts.resize(rng() % 6 + 1);
    std::vector<Move> pc;
    for (Reg d : dsts)
      pc.push_back({d, static_cast<Reg>(rng() % 6)});

    std::map<Reg, int> par, seq;
    for (Reg r = 0; r < 6; ++r)
      par[r] = seq[r] = 100 + r;
    const auto old = par;
    for (const Move &m : pc)
      par[m.dst] = old.at(m.src);
    for (const Move &m : sequentialize(pc, 99))
      seq[m.dst] = seq[m.src];
    seq.erase(99);
    CHECK_MSG(par == seq, "trial " + std::to_string(trial));
  }
}

void testLostCopy() {
  CHECK(run(lostCopyExample()) == 2); // SSA semantics
  CHECK(lostCopyExample().isCritical(1, 1));

  Function naive = lostCopyExample();
  lowerPhis(naive, {.splitCriticalEdges = false});
  CHECK(run(naive) == 3); // the copy also ran on the exit edge: wrong

  Function fixed = lostCopyExample();
  CHECK(lowerPhis(fixed) == 1); // the back edge was split
  CHECK(run(fixed) == 2);
}

void testSwap() {
  CHECK(run(swapExample()) == 1);

  Function naive = swapExample();
  lowerPhis(naive, {.parallelCopies = false});
  CHECK(run(naive) == 2); // a2 = b2; b2 = a2 lost the swap

  Function fixed = swapExample();
  lowerPhis(fixed);
  CHECK(run(fixed) == 1);
}

void testSelfMoveElimination() {
  // After allocation: v1,v2 -> P0 (coalesced), v3 -> P1, v4 -> P2.
  std::vector<Move> moves{{1, 2}, {3, 4}};
  const std::map<Reg, int> assignment{{1, 0}, {2, 0}, {3, 1}, {4, 2}};
  CHECK(removeSelfMoves(moves, assignment) == 1);
  CHECK(moves == (std::vector<Move>{{3, 4}}));
}

int main() {
  testSequentializeSwap();
  testSequentializeChainNeedsNoTemp();
  testSequentializeRandomized();
  testLostCopy();
  testSwap();
  testSelfMoveElimination();
  return ts::report("08_phi_elimination");
}

// ---------------------------------------------------------------------------
// CHECKPOINT
//
// Q1. sequentialize() needs a scratch register to break cycles. Suppose the
//     allocator has already run and every register is taken. What can you do
//     instead? (Hint: x86 has `xchg`. On RISC targets, a cycle of length k can
//     be done with k-1 swaps; how do you do a swap with no temp? What does the
//     XOR-swap cost compared to a spill?)
//
// Q2. The copies here are placed *before* register allocation. Why does placing
//     them first and coalescing later beat trying to choose registers so that
//     no copies are needed? (Hint: think about what NP-hardness result applies,
//     and what the coalescer's "conservative" tests protect against.)
//
// CHALLENGE: lowerPhis() splits every edge into a phi block from a multi-
//     successor block. Change it to split only when needed: if the phi block
//     has a single predecessor, put the copies at the *start* of the phi block
//     instead. Add a test showing that one fewer block is created, and that
//     run() still returns the right value.
