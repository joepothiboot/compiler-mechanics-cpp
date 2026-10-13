// 10_live_intervals_linear_scan.cpp — infinite virtual registers, K physical.
//
// SSA hands the backend an unlimited supply of virtual registers. The machine
// has K (16 GPRs on x86-64, 31 on AArch64, 32 on RISC-V, minus reserved ones).
// The register allocator maps virtual to physical registers, and anything that
// doesn't fit is SPILLED: kept in a stack slot, with a store after each def and
// a load before each use.
//
// HARD TRUTHS
//   * Chaitin (1981): any graph can be the interference graph of some program,
//     so optimal register allocation (minimum spills) is as hard as graph
//     coloring, which is NP-complete. Hence heuristics (see 07 for Chaitin–Briggs).
//   * Hack, Grund & Goos (2006): interference graphs of programs in *strict SSA
//     form* are CHORDAL, and chordal graphs color optimally in polynomial time,
//     with exactly maxLive colors. The NP-hardness moves to *spilling* (choosing
//     what to evict) and *coalescing* (removing the copies phi lowering creates).
//   * Here each vreg has a single interval [start, end] with no holes, so the
//     interference graph is an INTERVAL graph. Interval graphs are perfect, so
//     K >= maxLive means linear scan never needs to spill (tested below).
//
// PIPELINE IN THIS FILE
//   1. Liveness: backward dataflow per block (the same framework as 06).
//   2. Live intervals over "slot indexes". Each instruction i gets a USE slot 2i
//      and a DEF slot 2i+1, so `x = c + 1` (c dies, x is born) doesn't make c
//      and x overlap. LLVM's SlotIndexes uses the same trick with 4 sub-slots.
//   3. Linear scan (Poletto & Sarkar 1999): sweep intervals by start, keep an
//      `active` set, free registers whose interval has ended, and spill when
//      none are free. O(n log n), with no interference graph to build, which is
//      why JITs use it (HotSpot C1, V8 TurboFan; Wimmer's variant adds interval
//      splitting).
//   4. Spill heuristic: which interval should be evicted?
//        FurthestEnd  — Poletto & Sarkar's rule: evict whoever ends last.
//        LowestWeight — weight = Σ over uses/defs of 10^loopDepth (the shape of
//                       LLVM's spill weights). Keep loop values in registers.
//   5. Spill-code insertion: rewrite spilled vregs into load/store pairs with
//      tiny new intervals, then allocate again.
//
// THE EXAMPLE (K = 4): five values are live inside the loop, so one must spill.
//
//   B0 (depth 0)       B1: loop (depth 1)           B2 (depth 0)
//    0: c = ...         4: t = i * 2                 8: x = c + s
//    1: n = ...         5: s = s + t                 9: ret x, n
//    2: i = 0           6: i = i + 1
//    3: s = 0           7: br i < n → B1 | B2
//
//   slot     0         1
//            0123456789012345678       interval   weight
//   c         ================         [ 1,16]      2   used after the loop only
//   n           ================       [ 3,18]     12   used IN the loop and at the end
//   i             ===========          [ 5,15]     41
//   s               ==========         [ 7,16]     22
//   t                 ==               [ 9,10]     20
//   x                         ==       [17,18]      2   starts after c dies at 16
//
//   FurthestEnd evicts n, the one ending last, but n is read on every
//   iteration (a load in the loop). LowestWeight evicts c (weight 2), which
//   costs one store before the loop and one load after it.

#include "test_support.h"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <vector>

namespace lsra {

using Reg = int;

struct Inst {
  enum class Kind { Op, Load, Store } kind = Kind::Op;
  std::optional<Reg> def;
  std::vector<Reg> uses;
  int stackSlot = -1; // for Load/Store
};

struct Block {
  std::vector<Inst> insts;
  std::vector<int> succs;
  int loopDepth = 0;
};

struct Program {
  std::vector<Block> blocks; // in layout order: instruction numbering follows it
};

// --- 1. Liveness --------------------------------------------------------------

struct Liveness {
  std::vector<std::set<Reg>> liveIn, liveOut;
};

Liveness computeLiveness(const Program &p) {
  const auto n = p.blocks.size();
  std::vector<std::set<Reg>> use(n), def(n);
  for (std::size_t b = 0; b < n; ++b)
    for (const Inst &i : p.blocks[b].insts) {
      for (Reg r : i.uses)
        if (!def[b].contains(r))
          use[b].insert(r); // upward-exposed use
      if (i.def)
        def[b].insert(*i.def);
    }

  Liveness l{std::vector<std::set<Reg>>(n), std::vector<std::set<Reg>>(n)};
  for (bool changed = true; changed;) {
    changed = false;
    for (std::size_t b = n; b-- > 0;) {
      std::set<Reg> out;
      for (int s : p.blocks[b].succs)
        out.insert(l.liveIn[s].begin(), l.liveIn[s].end());
      std::set<Reg> in = use[b]; // in = use ∪ (out − def)
      for (Reg r : out)
        if (!def[b].contains(r))
          in.insert(r);
      if (in != l.liveIn[b] || out != l.liveOut[b]) {
        l.liveIn[b] = std::move(in);
        l.liveOut[b] = std::move(out);
        changed = true;
      }
    }
  }
  return l;
}

// --- 2. Live intervals ----------------------------------------------------------

struct Interval {
  Reg reg;
  int start, end; // inclusive slot range
  double weight;  // Σ 10^loopDepth over every use and def
  bool overlaps(const Interval &o) const { return start <= o.end && o.start <= end; }
};

constexpr int useSlot(int inst) { return 2 * inst; }
constexpr int defSlot(int inst) { return 2 * inst + 1; }

std::vector<Interval> buildIntervals(const Program &p) {
  const Liveness live = computeLiveness(p);
  std::map<Reg, Interval> ivs;
  auto extend = [&](Reg r, int slot, double w) {
    auto [it, fresh] = ivs.try_emplace(r, Interval{r, slot, slot, 0});
    it->second.start = std::min(it->second.start, slot);
    it->second.end = std::max(it->second.end, slot);
    it->second.weight += w;
  };

  int idx = 0;
  for (std::size_t b = 0; b < p.blocks.size(); ++b) {
    const Block &blk = p.blocks[b];
    const int first = idx, last = idx + static_cast<int>(blk.insts.size()) - 1;
    double w = 1;
    for (int d = 0; d < blk.loopDepth; ++d)
      w *= 10;
    for (Reg r : live.liveIn[b])
      extend(r, useSlot(first), 0);
    for (const Inst &i : blk.insts) {
      for (Reg r : i.uses)
        extend(r, useSlot(idx), w);
      if (i.def)
        extend(*i.def, defSlot(idx), w);
      ++idx;
    }
    for (Reg r : live.liveOut[b])
      extend(r, defSlot(last), 0);
  }

  auto values = std::views::values(ivs);
  return {values.begin(), values.end()};
}

int maxLive(const std::vector<Interval> &ivs) {
  int best = 0;
  for (const Interval &a : ivs)
    best = std::max(best, static_cast<int>(std::ranges::count_if(
                              ivs, [&](const Interval &b) {
                                return b.start <= a.start && a.start <= b.end;
                              })));
  return best;
}

// --- 3/4. Linear scan ------------------------------------------------------------

enum class SpillHeuristic { FurthestEnd, LowestWeight };

struct Allocation {
  std::map<Reg, int> phys;
  std::set<Reg> spilled;
};

Allocation linearScan(std::vector<Interval> ivs, int K, SpillHeuristic h) {
  std::ranges::sort(ivs, {}, &Interval::start);
  Allocation a;
  std::vector<Interval> active;
  std::vector<int> freeRegs;
  for (int r = K - 1; r >= 0; --r)
    freeRegs.push_back(r); // pop_back hands out r0 first

  // Higher = better to spill.
  auto spillScore = [h](const Interval &iv) {
    return h == SpillHeuristic::FurthestEnd ? static_cast<double>(iv.end) : -iv.weight;
  };

  for (const Interval &cur : ivs) {
    // Expire intervals that ended before this one starts.
    std::erase_if(active, [&](const Interval &iv) {
      if (iv.end >= cur.start)
        return false;
      freeRegs.push_back(a.phys.at(iv.reg));
      return true;
    });

    if (!freeRegs.empty()) {
      a.phys[cur.reg] = freeRegs.back();
      freeRegs.pop_back();
      active.push_back(cur);
      continue;
    }

    auto victim = std::ranges::max_element(active, {}, spillScore);
    if (spillScore(*victim) > spillScore(cur)) {
      a.phys[cur.reg] = a.phys.at(victim->reg); // steal the victim's register
      a.phys.erase(victim->reg);
      a.spilled.insert(victim->reg);
      *victim = cur;
    } else {
      a.spilled.insert(cur.reg);
    }
  }
  return a;
}

// No two intervals sharing a physical register may overlap.
bool isValid(const std::vector<Interval> &ivs, const Allocation &a) {
  for (const Interval &x : ivs)
    for (const Interval &y : ivs)
      if (x.reg < y.reg && a.phys.contains(x.reg) && a.phys.contains(y.reg) &&
          a.phys.at(x.reg) == a.phys.at(y.reg) && x.overlaps(y))
        return false;
  return std::ranges::all_of(ivs, [&](const Interval &iv) {
    return a.phys.contains(iv.reg) != a.spilled.contains(iv.reg);
  });
}

// --- 5. Spill-code insertion -----------------------------------------------------

struct SpillStats {
  int loads = 0, stores = 0;
  double dynamicCost = 0; // memory ops weighted by 10^loopDepth
};

SpillStats insertSpillCode(Program &p, const std::set<Reg> &spilled) {
  Reg nextReg = 0;
  for (const Block &b : p.blocks)
    for (const Inst &i : b.insts) {
      for (Reg r : i.uses)
        nextReg = std::max(nextReg, r + 1);
      if (i.def)
        nextReg = std::max(nextReg, *i.def + 1);
    }

  std::map<Reg, int> slotOf;
  for (Reg r : spilled)
    slotOf.emplace(r, static_cast<int>(slotOf.size()));

  SpillStats st;
  for (Block &b : p.blocks) {
    double w = 1;
    for (int d = 0; d < b.loopDepth; ++d)
      w *= 10;
    std::vector<Inst> out;
    for (Inst i : b.insts) {
      // Each use of a spilled vreg gets a fresh, instruction-local vreg.
      for (Reg &r : i.uses)
        if (slotOf.contains(r)) {
          const Reg tmp = nextReg++;
          out.push_back({Inst::Kind::Load, tmp, {}, slotOf.at(r)});
          r = tmp;
          ++st.loads;
          st.dynamicCost += w;
        }
      std::optional<Inst> store;
      if (i.def && slotOf.contains(*i.def)) {
        const Reg tmp = nextReg++;
        store = Inst{Inst::Kind::Store, std::nullopt, {tmp}, slotOf.at(*i.def)};
        i.def = tmp;
        ++st.stores;
        st.dynamicCost += w;
      }
      out.push_back(std::move(i));
      if (store)
        out.push_back(*store);
    }
    b.insts = std::move(out);
  }
  return st;
}

} // namespace lsra

using namespace lsra;

enum : Reg { c, n, i, s, t, x };

Program hotLoop() {
  using K = Inst::Kind;
  Program p;
  p.blocks = {
      {.insts = {{K::Op, c, {}}, {K::Op, n, {}}, {K::Op, i, {}}, {K::Op, s, {}}},
       .succs = {1}},
      {.insts = {{K::Op, t, {i}}, {K::Op, s, {s, t}}, {K::Op, i, {i}}, {K::Op, {}, {i, n}}},
       .succs = {1, 2},
       .loopDepth = 1},
      {.insts = {{K::Op, x, {c, s}}, {K::Op, {}, {x, n}}}},
  };
  return p;
}

const Interval &find(const std::vector<Interval> &ivs, Reg r) {
  return *std::ranges::find(ivs, r, &Interval::reg);
}

void testLivenessExtendsAroundTheLoop() {
  const auto live = computeLiveness(hotLoop());
  CHECK(live.liveIn[1] == (std::set<Reg>{c, n, i, s}));  // c and n live through the loop
  CHECK(live.liveOut[1] == (std::set<Reg>{c, n, i, s})); // back edge keeps i, s alive
  CHECK(live.liveIn[2] == (std::set<Reg>{c, n, s}));
}

void testIntervalsUseSlotIndexes() {
  const auto ivs = buildIntervals(hotLoop());
  CHECK(find(ivs, c).start == 1 && find(ivs, c).end == 16);  // def slot of 0 .. use slot of 8
  CHECK(find(ivs, n).end == 18);                              // last use: `ret` at inst 9
  CHECK(find(ivs, i).end == 15);                              // live-out of the loop block
  CHECK(find(ivs, x).start == 17);                            // born after c dies at 16
  CHECK(!find(ivs, c).overlaps(find(ivs, x)));
  CHECK(maxLive(ivs) == 5);
  CHECK(find(ivs, c).weight == 2);   // def + use, both depth 0
  CHECK(find(ivs, n).weight == 12);  // def (1) + loop use (10) + final use (1)
}

void testEnoughRegistersNeverSpill() {
  // Interval graphs are perfect: maxLive colors always suffice.
  const auto ivs = buildIntervals(hotLoop());
  for (auto h : {SpillHeuristic::FurthestEnd, SpillHeuristic::LowestWeight}) {
    const auto a = linearScan(ivs, maxLive(ivs), h);
    CHECK(a.spilled.empty());
    CHECK(isValid(ivs, a));
  }
}

void testSpillHeuristicsDisagree() {
  const auto ivs = buildIntervals(hotLoop());

  const auto furthest = linearScan(ivs, 4, SpillHeuristic::FurthestEnd);
  CHECK(furthest.spilled == std::set<Reg>{n}); // evicts the loop bound
  CHECK(isValid(ivs, furthest));

  const auto weighted = linearScan(ivs, 4, SpillHeuristic::LowestWeight);
  CHECK(weighted.spilled == std::set<Reg>{c}); // evicts the cold value
  CHECK(isValid(ivs, weighted));
}

void testSpillCodeCost() {
  Program a = hotLoop();
  const auto bad = insertSpillCode(a, {n});
  CHECK(bad.stores == 1 && bad.loads == 2);
  CHECK(bad.dynamicCost == 12); // the load inside the loop runs every iteration

  Program b = hotLoop();
  const auto good = insertSpillCode(b, {c});
  CHECK(good.stores == 1 && good.loads == 1);
  CHECK(good.dynamicCost == 2);

  // The loop block now starts with no extra instruction, and B2 begins with
  // the reload of c.
  CHECK(b.blocks[1].insts.size() == 4);
  CHECK(b.blocks[2].insts.front().kind == Inst::Kind::Load);
}

void testRewrittenProgramAllocates() {
  // After rewriting, the spilled value lives only in tiny load/store intervals,
  // so a second allocation round fits in K = 4.
  for (Reg victim : {c, n}) {
    Program p = hotLoop();
    insertSpillCode(p, {victim});
    const auto ivs = buildIntervals(p);
    CHECK(maxLive(ivs) <= 4);
    const auto a = linearScan(ivs, 4, SpillHeuristic::LowestWeight);
    CHECK(a.spilled.empty());
    CHECK(isValid(ivs, a));
  }
}

int main() {
  testLivenessExtendsAroundTheLoop();
  testIntervalsUseSlotIndexes();
  testEnoughRegistersNeverSpill();
  testSpillHeuristicsDisagree();
  testSpillCodeCost();
  testRewrittenProgramAllocates();
  return ts::report("10_live_intervals_linear_scan");
}

// ---------------------------------------------------------------------------
// CHECKPOINT
//
// Q1. Our intervals have no holes: `c` occupies a register for the entire loop
//     even though the loop never reads it. Wimmer's linear scan (HotSpot C1)
//     and LLVM's greedy allocator SPLIT intervals instead: c stays in a
//     register before and after the loop and lives on the stack only *across*
//     it. Where exactly would you put the store and the reload, and why is
//     that better than spilling c everywhere? (Hint: what's the frequency of a
//     loop's preheader and exit blocks?)
//
// Q2. Linear scan is O(n log n), and Chaitin–Briggs (07) is O(n²) to build the
//     graph alone. Yet LLVM's default for ahead-of-time compilation is neither:
//     it's a priority-based "greedy" allocator with eviction and splitting.
//     What does an AOT compiler gain from spending more compile time here that
//     a JIT can't afford? (Hint: a JIT also has to amortize the time spent
//     allocating against how long the code runs.)
//
// CHALLENGE: implement "second-chance" spilling: when a spilled value is
//     reloaded, keep it in the register until that register is needed again,
//     so consecutive uses don't each reload. Measure the drop in `loads` on a
//     block that uses the spilled value three times in a row.
