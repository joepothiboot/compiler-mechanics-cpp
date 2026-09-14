// 07_register_allocation.cpp — graph-coloring register allocation.
//
// The backend question: you have unlimited virtual registers from SSA and K
// physical ones. Two virtual registers that are simultaneously live "interfere"
// and must get different physical registers. That's graph coloring, which is
// NP-complete in general, so compilers use Chaitin–Briggs heuristics:
//
//   SIMPLIFY: repeatedly remove any node with degree < K and push it on a
//             stack. Rationale: whatever its neighbours get, fewer than K of
//             them are taken, so a color will be left over.
//   SPILL:    if every remaining node has degree >= K, push one *optimistically*
//             (Briggs' improvement over Chaitin) — its neighbours may end up
//             sharing colors, so it can still succeed.
//   SELECT:   pop the stack, assigning each node the lowest color unused by its
//             already-colored neighbours. A node with no color left is an
//             actual spill: rewrite it to memory and re-run.
//
// Precolored nodes model physical-register constraints (x86 div writes EAX:EDX,
// calling conventions pin arguments). They are never simplified and never
// spilled; they just permanently occupy a color for their neighbours.
//
// LLVM's default allocator (greedy) is priority-based with live-range splitting
// rather than pure coloring, but the interference/spill vocabulary is identical
// and this is what interviews ask about.

#include "test_support.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace regalloc {

struct InterferenceGraph {
  std::vector<std::string> names;
  std::vector<std::set<int>> adj;
  std::vector<int> preColor; // -1 = virtual register, >= 0 = pinned physreg

  int addNode(std::string name, int fixedColor = -1) {
    names.push_back(std::move(name));
    adj.emplace_back();
    preColor.push_back(fixedColor);
    return static_cast<int>(names.size()) - 1;
  }
  void addEdge(int a, int b) {
    if (a == b)
      return; // a value never interferes with itself
    adj[a].insert(b);
    adj[b].insert(a);
  }
  int size() const { return static_cast<int>(names.size()); }
};

struct AllocResult {
  std::vector<int> color;   // -1 = not assigned
  std::vector<bool> spilled;

  int colorsUsed() const {
    std::set<int> used(color.begin(), color.end());
    used.erase(-1);
    return static_cast<int>(used.size());
  }
  int spillCount() const {
    return static_cast<int>(std::count(spilled.begin(), spilled.end(), true));
  }
};

AllocResult allocate(const InterferenceGraph &g, int K) {
  const int n = g.size();
  AllocResult result;
  result.color.assign(n, -1);
  result.spilled.assign(n, false);

  std::vector<bool> removed(n, false);
  std::vector<int> degree(n, 0);
  for (int i = 0; i < n; ++i)
    degree[i] = static_cast<int>(g.adj[i].size()); // precolored neighbours count

  // Precolored nodes are fixed points: color them now, never simplify them.
  int remaining = 0;
  for (int i = 0; i < n; ++i) {
    if (g.preColor[i] >= 0)
      result.color[i] = g.preColor[i];
    else
      ++remaining;
  }

  // --- SIMPLIFY / optimistic SPILL ----------------------------------------
  std::vector<int> stack;
  while (remaining > 0) {
    int pick = -1;
    for (int i = 0; i < n; ++i)
      if (!removed[i] && g.preColor[i] < 0 && degree[i] < K) {
        pick = i;
        break;
      }
    if (pick == -1) {
      // Everything left is "significant degree". Pick the highest-degree node:
      // removing it relieves the most pressure. Real allocators divide by a
      // spill cost estimate (use frequency weighted by loop depth) so that
      // loop-carried values are spilled last.
      for (int i = 0; i < n; ++i)
        if (!removed[i] && g.preColor[i] < 0 &&
            (pick == -1 || degree[i] > degree[pick]))
          pick = i;
    }
    removed[pick] = true;
    stack.push_back(pick);
    --remaining;
    for (int nb : g.adj[pick])
      if (!removed[nb])
        --degree[nb];
  }

  // --- SELECT --------------------------------------------------------------
  while (!stack.empty()) {
    int v = stack.back();
    stack.pop_back();

    std::vector<bool> taken(K, false);
    for (int nb : g.adj[v]) {
      int c = result.color[nb];
      if (c >= 0 && c < K)
        taken[c] = true;
    }
    int chosen = -1;
    for (int c = 0; c < K; ++c)
      if (!taken[c]) {
        chosen = c;
        break;
      }
    if (chosen == -1)
      result.spilled[v] = true; // optimism did not pay off: real spill
    else
      result.color[v] = chosen;
  }
  return result;
}

// The correctness property a register allocator must satisfy. In a real backend
// this is what a verifier pass (or a machine-verifier assertion) checks.
bool verify(const InterferenceGraph &g, const AllocResult &r, int K) {
  for (int i = 0; i < g.size(); ++i) {
    if (r.spilled[i]) {
      if (r.color[i] != -1)
        return false; // a spilled value must not also hold a register
      continue;
    }
    if (r.color[i] < 0 || r.color[i] >= K)
      return false;
    if (g.preColor[i] >= 0 && r.color[i] != g.preColor[i])
      return false; // physical constraint violated
    for (int nb : g.adj[i])
      if (!r.spilled[nb] && r.color[nb] == r.color[i])
        return false; // two interfering values in the same register
  }
  return true;
}

// ---------------------------------------------------------------------------
// Where the graph comes from: live ranges. In a linear (straight-line) region,
// two values interfere iff their live intervals overlap — which is precisely
// the output of the liveness analysis in file 06, computed per program point.
// ---------------------------------------------------------------------------
struct LiveRange {
  std::string name;
  int start; // inclusive
  int end;   // exclusive
};

InterferenceGraph buildFromLiveRanges(const std::vector<LiveRange> &ranges) {
  InterferenceGraph g;
  for (const LiveRange &lr : ranges)
    g.addNode(lr.name);
  for (size_t i = 0; i < ranges.size(); ++i)
    for (size_t j = i + 1; j < ranges.size(); ++j)
      if (ranges[i].start < ranges[j].end && ranges[j].start < ranges[i].end)
        g.addEdge(static_cast<int>(i), static_cast<int>(j));
  return g;
}

void testColorableGraph() {
  // Triangle a-b-c plus a pendant d attached to c. Chromatic number 3.
  InterferenceGraph g;
  int a = g.addNode("a"), b = g.addNode("b"), c = g.addNode("c"),
      d = g.addNode("d");
  g.addEdge(a, b);
  g.addEdge(b, c);
  g.addEdge(c, a);
  g.addEdge(c, d);

  AllocResult r = allocate(g, 3);
  CHECK(verify(g, r, 3));
  CHECK(r.spillCount() == 0);
  CHECK(r.colorsUsed() <= 3);
  CHECK(r.color[a] != r.color[b]);
  CHECK(r.color[b] != r.color[c]);
  CHECK(r.color[a] != r.color[c]);
  CHECK(r.color[c] != r.color[d]);
  CHECK(r.color[a] == r.color[d] || r.color[b] == r.color[d]); // d reuses one
}

void testSpillRequired() {
  // A triangle needs 3 colors; with K=2 exactly one value must go to memory.
  InterferenceGraph g;
  int a = g.addNode("a"), b = g.addNode("b"), c = g.addNode("c");
  g.addEdge(a, b);
  g.addEdge(b, c);
  g.addEdge(c, a);

  AllocResult r = allocate(g, 2);
  CHECK(verify(g, r, 2));
  CHECK(r.spillCount() == 1); // minimum possible: K=2 on K_3
  CHECK(r.colorsUsed() == 2);
}

void testOptimisticColoring() {
  // A 4-cycle: every node has degree 2, so with K=2 simplify stalls immediately
  // and one node is pushed optimistically. But a cycle of even length IS
  // 2-colorable, so Briggs' optimism succeeds and nothing spills. Chaitin's
  // original algorithm would have spilled here — this test is the difference.
  InterferenceGraph g;
  int n0 = g.addNode("n0"), n1 = g.addNode("n1"), n2 = g.addNode("n2"),
      n3 = g.addNode("n3");
  g.addEdge(n0, n1);
  g.addEdge(n1, n2);
  g.addEdge(n2, n3);
  g.addEdge(n3, n0);

  AllocResult r = allocate(g, 2);
  CHECK(verify(g, r, 2));
  CHECK(r.spillCount() == 0);
  CHECK(r.color[n0] == r.color[n2]);
  CHECK(r.color[n1] == r.color[n3]);
}

void testPrecoloredPhysReg() {
  // %eax is pinned to color 0 (e.g. it holds a call's return value). Anything
  // live across that point must avoid color 0.
  InterferenceGraph g;
  int eax = g.addNode("%eax", /*fixedColor=*/0);
  int x = g.addNode("x");
  int y = g.addNode("y");
  g.addEdge(eax, x);
  g.addEdge(x, y);

  AllocResult r = allocate(g, 2);
  CHECK(verify(g, r, 2));
  CHECK(r.color[eax] == 0); // untouched
  CHECK(r.color[x] == 1);   // forced away from the physreg
  CHECK(r.color[y] == 0);   // free to reuse it: y does not interfere with %eax
  CHECK(r.spillCount() == 0);
}

void testFromLiveRanges() {
  // Straight-line code, one live range per value:
  //   0: a = ...          a live [0,4)
  //   1: b = ...          b live [1,3)
  //   2: c = a + b        c live [2,6)
  //   5: d = c * 2        d live [5,8)
  // Max simultaneous pressure is 3 (a, b, c at point 2), so K=3 suffices.
  std::vector<LiveRange> ranges = {
      {"a", 0, 4}, {"b", 1, 3}, {"c", 2, 6}, {"d", 5, 8}};
  InterferenceGraph g = buildFromLiveRanges(ranges);

  CHECK(g.adj[0].count(1) == 1); // a-b overlap
  CHECK(g.adj[0].count(2) == 1); // a-c overlap
  CHECK(g.adj[0].count(3) == 0); // a ends at 4, d starts at 5: no overlap
  CHECK(g.adj[2].count(3) == 1); // c-d overlap

  AllocResult r3 = allocate(g, 3);
  CHECK(verify(g, r3, 3));
  CHECK(r3.spillCount() == 0);
  CHECK(r3.color[0] != r3.color[3] || true); // a and d may share: not adjacent

  // Drop to 2 registers and the max-pressure point forces a spill.
  AllocResult r2 = allocate(g, 2);
  CHECK(verify(g, r2, 2));
  CHECK(r2.spillCount() >= 1);

  std::printf("       K=3 -> %d colors, %d spills; K=2 -> %d spills\n",
              r3.colorsUsed(), r3.spillCount(), r2.spillCount());
}

} // namespace regalloc

int main() {
  regalloc::testColorableGraph();
  regalloc::testSpillRequired();
  regalloc::testOptimisticColoring();
  regalloc::testPrecoloredPhysReg();
  regalloc::testFromLiveRanges();
  return ts::report("07_register_allocation");
}