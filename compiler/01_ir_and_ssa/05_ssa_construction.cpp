// 05_ssa_construction.cpp — converting a mutable-variable CFG into SSA.
//
// This is the transformation LLVM's mem2reg pass performs, and it is the single
// most-asked "do you actually understand IR?" question. The classic algorithm
// (Cytron et al. 1991) is implemented here end to end:
//
//   1. Build the CFG and predecessor lists.
//   2. Compute immediate dominators (Cooper/Harvey/Kennedy iterative algorithm
//      — the one LLVM's GenericDomTree is modeled on, minus the SLT refinement).
//   3. Compute dominance frontiers: DF(n) = blocks where n's dominance "runs
//      out", i.e. exactly where a definition in n may need a phi.
//   4. Insert phi nodes at the iterated dominance frontier of each variable's
//      definition sites.
//   5. Rename: DFS the dominator tree carrying a stack of current versions per
//      variable; each definition pushes a new version, each use reads the top,
//      and every edge into a successor fills in one phi operand.
//   6. Delete dead phis (this is the difference between "minimal SSA" and
//      "pruned SSA", which real compilers get via a liveness query).
//
// The test verifies the interesting property: an interpreter produces identical
// results before and after the transformation, for several inputs.

#include "test_support.h"

#include <cassert>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ssa {

// ---------------------------------------------------------------------------
// A deliberately tiny IR: enough for a loop and a diamond, small enough that
// every algorithm below fits on a screen.
// ---------------------------------------------------------------------------
enum class Op { Const, Copy, Add, Sub, Mul, Lt };

struct Operand {
  bool isConst = false;
  long long value = 0;
  std::string name;

  static Operand imm(long long v) {
    Operand o;
    o.isConst = true;
    o.value = v;
    return o;
  }
  static Operand var(std::string n) {
    Operand o;
    o.name = std::move(n);
    return o;
  }
  std::string str() const { return isConst ? std::to_string(value) : name; }
};

struct Stmt {
  std::string dst;
  Op op;
  std::vector<Operand> ops;
};

// A phi remembers the original variable it was created for; renaming needs that
// to know which stack to read when filling operands from each predecessor.
struct Phi {
  std::string var;
  std::string dst;
  std::vector<std::pair<int, std::string>> args; // (predecessor block, value)
};

enum class TermKind { Jmp, Br, Ret };

struct Terminator {
  TermKind kind = TermKind::Ret;
  Operand cond;   // Br
  Operand retVal; // Ret
  int target = -1, ifTrue = -1, ifFalse = -1;
};

struct Block {
  std::string name;
  std::vector<Phi> phis;
  std::vector<Stmt> stmts;
  Terminator term;
  std::vector<int> preds;
};

struct Function {
  std::string name;
  std::vector<std::string> params; // live-in variables
  std::vector<std::pair<std::string, std::string>> paramNames; // orig -> SSA
  std::vector<Block> blocks; // blocks[0] is the entry block
};

std::vector<int> successors(const Block &b) {
  switch (b.term.kind) {
  case TermKind::Jmp: return {b.term.target};
  case TermKind::Br: return {b.term.ifTrue, b.term.ifFalse};
  case TermKind::Ret: return {};
  }
  return {};
}

void computePreds(Function &f) {
  for (Block &b : f.blocks)
    b.preds.clear();
  for (int i = 0; i < static_cast<int>(f.blocks.size()); ++i)
    for (int s : successors(f.blocks[i]))
      f.blocks[s].preds.push_back(i);
}

// ---------------------------------------------------------------------------
// Interpreter. Works on both the pre-SSA and post-SSA forms because a phi is
// just "pick the operand matching the predecessor we came from". Tracking
// `prev` is exactly why phis are not real machine instructions — they encode
// control-flow history, and the backend later lowers them to copies on edges.
// ---------------------------------------------------------------------------
using Env = std::map<std::string, long long>;

long long readOperand(const Operand &o, const Env &env) {
  if (o.isConst)
    return o.value;
  // Undef arises for phi operands on paths where the variable was never
  // defined; LLVM materializes `undef`/`poison` here. Zero keeps the test
  // deterministic, and we separately assert no undef survives.
  if (o.name.rfind("undef_", 0) == 0)
    return 0;
  auto it = env.find(o.name);
  assert(it != env.end() && "use of undefined value");
  return it->second;
}

long long evalStmt(const Stmt &s, const Env &env) {
  long long a = s.ops.empty() ? 0 : readOperand(s.ops[0], env);
  long long b = s.ops.size() > 1 ? readOperand(s.ops[1], env) : 0;
  switch (s.op) {
  case Op::Const:
  case Op::Copy: return a;
  case Op::Add: return a + b;
  case Op::Sub: return a - b;
  case Op::Mul: return a * b;
  case Op::Lt: return a < b ? 1 : 0;
  }
  return 0;
}

long long interpret(const Function &f, Env env) {
  int cur = 0, prev = -1;
  for (int step = 0; step < 100000; ++step) {
    const Block &b = f.blocks[cur];

    // Phis execute *simultaneously* on block entry: a phi must not observe the
    // result of a sibling phi in the same block (the classic "swap problem"
    // that makes naive phi elimination incorrect).
    std::vector<std::pair<std::string, long long>> phiResults;
    for (const Phi &p : b.phis) {
      bool found = false;
      for (const auto &arg : p.args) {
        if (arg.first == prev) {
          phiResults.emplace_back(p.dst, readOperand(Operand::var(arg.second), env));
          found = true;
          break;
        }
      }
      assert(found && "phi has no operand for the incoming edge");
      (void)found;
    }
    for (const auto &pr : phiResults)
      env[pr.first] = pr.second;

    for (const Stmt &s : b.stmts) {
      long long v = evalStmt(s, env); // compute before inserting into env
      env[s.dst] = v;
    }

    switch (b.term.kind) {
    case TermKind::Ret: return readOperand(b.term.retVal, env);
    case TermKind::Jmp: prev = cur; cur = b.term.target; break;
    case TermKind::Br:
      prev = cur;
      cur = readOperand(b.term.cond, env) ? b.term.ifTrue : b.term.ifFalse;
      break;
    }
  }
  assert(false && "interpreter did not terminate");
  return -1;
}

// ---------------------------------------------------------------------------
// Reverse postorder: the visit order that makes the iterative dominator
// solver converge in (usually) two passes. Every forward dataflow problem in a
// compiler wants this order.
// ---------------------------------------------------------------------------
void postOrderDFS(const Function &f, int b, std::vector<bool> &visited,
                  std::vector<int> &post) {
  visited[b] = true;
  for (int s : successors(f.blocks[b]))
    if (!visited[s])
      postOrderDFS(f, s, visited, post);
  post.push_back(b);
}

std::vector<int> reversePostOrder(const Function &f) {
  std::vector<bool> visited(f.blocks.size(), false);
  std::vector<int> post;
  postOrderDFS(f, 0, visited, post);
  return std::vector<int>(post.rbegin(), post.rend());
}

// Walk both nodes up the (partially built) dominator tree until they meet.
// Comparing RPO numbers keeps the walk going "up" toward the entry.
int intersect(int a, int b, const std::vector<int> &idom,
              const std::vector<int> &rpoNum) {
  while (a != b) {
    while (rpoNum[a] > rpoNum[b])
      a = idom[a];
    while (rpoNum[b] > rpoNum[a])
      b = idom[b];
  }
  return a;
}

std::vector<int> computeIDom(const Function &f) {
  const int n = static_cast<int>(f.blocks.size());
  std::vector<int> rpo = reversePostOrder(f);
  std::vector<int> rpoNum(n, -1);
  for (int i = 0; i < static_cast<int>(rpo.size()); ++i)
    rpoNum[rpo[i]] = i;

  std::vector<int> idom(n, -1);
  idom[0] = 0; // the entry dominates itself; the fixpoint needs a seed
  bool changed = true;
  while (changed) {
    changed = false;
    for (int b : rpo) {
      if (b == 0)
        continue;
      int newIdom = -1;
      for (int p : f.blocks[b].preds) {
        if (idom[p] == -1)
          continue; // not processed yet on the first sweep
        newIdom = (newIdom == -1) ? p : intersect(p, newIdom, idom, rpoNum);
      }
      if (newIdom != -1 && idom[b] != newIdom) {
        idom[b] = newIdom;
        changed = true;
      }
    }
  }
  return idom;
}

// DF(n) = the set of blocks that n dominates a predecessor of, but does not
// strictly dominate. Cooper's formulation: for every join point, walk from each
// predecessor up to (but not including) the join's immediate dominator.
std::vector<std::set<int>> computeDominanceFrontiers(const Function &f,
                                                     const std::vector<int> &idom) {
  std::vector<std::set<int>> df(f.blocks.size());
  for (int b = 0; b < static_cast<int>(f.blocks.size()); ++b) {
    if (f.blocks[b].preds.size() < 2)
      continue; // only join points can need phis
    for (int p : f.blocks[b].preds) {
      int runner = p;
      while (runner != idom[b]) {
        df[runner].insert(b);
        runner = idom[runner];
      }
    }
  }
  return df;
}

// ---------------------------------------------------------------------------
// The full conversion.
// ---------------------------------------------------------------------------
Function buildSSA(const Function &orig) {
  Function f = orig; // rewrite a copy in place
  computePreds(f);
  std::vector<int> idom = computeIDom(f);
  std::vector<std::set<int>> df = computeDominanceFrontiers(f, idom);

  // --- Step 1: where is each variable defined? -----------------------------
  std::map<std::string, std::set<int>> defSites;
  for (const std::string &p : f.params)
    defSites[p].insert(0); // parameters are "defined" on function entry
  for (int b = 0; b < static_cast<int>(f.blocks.size()); ++b)
    for (const Stmt &s : f.blocks[b].stmts)
      defSites[s.dst].insert(b);

  // --- Step 2: phi placement at the iterated dominance frontier -------------
  for (const auto &entry : defSites) {
    const std::string &var = entry.first;
    std::set<int> worklist = entry.second;
    std::set<int> placed;
    while (!worklist.empty()) {
      int b = *worklist.begin();
      worklist.erase(worklist.begin());
      for (int d : df[b]) {
        if (!placed.insert(d).second)
          continue;
        Phi phi;
        phi.var = var;
        f.blocks[d].phis.push_back(phi);
        // A phi is itself a definition, so it can force further phis further
        // down — this is what makes the frontier "iterated".
        if (!entry.second.count(d))
          worklist.insert(d);
      }
    }
  }

  // --- Step 3: renaming ----------------------------------------------------
  std::map<std::string, std::vector<std::string>> stacks;
  std::map<std::string, int> counters;

  auto pushName = [&](const std::string &var) {
    std::string versioned = var + "_" + std::to_string(counters[var]++);
    stacks[var].push_back(versioned);
    return versioned;
  };
  auto topName = [&](const std::string &var) -> std::string {
    auto it = stacks.find(var);
    if (it == stacks.end() || it->second.empty())
      return "undef_" + var; // variable not defined on this path
    return it->second.back();
  };

  // Dominator-tree children, derived from idom.
  std::vector<std::vector<int>> domChildren(f.blocks.size());
  for (int b = 1; b < static_cast<int>(f.blocks.size()); ++b)
    domChildren[idom[b]].push_back(b);

  for (const std::string &p : f.params)
    f.paramNames.emplace_back(p, pushName(p)); // never popped: live everywhere

  std::function<void(int)> rename = [&](int b) {
    std::vector<std::string> pushedHere;
    Block &bb = f.blocks[b];

    for (Phi &p : bb.phis) {
      p.dst = pushName(p.var);
      pushedHere.push_back(p.var);
    }
    for (Stmt &s : bb.stmts) {
      for (Operand &o : s.ops)
        if (!o.isConst)
          o.name = topName(o.name); // uses read the *current* version first
      std::string origDst = s.dst;
      s.dst = pushName(origDst); // then the def creates the next version
      pushedHere.push_back(origDst);
    }
    if (bb.term.kind == TermKind::Br && !bb.term.cond.isConst)
      bb.term.cond.name = topName(bb.term.cond.name);
    if (bb.term.kind == TermKind::Ret && !bb.term.retVal.isConst)
      bb.term.retVal.name = topName(bb.term.retVal.name);

    // Fill in one operand per outgoing edge in each successor's phis. Because
    // the dominator-tree DFS visits every block exactly once, every phi ends up
    // with exactly one operand per predecessor.
    for (int s : successors(bb))
      for (Phi &p : f.blocks[s].phis)
        p.args.emplace_back(b, topName(p.var));

    for (int child : domChildren[b])
      rename(child);

    for (auto it = pushedHere.rbegin(); it != pushedHere.rend(); ++it)
      stacks[*it].pop_back(); // leaving this subtree: restore outer versions
  };
  rename(0);

  return f;
}

// ---------------------------------------------------------------------------
// Minimal SSA can place phis for values that are dead at the join (our `t < n`
// temporary is the usual culprit). Real compilers avoid them up front using
// liveness ("pruned SSA"); removing them afterward is equivalent and shows the
// classic use-set fixpoint.
// ---------------------------------------------------------------------------
void eliminateDeadPhis(Function &f) {
  bool changed = true;
  while (changed) {
    changed = false;
    std::set<std::string> used;
    for (const Block &b : f.blocks) {
      for (const Stmt &s : b.stmts)
        for (const Operand &o : s.ops)
          if (!o.isConst)
            used.insert(o.name);
      if (b.term.kind == TermKind::Br && !b.term.cond.isConst)
        used.insert(b.term.cond.name);
      if (b.term.kind == TermKind::Ret && !b.term.retVal.isConst)
        used.insert(b.term.retVal.name);
      for (const Phi &p : b.phis)
        for (const auto &arg : p.args)
          used.insert(arg.second);
    }
    for (Block &b : f.blocks) {
      auto it = b.phis.begin();
      while (it != b.phis.end()) {
        if (used.count(it->dst) == 0) {
          it = b.phis.erase(it);
          changed = true; // removing a phi may kill the phis it referenced
        } else {
          ++it;
        }
      }
    }
  }
}

std::string printFunction(const Function &f) {
  std::string out = f.name + ":\n";
  for (const Block &b : f.blocks) {
    out += "  " + b.name + ":\n";
    for (const Phi &p : b.phis) {
      out += "    " + p.dst + " = phi";
      for (const auto &arg : p.args)
        out += " [" + f.blocks[arg.first].name + ": " + arg.second + "]";
      out += "\n";
    }
    for (const Stmt &s : b.stmts) {
      static const char *names[] = {"const", "copy", "add", "sub", "mul", "lt"};
      out += "    " + s.dst + " = " + names[static_cast<int>(s.op)];
      for (const Operand &o : s.ops)
        out += " " + o.str();
      out += "\n";
    }
    switch (b.term.kind) {
    case TermKind::Ret: out += "    ret " + b.term.retVal.str() + "\n"; break;
    case TermKind::Jmp: out += "    jmp " + f.blocks[b.term.target].name + "\n"; break;
    case TermKind::Br:
      out += "    br " + b.term.cond.str() + ", " + f.blocks[b.term.ifTrue].name +
             ", " + f.blocks[b.term.ifFalse].name + "\n";
      break;
    }
  }
  return out;
}

bool hasUndefOperands(const Function &f) {
  auto isUndef = [](const std::string &n) { return n.rfind("undef_", 0) == 0; };
  for (const Block &b : f.blocks) {
    for (const Stmt &s : b.stmts)
      for (const Operand &o : s.ops)
        if (!o.isConst && isUndef(o.name))
          return true;
    for (const Phi &p : b.phis)
      for (const auto &arg : p.args)
        if (isUndef(arg.second))
          return true;
    if (b.term.kind == TermKind::Br && !b.term.cond.isConst && isUndef(b.term.cond.name))
      return true;
    if (b.term.kind == TermKind::Ret && !b.term.retVal.isConst && isUndef(b.term.retVal.name))
      return true;
  }
  return false;
}

bool isSingleAssignment(const Function &f) {
  std::set<std::string> seen;
  for (const Block &b : f.blocks) {
    for (const Phi &p : b.phis)
      if (!seen.insert(p.dst).second)
        return false;
    for (const Stmt &s : b.stmts)
      if (!seen.insert(s.dst).second)
        return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Test program 1: a counted loop. `i` and `s` are reassigned across a back edge,
// which is the case that needs a phi in the loop header.
//
//   entry: i = 0; s = 0;           jmp header
//   header: t = i < n;             br t, body, exit
//   body:  s = s + i; i = i + 1;   jmp header
//   exit:  ret s
// ---------------------------------------------------------------------------
Function makeLoopFunction() {
  Function f;
  f.name = "sum_below";
  f.params = {"n"};
  f.blocks.resize(4);

  Block &entry = f.blocks[0];
  entry.name = "entry";
  entry.stmts = {{"i", Op::Const, {Operand::imm(0)}},
                 {"s", Op::Const, {Operand::imm(0)}}};
  entry.term.kind = TermKind::Jmp;
  entry.term.target = 1;

  Block &header = f.blocks[1];
  header.name = "header";
  header.stmts = {{"t", Op::Lt, {Operand::var("i"), Operand::var("n")}}};
  header.term.kind = TermKind::Br;
  header.term.cond = Operand::var("t");
  header.term.ifTrue = 2;
  header.term.ifFalse = 3;

  Block &body = f.blocks[2];
  body.name = "body";
  body.stmts = {{"s", Op::Add, {Operand::var("s"), Operand::var("i")}},
                {"i", Op::Add, {Operand::var("i"), Operand::imm(1)}}};
  body.term.kind = TermKind::Jmp;
  body.term.target = 1;

  Block &exit = f.blocks[3];
  exit.name = "exit";
  exit.term.kind = TermKind::Ret;
  exit.term.retVal = Operand::var("s");

  computePreds(f);
  return f;
}

// Test program 2: an if/else diamond — the other phi-producing shape.
//
//   entry: a = 1; c = a < b;  br c, then, else
//   then:  a = a + 10;        jmp join
//   else:  a = a + 20;        jmp join
//   join:  r = a * 2;         ret r
Function makeDiamondFunction() {
  Function f;
  f.name = "diamond";
  f.params = {"b"};
  f.blocks.resize(4);

  Block &entry = f.blocks[0];
  entry.name = "entry";
  entry.stmts = {{"a", Op::Const, {Operand::imm(1)}},
                 {"c", Op::Lt, {Operand::var("a"), Operand::var("b")}}};
  entry.term.kind = TermKind::Br;
  entry.term.cond = Operand::var("c");
  entry.term.ifTrue = 1;
  entry.term.ifFalse = 2;

  Block &thenBB = f.blocks[1];
  thenBB.name = "then";
  thenBB.stmts = {{"a", Op::Add, {Operand::var("a"), Operand::imm(10)}}};
  thenBB.term.kind = TermKind::Jmp;
  thenBB.term.target = 3;

  Block &elseBB = f.blocks[2];
  elseBB.name = "else";
  elseBB.stmts = {{"a", Op::Add, {Operand::var("a"), Operand::imm(20)}}};
  elseBB.term.kind = TermKind::Jmp;
  elseBB.term.target = 3;

  Block &join = f.blocks[3];
  join.name = "join";
  join.stmts = {{"r", Op::Mul, {Operand::var("a"), Operand::imm(2)}}};
  join.term.kind = TermKind::Ret;
  join.term.retVal = Operand::var("r");

  computePreds(f);
  return f;
}

long long runOriginal(const Function &f, long long paramValue) {
  return interpret(f, Env{{f.params[0], paramValue}});
}
long long runSSA(const Function &f, long long paramValue) {
  return interpret(f, Env{{f.paramNames[0].second, paramValue}});
}

std::set<std::string> phiVarsIn(const Block &b) {
  std::set<std::string> vars;
  for (const Phi &p : b.phis)
    vars.insert(p.var);
  return vars;
}

void testLoopSSA() {
  Function orig = makeLoopFunction();

  // Dominator sanity checks: header's idom is entry, body's idom is header.
  std::vector<int> idom = computeIDom(orig);
  CHECK(idom[1] == 0);
  CHECK(idom[2] == 1);
  CHECK(idom[3] == 1);

  auto df = computeDominanceFrontiers(orig, idom);
  CHECK(df[2] == std::set<int>{1}); // body's frontier is the loop header
  CHECK(df[1] == std::set<int>{1}); // header is in its own frontier (loop)

  Function form = buildSSA(orig);
  CHECK(isSingleAssignment(form));
  eliminateDeadPhis(form);
  CHECK(!hasUndefOperands(form));

  CHECK((phiVarsIn(form.blocks[1]) == std::set<std::string>{"i", "s"}));
  CHECK(form.blocks[0].phis.empty());
  CHECK(form.blocks[3].phis.empty());
  for (const Phi &p : form.blocks[1].phis)
    CHECK(p.args.size() == 2); // one per predecessor: entry and body

  for (long long n : {0LL, 1LL, 2LL, 5LL, 10LL, 37LL})
    CHECK_MSG(runOriginal(orig, n) == runSSA(form, n),
              "n=" + std::to_string(n));
  CHECK(runSSA(form, 5) == 10); // 0+1+2+3+4

  std::printf("%s", printFunction(form).c_str());
}

void testDiamondSSA() {
  Function orig = makeDiamondFunction();
  Function form = buildSSA(orig);
  CHECK(isSingleAssignment(form));
  eliminateDeadPhis(form);
  CHECK(!hasUndefOperands(form));

  CHECK((phiVarsIn(form.blocks[3]) == std::set<std::string>{"a"}));
  CHECK(form.blocks[3].phis.size() == 1);
  CHECK(form.blocks[3].phis[0].args.size() == 2);
  CHECK(form.blocks[1].phis.empty());

  for (long long b : {-5LL, 0LL, 1LL, 2LL, 100LL})
    CHECK_MSG(runOriginal(orig, b) == runSSA(form, b),
              "b=" + std::to_string(b));
  CHECK(runSSA(form, 2) == 22);  // 1 < 2 -> then: (1+10)*2
  CHECK(runSSA(form, 0) == 42);  // 1 >= 0 -> else: (1+20)*2

  std::printf("%s", printFunction(form).c_str());
}

} // namespace ssa

int main() {
  ssa::testLoopSSA();
  ssa::testDiamondSSA();
  return ts::report("05_ssa_construction");
}