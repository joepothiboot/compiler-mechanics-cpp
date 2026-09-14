// 02_polymorphism.cpp — virtual dispatch, its limits, and CRTP.
//
// LLVM makes a deliberate, unusual choice: most of its hot-path polymorphism is
// *static* (CRTP + templates), and virtual dispatch is reserved for genuine
// plugin boundaries (Pass, TargetMachine, MLIR interfaces). This file shows the
// mechanics behind that choice.

#include "test_support.h"

#include <string>
#include <vector>

namespace vtables {

// ---------------------------------------------------------------------------
// Classic virtual hierarchy. Useful where the set of implementations is open
// (backends, passes loaded from plugins) — the cost is one indirect call that
// the optimizer usually cannot devirtualize across a translation unit.
// ---------------------------------------------------------------------------
struct TargetOp {
  virtual ~TargetOp() = default;              // Required: deleting via base
                                              // without it is UB.
  virtual const char *mnemonic() const = 0;
  virtual int latency() const { return 1; }   // Non-pure virtual = default impl.
};

struct AddOp : TargetOp {
  const char *mnemonic() const override { return "add"; }
};

struct MulOp : TargetOp {
  const char *mnemonic() const override { return "mul"; }
  int latency() const override { return 3; }
};

struct DivOp : TargetOp {
  const char *mnemonic() const override { return "div"; }
  int latency() const override { return 20; }
};

int totalLatency(const std::vector<TargetOp *> &ops) {
  int sum = 0;
  for (const TargetOp *op : ops)
    sum += op->latency(); // Indirect call through the vptr.
  return sum;
}

// ---------------------------------------------------------------------------
// Virtual call during construction resolves to the *base* override.
//
// Matters in compilers because IR nodes often want to self-register with a
// parent block or symbol table in their constructor; if that registration is
// virtual, you silently get the base behaviour. LLVM sidesteps this with
// factory functions (Foo::Create(...)) rather than doing work in constructors.
// ---------------------------------------------------------------------------
struct Base {
  std::string tagSeenInCtor;
  Base() { tagSeenInCtor = tag(); } // Derived part isn't constructed yet, so the
                                    // vptr still points at Base's vtable.
  virtual ~Base() = default;
  virtual std::string tag() const { return "Base"; }
};

struct Derived : Base {
  std::string tag() const override { return "Derived"; }
};

// ---------------------------------------------------------------------------
// WHY A MEMBER TEMPLATE CANNOT BE VIRTUAL.
//
//   struct Visitor {
//     template <typename NodeT> virtual void visit(NodeT &node); // ill-formed
//   };
//
// A vtable is a fixed-size array of function pointers, laid out when the class
// is compiled; every override must land in a slot the compiler can assign
// statically. A member template has a potentially unbounded set of
// instantiations, and a *new* one can be created in a different translation
// unit compiled later (or by a plugin loaded at runtime). There is no way to
// assign stable slots, so the language forbids it outright.
//
// Consequence for IR traversal: you cannot write one generic virtual
// `visit<T>()` that adapts to new node types. Compilers therefore pick one of:
//
//   (A) Double dispatch with a fixed virtual overload set — extensible in
//       *node types* only by editing the visitor interface (the expression
//       problem). Used where the node set is closed and stable.
//   (B) CRTP static visitors (llvm::InstVisitor, clang::RecursiveASTVisitor) —
//       the "virtual" method set is resolved at compile time via templates, so
//       there is no vtable at all and calls inline.
//   (C) Runtime kind tag + switch/dyn_cast (mlir::TypeSwitch) — see file 03.
// ---------------------------------------------------------------------------

// (A) Double dispatch.
struct AddNode;
struct MulNode;

struct NodeVisitor {
  virtual ~NodeVisitor() = default;
  virtual void visitAdd(AddNode &) = 0;
  virtual void visitMul(MulNode &) = 0; // Adding a node type breaks every
                                        // visitor: that's the tradeoff.
};

struct Node {
  virtual ~Node() = default;
  virtual void accept(NodeVisitor &v) = 0; // First dispatch: on the node.
};

struct AddNode : Node {
  void accept(NodeVisitor &v) override { v.visitAdd(*this); } // Second: on the
                                                              // visitor, now
                                                              // with the exact
                                                              // static type.
};

struct MulNode : Node {
  void accept(NodeVisitor &v) override { v.visitMul(*this); }
};

struct MnemonicCollector : NodeVisitor {
  std::string out;
  void visitAdd(AddNode &) override { out += "add;"; }
  void visitMul(MulNode &) override { out += "mul;"; }
};

void testVirtualDispatch() {
  AddOp a;
  MulOp m;
  DivOp d;
  std::vector<TargetOp *> ops{&a, &m, &d};
  CHECK(totalLatency(ops) == 24);
  CHECK(std::string(ops[2]->mnemonic()) == "div");

  // A polymorphic object carries a vptr, so it is never smaller than a pointer.
  CHECK(sizeof(AddOp) >= sizeof(void *));

  Derived der;
  CHECK(der.tagSeenInCtor == "Base"); // The constructor trap.
  CHECK(der.tag() == "Derived");      // Fully constructed: normal dispatch.

  AddNode an;
  MulNode mn;
  MnemonicCollector collector;
  an.accept(collector);
  mn.accept(collector);
  CHECK(collector.out == "add;mul;");
}

} // namespace vtables

namespace crtp {

// ---------------------------------------------------------------------------
// CRTP: static polymorphism, the LLVM workhorse.
//
// The base class is templated on the derived class, so it can static_cast
// `this` to Derived and call the derived method directly. Benefits in a
// compiler: zero vtable, calls inline through the whole traversal (an IR walk
// happens millions of times per compile), and a derived visitor only overrides
// the handful of node kinds it cares about while inheriting a default
// traversal. Costs: no heterogeneous container of visitors, and every
// instantiation duplicates code (binary size — a real LLVM concern).
//
// Layout mirrors llvm::InstVisitor's delegation chain:
//   visitAdd -> visitBinary -> visitDefault
// so a subclass can intercept at whatever granularity it wants.
// ---------------------------------------------------------------------------

enum class ExprKind { Const, Add, Mul };

struct Expr {
  explicit Expr(ExprKind k) : kind(k) {}
  ExprKind kind; // No virtuals here at all: dispatch is by tag + static_cast.
};

struct ConstExpr : Expr {
  explicit ConstExpr(long long v) : Expr(ExprKind::Const), value(v) {}
  long long value;
};

struct BinaryExpr : Expr {
  BinaryExpr(ExprKind k, const Expr *l, const Expr *r)
      : Expr(k), lhs(l), rhs(r) {}
  const Expr *lhs;
  const Expr *rhs;
};

template <typename Derived, typename ResultT = int>
class ExprVisitor {
public:
  ResultT visit(const Expr &e) {
    switch (e.kind) {
    case ExprKind::Const:
      return self().visitConst(static_cast<const ConstExpr &>(e));
    case ExprKind::Add:
      return self().visitAdd(static_cast<const BinaryExpr &>(e));
    case ExprKind::Mul:
      return self().visitMul(static_cast<const BinaryExpr &>(e));
    }
    return ResultT{};
  }

  // Defaults delegate "upward" so subclasses override at any level. These
  // bodies are only instantiated when called, which is why Derived may still be
  // incomplete at the point of inheritance.
  ResultT visitAdd(const BinaryExpr &e) { return self().visitBinary(e); }
  ResultT visitMul(const BinaryExpr &e) { return self().visitBinary(e); }
  ResultT visitBinary(const BinaryExpr &e) {
    return self().visit(*e.lhs) + self().visit(*e.rhs);
  }
  ResultT visitConst(const ConstExpr &) { return ResultT{}; }

protected:
  Derived &self() { return *static_cast<Derived *>(this); } // The CRTP move.
};

// Only overrides the two binary cases; inherits traversal and the const case.
struct CostVisitor : ExprVisitor<CostVisitor> {
  int visitAdd(const BinaryExpr &e) { return 1 + visitBinary(e); }
  int visitMul(const BinaryExpr &e) { return 3 + visitBinary(e); }
};

// Only overrides the leaf case; inherits the entire traversal.
struct LeafCounter : ExprVisitor<LeafCounter> {
  int visitConst(const ConstExpr &) { return 1; }
};

// ---------------------------------------------------------------------------
// Second CRTP flavour: interface injection (cf. llvm::PassInfoMixin). The base
// supplies boilerplate implemented in terms of a small derived-provided hook.
// ---------------------------------------------------------------------------
template <typename Derived>
class FunctionPassMixin {
public:
  // Returns true if anything changed — LLVM's pass contract, used to decide
  // whether analyses must be invalidated.
  bool runOnFunction(std::vector<int> &instructions) {
    bool changed = false;
    for (int &inst : instructions)
      changed |= static_cast<Derived *>(this)->runOnInstruction(inst);
    return changed;
  }
};

struct StrengthReduction : FunctionPassMixin<StrengthReduction> {
  bool runOnInstruction(int &opcodeCost) {
    if (opcodeCost == 3) { // pretend: multiply-by-2 -> shift
      opcodeCost = 1;
      return true;
    }
    return false;
  }
};

void testCRTP() {
  ConstExpr two(2), three(3), four(4);
  BinaryExpr mul(ExprKind::Mul, &two, &three);
  BinaryExpr add(ExprKind::Add, &mul, &four); // (2*3) + 4

  CostVisitor cost;
  CHECK(cost.visit(add) == 4); // add(1) + mul(3), constants free

  LeafCounter leaves;
  CHECK(leaves.visit(add) == 3);

  // No vtable pointer anywhere in the visitors.
  CHECK(sizeof(CostVisitor) == 1);

  std::vector<int> fn{3, 1, 3, 5};
  StrengthReduction pass;
  CHECK(pass.runOnFunction(fn));
  CHECK((fn == std::vector<int>{1, 1, 1, 5}));
  CHECK(!pass.runOnFunction(fn)); // Idempotent: second run changes nothing.
}

} // namespace crtp

int main() {
  vtables::testVirtualDispatch();
  crtp::testCRTP();
  return ts::report("02_polymorphism");
}