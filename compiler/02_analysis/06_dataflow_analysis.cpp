// 06_dataflow_analysis.cpp — the monotone dataflow framework, twice.
//
// Every "does this optimization apply?" question in a compiler reduces to a
// dataflow problem: a lattice of facts, a transfer function per block, a meet
// operator at join points, and iteration to a fixpoint. Termination is
// guaranteed because the lattice has finite height and the transfer functions
// are monotone — facts only ever grow (or only ever shrink), never oscillate.
//
// Two instances, chosen because they are the two most commonly asked about:
//   * Live variables  — BACKWARD, meet = union. Drives dead-code elimination
//                       and, critically, register allocation (file 07).
//   * Reaching defs   — FORWARD, meet = union. Drives constant propagation and
//                       def-use chain construction.
//
// Facts are bitvectors: the representation real compilers use, because the
// meet operation becomes a machine word OR.

#include "test_support.h"

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

namespace dataflow {

struct BitVec {
  std::vector<bool> bits;

  BitVec() = default;
  explicit BitVec(size_t n) : bits(n, false) {}

  void set(size_t i) { bits[i] = true; }
  bool test(size_t i) const { return bits[i]; }

  // Returns true if anything changed — the worklist algorithm uses this to
  // decide whether to re-enqueue neighbours, which is what makes it fast.
  bool unionWith(const BitVec &o) {
    bool changed = false;
    for (size_t i = 0; i < bits.size(); ++i)
      if (o.bits[i] && !bits[i]) {
        bits[i] = true;
        changed = true;
      }
    return changed;
  }
  void subtract(const BitVec &o) {
    for (size_t i = 0; i < bits.size(); ++i)
      if (o.bits[i])
        bits[i] = false;
  }
  bool operator==(const BitVec &o) const { return bits == o.bits; }

  std::vector<std::string> toNames(const std::vector<std::string> &universe) const {
    std::vector<std::string> out;
    for (size_t i = 0; i < bits.size(); ++i)
      if (bits[i])
        out.push_back(universe[i]);
    return out; // already sorted by index; universe is kept sorted
  }
  std::vector<int> toIndices() const {
    std::vector<int> out;
    for (size_t i = 0; i < bits.size(); ++i)
      if (bits[i])
        out.push_back(static_cast<int>(i));
    return out;
  }
};

// dst == "" means the instruction defines nothing (a branch or a return).
struct Instr {
  std::string dst;
  std::vector<std::string> uses;
};

struct Blk {
  std::string name;
  std::vector<Instr> instrs;
  std::vector<int> succs;
  std::vector<int> preds;
};

struct CFG {
  std::vector<Blk> blocks;
  std::vector<std::string> variables; // sorted universe for bit indices

  void finalize() {
    for (Blk &b : blocks)
      b.preds.clear();
    for (int i = 0; i < static_cast<int>(blocks.size()); ++i)
      for (int s : blocks[i].succs)
        blocks[s].preds.push_back(i);

    variables.clear();
    for (const Blk &b : blocks)
      for (const Instr &in : b.instrs) {
        if (!in.dst.empty())
          variables.push_back(in.dst);
        for (const std::string &u : in.uses)
          variables.push_back(u);
      }
    std::sort(variables.begin(), variables.end());
    variables.erase(std::unique(variables.begin(), variables.end()),
                    variables.end());
  }

  int varIndex(const std::string &v) const {
    return static_cast<int>(
        std::lower_bound(variables.begin(), variables.end(), v) -
        variables.begin());
  }
};

// ---------------------------------------------------------------------------
// LIVE VARIABLES (backward, union).
//
//   LIVEOUT(B) = union over successors S of LIVEIN(S)
//   LIVEIN(B)  = USE(B) ∪ (LIVEOUT(B) − DEF(B))
//
// USE(B) is *upward-exposed* uses only: a variable read in B before B writes it.
// Getting that distinction right is the whole trick — `s = s + i` uses s
// (upward-exposed) and defines s, but `s = 0; x = s` does not make s live-in.
// ---------------------------------------------------------------------------
struct LivenessResult {
  std::vector<BitVec> in, out;
};

LivenessResult computeLiveness(const CFG &cfg) {
  const size_t n = cfg.blocks.size();
  const size_t v = cfg.variables.size();

  std::vector<BitVec> use(n, BitVec(v)), def(n, BitVec(v));
  for (size_t b = 0; b < n; ++b) {
    BitVec definedSoFar(v);
    for (const Instr &in : cfg.blocks[b].instrs) {
      for (const std::string &u : in.uses) {
        int idx = cfg.varIndex(u);
        if (!definedSoFar.test(idx)) // only upward-exposed uses count
          use[b].set(idx);
      }
      if (!in.dst.empty()) {
        int idx = cfg.varIndex(in.dst);
        def[b].set(idx);
        definedSoFar.set(idx);
      }
    }
  }

  LivenessResult res;
  res.in.assign(n, BitVec(v));
  res.out.assign(n, BitVec(v));

  // Backward problem -> seed the worklist with the exit blocks and propagate
  // toward the entry. (Reverse postorder of the reverse CFG would be optimal;
  // a worklist is simpler and converges to the same fixpoint.)
  std::deque<int> worklist;
  for (size_t b = 0; b < n; ++b)
    worklist.push_back(static_cast<int>(b));

  while (!worklist.empty()) {
    int b = worklist.front();
    worklist.pop_front();

    BitVec newOut(v);
    for (int s : cfg.blocks[b].succs)
      newOut.unionWith(res.in[s]);

    BitVec newIn = newOut;
    newIn.subtract(def[b]);
    newIn.unionWith(use[b]);

    res.out[b] = newOut;
    if (!(newIn == res.in[b])) {
      res.in[b] = newIn;
      for (int p : cfg.blocks[b].preds) // IN changed -> predecessors must redo
        worklist.push_back(p);
    }
  }
  return res;
}

// ---------------------------------------------------------------------------
// REACHING DEFINITIONS (forward, union).
//
//   IN(B)  = union over predecessors P of OUT(P)
//   OUT(B) = GEN(B) ∪ (IN(B) − KILL(B))
//
// GEN(B): definitions in B not overwritten later in B.
// KILL(B): every definition anywhere in the program of a variable B defines
//          (including earlier ones in B; GEN re-adds the survivors).
// Facts are definition *sites*, not variables — that's what lets you build
// def-use chains, and it's why SSA is so attractive: in SSA each use has
// exactly one reaching definition by construction, so this analysis disappears.
// ---------------------------------------------------------------------------
struct Definition {
  int block;
  int index;
  std::string var;
};

struct ReachingDefsResult {
  std::vector<Definition> definitions;
  std::vector<BitVec> in, out;
};

ReachingDefsResult computeReachingDefs(const CFG &cfg) {
  ReachingDefsResult res;
  for (int b = 0; b < static_cast<int>(cfg.blocks.size()); ++b)
    for (int i = 0; i < static_cast<int>(cfg.blocks[b].instrs.size()); ++i)
      if (!cfg.blocks[b].instrs[i].dst.empty())
        res.definitions.push_back({b, i, cfg.blocks[b].instrs[i].dst});

  const size_t n = cfg.blocks.size();
  const size_t d = res.definitions.size();

  std::vector<BitVec> gen(n, BitVec(d)), kill(n, BitVec(d));
  for (size_t b = 0; b < n; ++b) {
    std::vector<std::string> definedHere;
    for (size_t k = 0; k < d; ++k)
      if (res.definitions[k].block == static_cast<int>(b))
        definedHere.push_back(res.definitions[k].var);

    for (size_t k = 0; k < d; ++k) {
      const Definition &def = res.definitions[k];
      bool varDefinedHere = std::find(definedHere.begin(), definedHere.end(),
                                      def.var) != definedHere.end();
      if (varDefinedHere)
        kill[b].set(k);
    }
    // GEN keeps only the last definition of each variable in the block.
    for (size_t k = 0; k < d; ++k) {
      const Definition &def = res.definitions[k];
      if (def.block != static_cast<int>(b))
        continue;
      bool laterDef = false;
      for (size_t j = k + 1; j < d; ++j)
        if (res.definitions[j].block == static_cast<int>(b) &&
            res.definitions[j].var == def.var)
          laterDef = true;
      if (!laterDef)
        gen[b].set(k);
    }
  }

  res.in.assign(n, BitVec(d));
  res.out.assign(n, BitVec(d));

  std::deque<int> worklist;
  for (size_t b = 0; b < n; ++b)
    worklist.push_back(static_cast<int>(b));

  while (!worklist.empty()) {
    int b = worklist.front();
    worklist.pop_front();

    BitVec newIn(d);
    for (int p : cfg.blocks[b].preds)
      newIn.unionWith(res.out[p]);

    BitVec newOut = newIn;
    newOut.subtract(kill[b]);
    newOut.unionWith(gen[b]);

    res.in[b] = newIn;
    if (!(newOut == res.out[b])) {
      res.out[b] = newOut;
      for (int s : cfg.blocks[b].succs)
        worklist.push_back(s);
    }
  }
  return res;
}

// ---------------------------------------------------------------------------
// The same counted loop used in file 05, in "instruction list" form:
//
//   B0: i = 0; s = 0                      -> B1
//   B1: t = i < n; br t                   -> B2, B3
//   B2: s = s + i; i = i + 1              -> B1
//   B3: ret s
//
// Definition numbering (block order, then instruction order):
//   d0: i@B0   d1: s@B0   d2: t@B1   d3: s@B2   d4: i@B2
// ---------------------------------------------------------------------------
CFG makeLoopCFG() {
  CFG cfg;
  cfg.blocks.resize(4);

  cfg.blocks[0].name = "B0";
  cfg.blocks[0].instrs = {{"i", {}}, {"s", {}}};
  cfg.blocks[0].succs = {1};

  cfg.blocks[1].name = "B1";
  cfg.blocks[1].instrs = {{"t", {"i", "n"}}, {"", {"t"}}}; // second = the branch
  cfg.blocks[1].succs = {2, 3};

  cfg.blocks[2].name = "B2";
  cfg.blocks[2].instrs = {{"s", {"s", "i"}}, {"i", {"i"}}};
  cfg.blocks[2].succs = {1};

  cfg.blocks[3].name = "B3";
  cfg.blocks[3].instrs = {{"", {"s"}}}; // the return
  cfg.blocks[3].succs = {};

  cfg.finalize();
  return cfg;
}

using Names = std::vector<std::string>;

void testLiveness() {
  CFG cfg = makeLoopCFG();
  LivenessResult live = computeLiveness(cfg);
  const Names &u = cfg.variables; // {i, n, s, t}
  CHECK((u == Names{"i", "n", "s", "t"}));

  // Hand-computed fixpoint.
  CHECK((live.in[0].toNames(u) == Names{"n"}));            // only the param
  CHECK((live.out[0].toNames(u) == Names{"i", "n", "s"}));
  CHECK((live.in[1].toNames(u) == Names{"i", "n", "s"}));  // t defined before use
  CHECK((live.out[1].toNames(u) == Names{"i", "n", "s"}));
  CHECK((live.in[2].toNames(u) == Names{"i", "n", "s"}));
  CHECK((live.out[2].toNames(u) == Names{"i", "n", "s"}));
  CHECK((live.in[3].toNames(u) == Names{"s"}));
  CHECK(live.out[3].toNames(u).empty());

  // t is never live *out* of any block: it is consumed by the branch in the
  // same block. That is exactly why a register allocator can keep it in a
  // scratch register, and why SSA construction produced a dead phi for it.
  int tIdx = cfg.varIndex("t");
  for (size_t b = 0; b < cfg.blocks.size(); ++b)
    CHECK(!live.out[b].test(tIdx));

  std::printf("       live-in(B1) = ");
  for (const std::string &name : live.in[1].toNames(u))
    std::printf("%s ", name.c_str());
  std::printf("\n");
}

void testReachingDefs() {
  CFG cfg = makeLoopCFG();
  ReachingDefsResult rd = computeReachingDefs(cfg);
  CHECK(rd.definitions.size() == 5);
  CHECK(rd.definitions[0].var == "i" && rd.definitions[0].block == 0);
  CHECK(rd.definitions[4].var == "i" && rd.definitions[4].block == 2);

  using Idx = std::vector<int>;
  CHECK(rd.in[0].toIndices().empty());
  CHECK((rd.out[0].toIndices() == Idx{0, 1}));
  CHECK((rd.in[1].toIndices() == Idx{0, 1, 2, 3, 4})); // loop back-edge merges
  CHECK((rd.out[1].toIndices() == Idx{0, 1, 2, 3, 4}));
  CHECK((rd.in[2].toIndices() == Idx{0, 1, 2, 3, 4}));
  CHECK((rd.out[2].toIndices() == Idx{2, 3, 4}));      // d0/d1 killed by d4/d3
  CHECK((rd.in[3].toIndices() == Idx{0, 1, 2, 3, 4}));

  // Practical consequence: the use of `s` in B3 has TWO reaching definitions
  // (d1 from the entry and d3 from the loop body), so a naive constant
  // propagation cannot replace it. In SSA that use reads a single phi instead.
  int reaching = 0;
  for (size_t k = 0; k < rd.definitions.size(); ++k)
    if (rd.definitions[k].var == "s" && rd.in[3].test(k))
      ++reaching;
  CHECK(reaching == 2);
}

} // namespace dataflow

int main() {
  dataflow::testLiveness();
  dataflow::testReachingDefs();
  return ts::report("06_dataflow_analysis");
}