// 11_scheduling_and_layout.cpp — instruction scheduling and code layout.
//
// PART 1: LIST SCHEDULING WITHIN A BASIC BLOCK
//
// A pipelined CPU starts a new instruction every cycle, but a result isn't
// ready until its LATENCY has passed (add: 1, imul: 3, L1 load: 4+). If the
// next instruction needs that result, an in-order core STALLS (a data hazard,
// read-after-write). Out-of-order cores hide some of this in hardware, but
// in-order cores (many ARM Cortex-A5x, RISC-V embedded, GPUs, DSPs) rely on
// the compiler, and even OoO cores benefit when the window fills up.
//
// The dependency DAG of a block, for loads from x and y each feeding an add,
// assuming alias analysis has proved the stores don't touch x or y:
//
//      ld a,[x] ──4──▶ add b,a,1 ──1──▶ st [z],b
//      ld c,[y] ──4──▶ add d,c,1 ──1──▶ st [w],d      edge label = latency
//
//   source order (in-order, 1 issue/cycle):    scheduled (critical path first):
//   cycle 0  ld a                              cycle 0  ld a
//         1  (stall) (stall) (stall)                 1  ld c
//         4  add b                                   2  (stall)(stall)
//         5  st [z]                                  4  add b
//         6  ld c                                    5  add d
//         7  (stall) (stall) (stall)                 6  st [z]
//        10  add d ...                               7  st [w]
//
// Dependencies the DAG must keep:
//   RAW (true):   b = a + 1 after a is defined          latency = producer's
//   WAR (anti):   a redefined after its last read       order only
//   WAW (output): two defs of a keep their order        order only
//   memory:       loads/stores that may alias keep their order (alias analysis
//                 can remove these edges; see the guide's ch. 5)
//
// HARD TRUTH: optimal scheduling with latencies is NP-complete, even for one
// basic block on a single-issue machine. LIST SCHEDULING is the standard
// heuristic: each cycle, issue the ready instruction with the highest priority,
// where priority = critical-path length to the end of the DAG.
//
// THE PHASE-ORDERING TENSION: hoisting loads early lengthens live ranges, which
// raises register pressure (10). Schedule before allocation and you may cause
// spills; schedule after it, and the allocator's register reuse adds WAR edges
// that block reordering. LLVM runs a pre-RA scheduler that tracks register
// pressure, then a post-RA one; GCC has -fschedule-insns and -fschedule-insns2.
//
// PART 2: CODE LAYOUT
//
// A taken branch costs front-end bandwidth: the fetch unit has to redirect, and
// the instructions after the branch in the fetched block are wasted. Placing
// the likely successor directly after a block makes it a FALL-THROUGH. Moving
// never-executed blocks (error handling) away from hot code keeps the hot path
// dense in the I-cache and I-TLB.
//
//   Pettis–Hansen chaining: sort edges by profile weight; for each edge A→B, if
//   A ends a chain and B starts another, concatenate them. Place the entry
//   chain first, then the remaining chains hottest first, and move cold blocks
//   to the end (the ".text.unlikely" / ".cold" split).
//
//   source order:   entry │ error │ header │ body │ exit     (error mid-hot path)
//   optimized:      entry │ header │ body │ exit ║ error     (cold split off)
//
// LLVM: MachineBlockPlacement (chain-based, profile-driven); ext-TSP layout in
// BOLT and LLVM; hot/cold splitting with -fsplit-machine-functions.

#include "test_support.h"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <vector>

namespace sched {

// --- Part 1: list scheduling -------------------------------------------------------

struct Inst {
  std::string name;
  std::optional<int> def;
  std::vector<int> uses;
  int latency = 1;
  enum class Mem { None, Load, Store } mem = Mem::None;
};

struct Edge {
  int to;
  int latency;
};

struct Dag {
  std::vector<std::vector<Edge>> succs;
  std::vector<int> numPreds;
};

Dag buildDag(const std::vector<Inst> &block) {
  const int n = static_cast<int>(block.size());
  Dag g{std::vector<std::vector<Edge>>(n), std::vector<int>(n, 0)};
  auto addEdge = [&](int from, int to, int lat) {
    for (Edge &e : g.succs[from])
      if (e.to == to) {
        e.latency = std::max(e.latency, lat);
        return;
      }
    g.succs[from].push_back({to, lat});
    ++g.numPreds[to];
  };

  for (int j = 0; j < n; ++j)
    for (int i = 0; i < j; ++i) {
      const Inst &a = block[i], &b = block[j];
      if (a.def && std::ranges::contains(b.uses, *a.def))
        addEdge(i, j, a.latency); // RAW
      if (b.def && std::ranges::contains(a.uses, *b.def))
        addEdge(i, j, 0); // WAR: b may issue in the same cycle as a
      if (a.def && b.def && *a.def == *b.def)
        addEdge(i, j, 1); // WAW
      const bool memA = a.mem != Inst::Mem::None, memB = b.mem != Inst::Mem::None;
      if (memA && memB && (a.mem == Inst::Mem::Store || b.mem == Inst::Mem::Store))
        addEdge(i, j, a.mem == Inst::Mem::Store ? 1 : 0); // may-alias memory order
    }
  return g;
}

// Longest latency-weighted path from each node to the end of the block.
std::vector<int> criticalPath(const std::vector<Inst> &block, const Dag &g) {
  std::vector<int> cp(block.size());
  for (int i = static_cast<int>(block.size()) - 1; i >= 0; --i) {
    cp[i] = block[i].latency;
    for (const Edge &e : g.succs[i])
      cp[i] = std::max(cp[i], e.latency + cp[e.to]);
  }
  return cp;
}

// Simulates a single-issue, in-order pipeline: instructions issue in the given
// order, each waiting until its operands are ready. Returns the cycle when the
// last result is available.
int simulateInOrder(const std::vector<Inst> &block, const std::vector<int> &order) {
  const Dag g = buildDag(block);
  std::vector<int> earliest(block.size(), 0);
  int cycle = 0, done = 0;
  for (int idx : order) {
    cycle = std::max(cycle, earliest[idx]);
    for (const Edge &e : g.succs[idx])
      earliest[e.to] = std::max(earliest[e.to], cycle + e.latency);
    done = std::max(done, cycle + block[idx].latency);
    ++cycle; // one issue per cycle
  }
  return done;
}

std::vector<int> listSchedule(const std::vector<Inst> &block) {
  const Dag g = buildDag(block);
  const auto cp = criticalPath(block, g);
  const int n = static_cast<int>(block.size());
  std::vector<int> preds = g.numPreds, earliest(n, 0), order;
  std::set<int> ready;
  for (int i = 0; i < n; ++i)
    if (preds[i] == 0)
      ready.insert(i);

  for (int cycle = 0; static_cast<int>(order.size()) < n; ++cycle) {
    // Among instructions whose operands are available now, take the one with
    // the longest critical path (ties: original order, for stability).
    std::optional<int> pick;
    for (int i : ready)
      if (earliest[i] <= cycle && (!pick || cp[i] > cp[*pick]))
        pick = i;
    if (!pick)
      continue; // nothing can issue: stall cycle
    ready.erase(*pick);
    order.push_back(*pick);
    for (const Edge &e : g.succs[*pick]) {
      earliest[e.to] = std::max(earliest[e.to], cycle + e.latency);
      if (--preds[e.to] == 0)
        ready.insert(e.to);
    }
  }
  return order;
}

bool respectsDependences(const std::vector<Inst> &block, const std::vector<int> &order) {
  const Dag g = buildDag(block);
  std::vector<int> pos(block.size());
  for (int k = 0; k < static_cast<int>(order.size()); ++k)
    pos[order[k]] = k;
  for (int i = 0; i < static_cast<int>(block.size()); ++i)
    for (const Edge &e : g.succs[i])
      if (pos[i] >= pos[e.to])
        return false;
  return true;
}

// --- Part 2: block layout ---------------------------------------------------------

struct CfgEdge {
  int from, to;
  long weight; // profile count
};

struct Cfg {
  std::vector<std::string> names;
  std::vector<int> sizes;  // bytes
  std::vector<long> counts; // block execution counts
  std::vector<CfgEdge> edges;
};

// Weighted count of taken branches: every edge whose target isn't laid out
// directly after its source costs one taken branch per execution.
long takenBranches(const Cfg &cfg, const std::vector<int> &layout) {
  std::map<int, int> pos;
  for (int k = 0; k < static_cast<int>(layout.size()); ++k)
    pos[layout[k]] = k;
  long taken = 0;
  for (const CfgEdge &e : cfg.edges)
    if (pos.at(e.to) != pos.at(e.from) + 1)
      taken += e.weight;
  return taken;
}

// Bytes from the first to the last hot block: what the hot path drags into the I-cache.
int hotSpan(const Cfg &cfg, const std::vector<int> &layout, long coldThreshold) {
  std::vector<int> hotPos;
  for (int k = 0; k < static_cast<int>(layout.size()); ++k)
    if (cfg.counts[layout[k]] > coldThreshold)
      hotPos.push_back(k);
  int bytes = 0;
  for (int k = hotPos.front(); k <= hotPos.back(); ++k)
    bytes += cfg.sizes[layout[k]];
  return bytes;
}

std::vector<int> pettisHansen(const Cfg &cfg, int entry, long coldThreshold) {
  const int n = static_cast<int>(cfg.names.size());
  std::vector<std::vector<int>> chains(n);
  std::vector<int> chainOf(n);
  for (int b = 0; b < n; ++b) {
    chains[b] = {b};
    chainOf[b] = b;
  }

  auto edges = cfg.edges;
  std::ranges::sort(edges, std::greater{}, &CfgEdge::weight);
  for (const CfgEdge &e : edges) {
    const int ca = chainOf[e.from], cb = chainOf[e.to];
    if (ca == cb || chains[ca].back() != e.from || chains[cb].front() != e.to)
      continue; // not tail→head, or would close a cycle
    if (e.to == entry)
      continue; // the entry block must start the function
    for (int b : chains[cb])
      chainOf[b] = ca;
    chains[ca].insert(chains[ca].end(), chains[cb].begin(), chains[cb].end());
    chains[cb].clear();
  }

  // Entry chain first, then other chains by hottest block; cold blocks last.
  std::vector<int> order;
  for (int c = 0; c < n; ++c)
    if (!chains[c].empty() && c != chainOf[entry])
      order.push_back(c);
  auto heat = [&](int c) {
    return std::ranges::max(chains[c] | std::views::transform([&](int b) { return cfg.counts[b]; }));
  };
  std::ranges::stable_sort(order, std::greater{}, heat);
  order.insert(order.begin(), chainOf[entry]);

  std::vector<int> hot, cold;
  for (int c : order)
    for (int b : chains[c])
      (cfg.counts[b] > coldThreshold ? hot : cold).push_back(b);
  hot.insert(hot.end(), cold.begin(), cold.end());
  return hot;
}

} // namespace sched

using namespace sched;

// a = ld [x]; b = a+1; st [z], b; c = ld [y]; d = c+1; st [w], d
// mayAlias = false models alias analysis proving the stores hit different memory
// from the loads: the memory-order edges disappear.
std::vector<Inst> twoChains(bool mayAlias) {
  using M = Inst::Mem;
  const M store = mayAlias ? M::Store : M::None;
  return {
      {"ld a", 0, {}, 4, M::Load},  {"add b", 1, {0}, 1},
      {"st z", {}, {1}, 1, store},  {"ld c", 2, {}, 4, M::Load},
      {"add d", 3, {2}, 1},         {"st w", {}, {3}, 1, store},
  };
}

void testDagEdges() {
  const auto g = buildDag(twoChains(/*mayAlias=*/true));
  // ld a → add b carries the load latency.
  CHECK(std::ranges::contains(g.succs[0], 4, &Edge::latency));
  // st z → ld c: a store may alias a later load, so they stay ordered.
  CHECK(std::ranges::contains(g.succs[2], 3, &Edge::to));
}

void testSchedulingHidesLoadLatency() {
  const std::vector<int> source{0, 1, 2, 3, 4, 5};

  // May-alias: `st z` pins `ld c` below it, so nothing can be reordered.
  const auto aliased = twoChains(true);
  CHECK(simulateInOrder(aliased, source) == 12);
  CHECK(simulateInOrder(aliased, listSchedule(aliased)) == 12);

  // Proven no-alias: both loads issue first (the diagram in the header).
  const auto independent = twoChains(false);
  CHECK(simulateInOrder(independent, source) == 12);
  const auto order = listSchedule(independent);
  CHECK(respectsDependences(independent, order));
  CHECK(order == (std::vector<int>{0, 3, 1, 4, 2, 5}));
  CHECK(simulateInOrder(independent, order) == 8);
}

void testIndependentLoadsOverlap() {
  // With no stores in between (alias analysis proved the loads independent),
  // all three loads go first and their latencies overlap.
  using M = Inst::Mem;
  const std::vector<Inst> block{
      {"ld a", 0, {}, 4, M::Load}, {"add a", 1, {0}, 1},
      {"ld b", 2, {}, 4, M::Load}, {"add b", 3, {2}, 1},
      {"ld c", 4, {}, 4, M::Load}, {"add c", 5, {4}, 1},
  };
  const auto order = listSchedule(block);
  CHECK(respectsDependences(block, order));
  CHECK((std::vector<int>(order.begin(), order.begin() + 3) == std::vector<int>{0, 2, 4}));
  CHECK(simulateInOrder(block, {0, 1, 2, 3, 4, 5}) == 15);
  CHECK(simulateInOrder(block, order) == 7); // loads at 0,1,2; adds at 4,5,6
}

void testWarEdgeAllowsSameCycleOnlyInOrder() {
  // r0 = ld; r1 = r0 + 1; r0 = 5  (the last def must stay after the read of r0)
  const std::vector<Inst> block{
      {"ld r0", 0, {}, 4, Inst::Mem::Load}, {"add r1", 1, {0}, 1}, {"mov r0", 0, {}, 1}};
  const auto order = listSchedule(block);
  CHECK(respectsDependences(block, order));
  CHECK(order.back() == 2);
}

// entry(0) → error(1) [rare] | header(2); header ↔ body(3); header → exit(4)
Cfg loopWithErrorPath() {
  return Cfg{
      .names = {"entry", "error", "header", "body", "exit"},
      .sizes = {16, 200, 16, 64, 16},
      .counts = {1000, 1, 100'900, 99'900, 1000},
      .edges = {{0, 1, 1}, {0, 2, 999}, {1, 4, 1}, {2, 3, 99'900}, {3, 2, 99'900}, {2, 4, 999}},
  };
}

void testLayoutMakesHotEdgesFallThrough() {
  const Cfg cfg = loopWithErrorPath();
  const std::vector<int> source{0, 1, 2, 3, 4};
  const auto layout = pettisHansen(cfg, 0, /*coldThreshold=*/10);

  CHECK(layout == (std::vector<int>{0, 2, 3, 4, 1}));
  CHECK(layout.front() == 0);  // entry stays first
  CHECK(layout.back() == 1);   // error handler split off as cold
  CHECK(takenBranches(cfg, source) == 101'899);
  CHECK(takenBranches(cfg, layout) == 100'901); // entry→header now falls through
}

void testColdSplittingShrinksHotSpan() {
  const Cfg cfg = loopWithErrorPath();
  CHECK(hotSpan(cfg, {0, 1, 2, 3, 4}, 10) == 312); // 200-byte error block in the middle
  CHECK(hotSpan(cfg, pettisHansen(cfg, 0, 10), 10) == 112);
}

int main() {
  testDagEdges();
  testSchedulingHidesLoadLatency();
  testIndependentLoadsOverlap();
  testWarEdgeAllowsSameCycleOnlyInOrder();
  testLayoutMakesHotEdgesFallThrough();
  testColdSplittingShrinksHotSpan();
  return ts::report("11_scheduling_and_layout");
}

// ---------------------------------------------------------------------------
// CHECKPOINT
//
// Q1. In testLayoutMakesHotEdgesFallThrough, body→header (99,900 executions)
//     is STILL a taken branch, and no ordering of {header, body} can fix it,
//     because one of the two loop edges must be taken. LLVM's LoopRotate turns
//     `while (c) { body }` into `if (c) do { body } while (c);`. How does that
//     change which edge is taken on each iteration, and how many taken branches
//     per iteration remain?
//
// Q2. The no-alias schedule issues `ld c` before `add b`, so a, b and c are
//     all live at once. On a machine with only two free registers that
//     schedule forces a spill, which costs more than the stalls it saved. How
//     would a register-pressure-aware list scheduler change its priority
//     function? (LLVM's default GenericScheduler tracks pressure;
//     -misched=ilpmax ignores it.) What should it do when pressure exceeds
//     the limit?
//
// CHALLENGE: extend simulateInOrder() and listSchedule() to a 2-wide machine
//     (two issues per cycle, but at most one memory operation per cycle).
//     Check that testIndependentLoadsOverlap still passes, and find a block
//     where 2-wide issue doesn't help because the critical path dominates.
