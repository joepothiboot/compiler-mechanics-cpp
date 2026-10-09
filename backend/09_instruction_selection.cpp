// 09_instruction_selection.cpp — instruction selection by tree tiling.
//
// After optimization, each basic block's computations form expression trees
// (DAGs, really; LLVM's SelectionDAG). Instruction selection COVERS each tree
// with TILES: one tile per machine instruction, where a tile is a tree pattern
// the instruction implements. Each tile has a cost, and we want a cover of
// minimum total cost.
//
//   a + (b << 2) + 8                       Two coverings on x86-64:
//
//            Add  ─────────────┐           (A) one tile, "3-component LEA":
//           /   \              │               lea t0, [a + b*4 + 8]
//         Add   Const 8        │ lea [b+i*s+d]
//        /   \                 │           (B) two tiles:
//      a     Shl ──┐           │               lea t0, [a + b*4]   ┐ lea [b+i*s]
//           /   \  │ (scale)   │               add t0, 8           ┘ add r, imm
//          b   Const 2         │
//                  ┘───────────┘
//
// Which one is better depends on the CPU, not on the tree. On Skylake a LEA with
// three components (base + index + displacement) has 3-cycle latency and runs on
// one port, so (B) wins (1 + 1 cycles). On Ice Lake and Zen 4 the 3-component
// LEA is fast, so (A) wins. Clang agrees (`clang -O2 --target=x86_64-linux-gnu -S`):
//   -march=skylake        → lea rax, [rdi + 4*rsi]; add rax, 8
//   -march=icelake-server → lea rax, [rdi + 4*rsi + 8]
//
// TWO ALGORITHMS
//   * MAXIMAL MUNCH (greedy, top-down): at each node, take the tile that covers
//     the MOST nodes, then recurse into the uncovered subtrees. Linear time,
//     simple, and optimal when bigger tiles are never more expensive. It isn't
//     optimal in general: on Skylake costs it picks (A).
//   * DYNAMIC PROGRAMMING (bottom-up, Aho–Ganapathi–Tjiang / BURS): for every
//     node compute bestCost(n) = min over tiles t matching at n of
//     cost(t) + Σ bestCost(leaf). This is optimal for TREES, in time
//     O(nodes × tiles). For DAGs with shared subexpressions, optimal tiling is
//     NP-complete, which is why selectors cut DAGs into trees, or tile them
//     greedily.
//
// 2-ADDRESS vs 3-ADDRESS
//   x86 "add dst, src" overwrites its first operand (dst = dst + src). If that
//   operand is a variable still needed later, we have to copy it first:
//   "mov t, a; add t, b". RISC ISAs (AArch64, RISC-V), and x86's new APX "NDD"
//   forms, have "add d, a, b" and no copy. LEA is x86's non-destructive 3-address
//   add, which is a big reason compilers use it for arithmetic: `x*5` becomes
//   `lea r, [x + x*4]`, which doesn't need a copy and avoids a 3-cycle imul.
//
// LLVM: SelectionDAG uses TableGen-generated matchers (a DP-flavoured tree
// matcher with pattern complexity as the priority); GlobalISel is the newer
// framework. GCC matches RTL against machine-description patterns. Cranelift
// uses ISLE, a term-rewriting DSL compiled into a matcher.

#include "test_support.h"

#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace isel {

enum class Op { Var, Const, Add, Mul, Shl, Load };

struct Node {
  Op op;
  long imm = 0;
  std::string name; // for Var
  int lhs = -1, rhs = -1;
};

struct Tree {
  std::vector<Node> nodes;

  int var(std::string n) { return push({.op = Op::Var, .name = std::move(n)}); }
  int cnst(long v) { return push({.op = Op::Const, .imm = v}); }
  int add(int l, int r) { return push({.op = Op::Add, .lhs = l, .rhs = r}); }
  int mul(int l, int r) { return push({.op = Op::Mul, .lhs = l, .rhs = r}); }
  int shl(int l, int r) { return push({.op = Op::Shl, .lhs = l, .rhs = r}); }
  int load(int addr) { return push({.op = Op::Load, .lhs = addr}); }

  const Node &operator[](int i) const { return nodes[i]; }
  int size(int n) const {
    return n < 0 ? 0 : 1 + size(nodes[n].lhs) + size(nodes[n].rhs);
  }

private:
  int push(Node n) {
    nodes.push_back(std::move(n));
    return static_cast<int>(nodes.size()) - 1;
  }
};

// --- Emission ---------------------------------------------------------------

struct Emitter {
  bool twoAddress = true; // false models AArch64 / RISC-V / x86 APX NDD
  std::vector<std::string> code;
  std::set<std::string> temps;

  std::string fresh() {
    std::string t = "t" + std::to_string(temps.size());
    temps.insert(t);
    return t;
  }
  // Arithmetic in the ISA's native operand form. In 2-address form the left
  // operand is overwritten, which is fine for a temp (each tree value has one use)
  // but needs a copy for a variable that may still be live.
  std::string binop(std::string_view op, const std::string &a, const std::string &b) {
    if (!twoAddress) {
      std::string d = fresh();
      code.push_back(std::format("{} {}, {}, {}", op, d, a, b));
      return d;
    }
    std::string d = a;
    if (!temps.contains(a)) {
      d = fresh();
      code.push_back(std::format("mov {}, {}", d, a));
    }
    code.push_back(std::format("{} {}, {}", op, d, b));
    return d;
  }
  // Instructions that are 3-address even on x86 (lea, mov from memory, imul imm).
  std::string op3(std::string_view mnemonic, std::string_view operands) {
    std::string d = fresh();
    code.push_back(std::format("{} {}, {}", mnemonic, d, operands));
    return d;
  }
};

// --- Tiles --------------------------------------------------------------------

struct Match {
  std::vector<int> leaves; // subtrees the tile does NOT cover (need registers)
  long imm = 0;            // immediate / displacement folded into the instruction
  long scale = 0;          // index scale for addressing-mode tiles
};

struct CostModel {
  int lea3 = 3;  // base + index*scale + disp: 3 on Skylake, 1 on Ice Lake / Zen 4
  int imul = 3;
  int load = 4;  // L1 hit
};

struct Tile {
  std::string_view name;
  std::function<int(const CostModel &)> cost;
  std::function<std::optional<Match>(const Tree &, int)> match;
  std::function<std::string(Emitter &, const Tree &, int, const Match &,
                            std::span<const std::string>)>
      emit;
};

namespace pat {
bool is(const Tree &t, int n, Op op) { return n >= 0 && t[n].op == op; }

std::optional<long> constant(const Tree &t, int n) {
  if (is(t, n, Op::Const))
    return t[n].imm;
  return std::nullopt;
}

// index << k (k in 1..3) or index * {2,4,8}: an x86 scaled-index operand.
struct Scaled { int index; long scale; };
std::optional<Scaled> scaled(const Tree &t, int n) {
  if (is(t, n, Op::Shl))
    if (auto k = constant(t, t[n].rhs); k && *k >= 1 && *k <= 3)
      return Scaled{t[n].lhs, 1L << *k};
  if (is(t, n, Op::Mul))
    if (auto c = constant(t, t[n].rhs); c && (*c == 2 || *c == 4 || *c == 8))
      return Scaled{t[n].lhs, *c};
  return std::nullopt;
}

// base + index*scale  (the "SIB" part of an x86 address)
struct Sib { int base, index; long scale; };
std::optional<Sib> sib(const Tree &t, int n) {
  if (is(t, n, Op::Add))
    if (auto s = scaled(t, t[n].rhs))
      return Sib{t[n].lhs, s->index, s->scale};
  return std::nullopt;
}
} // namespace pat

const std::vector<Tile> &x86Tiles() {
  using namespace pat;
  using Ops = std::span<const std::string>;
  auto fixed = [](int c) { return [c](const CostModel &) { return c; }; };

  static const std::vector<Tile> tiles = {
      {"var", fixed(0),
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Var)) return Match{};
         return std::nullopt;
       },
       [](Emitter &, const Tree &t, int n, const Match &, Ops) { return t[n].name; }},

      {"mov_imm", fixed(1),
       [](const Tree &t, int n) -> std::optional<Match> {
         if (auto c = constant(t, n)) return Match{{}, *c};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &m, Ops) {
         return e.op3("mov", std::to_string(m.imm));
       }},

      {"add", fixed(1),
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Add)) return Match{{t[n].lhs, t[n].rhs}};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &, Ops o) {
         return e.binop("add", o[0], o[1]);
       }},

      {"add_imm", fixed(1),
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Add))
           if (auto c = constant(t, t[n].rhs)) return Match{{t[n].lhs}, *c};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &m, Ops o) {
         return e.binop("add", o[0], std::to_string(m.imm));
       }},

      {"imul", [](const CostModel &c) { return c.imul; },
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Mul)) return Match{{t[n].lhs, t[n].rhs}};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &, Ops o) {
         return e.binop("imul", o[0], o[1]);
       }},

      // imul r, r/m, imm is a genuine 3-operand x86 instruction.
      {"imul_imm", [](const CostModel &c) { return c.imul; },
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Mul))
           if (auto c = constant(t, t[n].rhs)) return Match{{t[n].lhs}, *c};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &m, Ops o) {
         return e.op3("imul", std::format("{}, {}", o[0], m.imm));
       }},

      {"shl", fixed(1),
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Shl)) return Match{{t[n].lhs, t[n].rhs}};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &, Ops o) {
         return e.binop("shl", o[0], o[1]);
       }},

      {"shl_imm", fixed(1),
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Shl))
           if (auto c = constant(t, t[n].rhs)) return Match{{t[n].lhs}, *c};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &m, Ops o) {
         return e.binop("shl", o[0], std::to_string(m.imm));
       }},

      // x * {3,5,9}  →  lea d, [x + x*{2,4,8}]
      {"lea_mul", fixed(1),
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Mul))
           if (auto c = constant(t, t[n].rhs); c && (*c == 3 || *c == 5 || *c == 9))
             return Match{{t[n].lhs}, 0, *c - 1};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &m, Ops o) {
         return e.op3("lea", std::format("[{0} + {0}*{1}]", o[0], m.scale));
       }},

      {"lea_sib", fixed(1),
       [](const Tree &t, int n) -> std::optional<Match> {
         if (auto s = sib(t, n)) return Match{{s->base, s->index}, 0, s->scale};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &m, Ops o) {
         return e.op3("lea", std::format("[{} + {}*{}]", o[0], o[1], m.scale));
       }},

      // The "slow LEA" on Skylake: 3 components (base + index*scale + disp).
      {"lea_sib_disp", [](const CostModel &c) { return c.lea3; },
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Add))
           if (auto d = constant(t, t[n].rhs))
             if (auto s = sib(t, t[n].lhs))
               return Match{{s->base, s->index}, *d, s->scale};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &m, Ops o) {
         return e.op3("lea", std::format("[{} + {}*{} + {}]", o[0], o[1], m.scale, m.imm));
       }},

      {"load", [](const CostModel &c) { return c.load; },
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Load)) return Match{{t[n].lhs}};
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &, Ops o) {
         return e.op3("mov", std::format("[{}]", o[0]));
       }},

      // Addressing modes are free inside a load: [base + index*scale + disp].
      {"load_sib_disp", [](const CostModel &c) { return c.load; },
       [](const Tree &t, int n) -> std::optional<Match> {
         if (is(t, n, Op::Load) && is(t, t[n].lhs, Op::Add)) {
           const int a = t[n].lhs;
           if (auto d = constant(t, t[a].rhs))
             if (auto s = sib(t, t[a].lhs))
               return Match{{s->base, s->index}, *d, s->scale};
         }
         return std::nullopt;
       },
       [](Emitter &e, const Tree &, int, const Match &m, Ops o) {
         return e.op3("mov", std::format("[{} + {}*{} + {}]", o[0], o[1], m.scale, m.imm));
       }},
  };
  return tiles;
}

// --- Selection ----------------------------------------------------------------

struct Selection {
  int cost = 0;
  std::vector<std::string_view> tiles; // in emission order
  std::vector<std::string> code;
};

class Selector {
public:
  Selector(const Tree &t, CostModel cm, bool twoAddress)
      : tree_(t), cm_(cm) {
    emitter_.twoAddress = twoAddress;
  }

  Selection maximalMunch(int root) {
    chooser_ = [this](int n) { return largestTile(n); };
    return finish(root);
  }

  Selection dynamicProgramming(int root) {
    best_.clear();
    cost_.clear();
    bestCost(root);
    chooser_ = [this](int n) { return best_.at(n); };
    return finish(root);
  }

private:
  using Choice = std::pair<const Tile *, Match>;

  // Greedy: most nodes covered; ties go to the cheaper tile.
  Choice largestTile(int n) const {
    std::optional<Choice> pick;
    int pickCovered = -1, pickCost = 0;
    for (const Tile &tile : x86Tiles()) {
      auto m = tile.match(tree_, n);
      if (!m)
        continue;
      int covered = tree_.size(n);
      for (int leaf : m->leaves)
        covered -= tree_.size(leaf);
      const int cost = tile.cost(cm_);
      if (covered > pickCovered || (covered == pickCovered && cost < pickCost)) {
        pick = Choice{&tile, *m};
        pickCovered = covered;
        pickCost = cost;
      }
    }
    return *pick; // every Op has at least one tile
  }

  int bestCost(int n) {
    if (auto it = cost_.find(n); it != cost_.end())
      return it->second;
    int bestTotal = std::numeric_limits<int>::max();
    std::optional<Choice> pick;
    for (const Tile &tile : x86Tiles()) {
      auto m = tile.match(tree_, n);
      if (!m)
        continue;
      int total = tile.cost(cm_);
      for (int leaf : m->leaves)
        total += bestCost(leaf);
      if (total < bestTotal) {
        bestTotal = total;
        pick = Choice{&tile, *m};
      }
    }
    best_.emplace(n, *pick);
    cost_[n] = bestTotal;
    return bestTotal;
  }

  Selection finish(int root) {
    Selection s;
    emit(root, s);
    s.code = std::move(emitter_.code);
    return s;
  }

  std::string emit(int n, Selection &s) {
    auto [tile, match] = chooser_(n);
    std::vector<std::string> ops;
    for (int leaf : match.leaves)
      ops.push_back(emit(leaf, s));
    s.cost += tile->cost(cm_);
    if (tile->name != "var")
      s.tiles.push_back(tile->name);
    return tile->emit(emitter_, tree_, n, match, ops);
  }

  const Tree &tree_;
  CostModel cm_;
  Emitter emitter_;
  std::function<Choice(int)> chooser_;
  std::map<int, Choice> best_;
  std::map<int, int> cost_;
};

Selection munch(const Tree &t, int root, CostModel cm = {}, bool twoAddress = true) {
  return Selector(t, cm, twoAddress).maximalMunch(root);
}
Selection dp(const Tree &t, int root, CostModel cm = {}, bool twoAddress = true) {
  return Selector(t, cm, twoAddress).dynamicProgramming(root);
}

} // namespace isel

using namespace isel;

void testMulByFiveUsesLea() {
  Tree t;
  const int root = t.mul(t.var("x"), t.cnst(5));
  for (const Selection &s : {munch(t, root), dp(t, root)}) {
    CHECK(s.code == std::vector<std::string>{"lea t0, [x + x*4]"});
    CHECK(s.cost == 1); // vs imul: 3 cycles
  }
}

// The tree from the header: a + (b << 2) + 8.
int slowLeaTree(Tree &t) {
  return t.add(t.add(t.var("a"), t.shl(t.var("b"), t.cnst(2))), t.cnst(8));
}

void testMunchIsNotOptimalOnSkylake() {
  Tree t;
  const int root = slowLeaTree(t);
  const CostModel skylake{.lea3 = 3};

  const auto greedy = munch(t, root, skylake);
  CHECK(greedy.code == std::vector<std::string>{"lea t0, [a + b*4 + 8]"});
  CHECK(greedy.cost == 3);

  const auto optimal = dp(t, root, skylake);
  CHECK(optimal.code == (std::vector<std::string>{"lea t0, [a + b*4]", "add t0, 8"}));
  CHECK(optimal.cost == 2); // matches clang -march=skylake
}

void testSameTreeDifferentCpu() {
  Tree t;
  const int root = slowLeaTree(t);
  const auto icelake = dp(t, root, CostModel{.lea3 = 1});
  CHECK(icelake.code == std::vector<std::string>{"lea t0, [a + b*4 + 8]"});
  CHECK(icelake.cost == 1); // matches clang -march=icelake-server
}

void testAddressingModeFoldsIntoLoad() {
  // a[i + 2] with 8-byte elements: *(a + (i << 3) + 16)
  Tree t;
  const int root = t.load(t.add(t.add(t.var("a"), t.shl(t.var("i"), t.cnst(3))), t.cnst(16)));
  for (const Selection &s : {munch(t, root), dp(t, root)}) {
    CHECK(s.code == std::vector<std::string>{"mov t0, [a + i*8 + 16]"});
    CHECK(s.tiles == std::vector<std::string_view>{"load_sib_disp"});
  }
}

void testTwoVersusThreeAddress() {
  Tree t;
  const int root = t.add(t.add(t.var("a"), t.var("b")), t.var("c"));
  const auto x86 = dp(t, root, {}, /*twoAddress=*/true);
  CHECK(x86.code == (std::vector<std::string>{"mov t0, a", "add t0, b", "add t0, c"}));
  const auto risc = dp(t, root, {}, /*twoAddress=*/false);
  CHECK(risc.code == (std::vector<std::string>{"add t0, a, b", "add t1, t0, c"}));
}

// Property: DP is optimal, so it is never worse than greedy.
void testDpNeverWorseThanMunch() {
  std::mt19937 rng(7);
  const long consts[] = {1, 2, 3, 4, 5, 8, 9, 16};
  std::function<int(Tree &, int)> gen = [&](Tree &t, int depth) -> int {
    if (depth == 0 || rng() % 4 == 0)
      return rng() % 2 ? t.var(std::string(1, char('a' + rng() % 4)))
                       : t.cnst(consts[rng() % 8]);
    switch (rng() % 4) {
    case 0: return t.add(gen(t, depth - 1), gen(t, depth - 1));
    case 1: return t.mul(gen(t, depth - 1), gen(t, depth - 1));
    case 2: return t.shl(gen(t, depth - 1), t.cnst(1 + rng() % 3));
    default: return t.load(gen(t, depth - 1));
    }
  };
  for (int trial = 0; trial < 300; ++trial) {
    Tree t;
    const int root = gen(t, 5);
    for (CostModel cm : {CostModel{.lea3 = 3}, CostModel{.lea3 = 1}})
      CHECK_MSG(dp(t, root, cm).cost <= munch(t, root, cm).cost,
                "trial " + std::to_string(trial));
  }
}

int main() {
  testMulByFiveUsesLea();
  testMunchIsNotOptimalOnSkylake();
  testSameTreeDifferentCpu();
  testAddressingModeFoldsIntoLoad();
  testTwoVersusThreeAddress();
  testDpNeverWorseThanMunch();
  return ts::report("09_instruction_selection");
}

// ---------------------------------------------------------------------------
// CHECKPOINT
//
// Q1. DP tiling is optimal for trees, but `(a+b) * (a+b)` is a DAG: the Add is
//     shared. If you duplicate shared nodes to make a tree, you may compute
//     a+b twice. If you cut the DAG at shared nodes, a tile can no longer
//     reach "through" the shared node (e.g. fold it into an addressing mode).
//     When is each choice better? (Hint: what does folding a shared address
//     computation into two different loads cost?)
//
// Q2. Clang turns x*7 into `lea rax, [8*rdi]; sub rax, rdi` (2 instructions, 2
//     cycles), not `imul rax, rdi, 7` (1 instruction, 3 cycles). Which metric
//     is the selector minimizing, and when would the imul be the right choice?
//     (Hint: throughput vs latency, and -Os.)
//
// CHALLENGE: add a tile for x*7 → "lea t, [x*8]; sub t, x" (cost 2), and an
//     `add_mem` tile, Add(x, Load(addr)) → "add t, [addr]" (x86's CISC
//     memory-operand form). Write a test where the memory-operand tile makes
//     DP beat a load + add pair, and check DP ≤ munch still holds.
