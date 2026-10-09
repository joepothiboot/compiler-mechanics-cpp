// 04_ir_data_structures.cpp — intrusive lists and tagged unions.
//
// Part 1: llvm::ilist / iplist, the container behind BasicBlock::getInstList()
// and Function::getBasicBlockList().
// Part 2: std::variant as an AST/IR node, the "closed hierarchy" alternative to
// inheritance.

#include "test_support.h"

#include <cstddef>
#include <cstdlib>
#include <list>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// Global allocation counter. Compiler engineers profile allocations constantly:
// a pass that allocates per instruction will dominate compile time on large
// modules. This makes the intrusive-vs-std::list difference measurable rather
// than asserted.
// ---------------------------------------------------------------------------
static std::size_t g_allocations = 0;

void *operator new(std::size_t n) {
  ++g_allocations;
  if (void *p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t n) {
  ++g_allocations;
  if (void *p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace intrusive {

// ---------------------------------------------------------------------------
// The hook lives *inside* the element. That single fact buys everything LLVM
// wants:
//   * Inserting/removing costs zero allocations — the links already exist.
//   * Given only an `Instruction *`, you can unlink it: I->eraseFromParent()
//     and I->moveBefore(J) need no iterator and no side table. With
//     std::list<Instruction*> you would need a separate map from pointer to
//     iterator, costing memory and a hash lookup per edit.
//   * Pointers and iterators to other elements stay valid across any edit,
//     which is what makes it safe to hold worklists of Instruction* while
//     rewriting the function.
//   * An element can be spliced between two lists (moving an instruction to
//     another basic block) in O(1) with no reallocation and no re-hashing.
// The cost: an element can be in only one list per hook, and the container is
// not a value type — it never copies its elements.
// ---------------------------------------------------------------------------
struct ListHook {
  ListHook *prev = nullptr;
  ListHook *next = nullptr;
};

inline void hookInsertBefore(ListHook *pos, ListHook *node) {
  node->prev = pos->prev;
  node->next = pos;
  pos->prev->next = node;
  pos->prev = node;
}

inline void hookUnlink(ListHook *node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
  node->prev = node->next = nullptr;
}

template <typename T>
class IntrusiveList {
  static_assert(std::is_base_of<ListHook, T>::value,
                "T must derive from ListHook");

public:
  IntrusiveList() {
    // Circular sentinel: end() is &sentinel_, so insert/erase need no
    // null checks and no special-casing of the first/last element. LLVM's
    // ilist_sentinel does exactly this.
    sentinel_.next = &sentinel_;
    sentinel_.prev = &sentinel_;
  }
  IntrusiveList(const IntrusiveList &) = delete;
  IntrusiveList &operator=(const IntrusiveList &) = delete;

  class iterator {
  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T *;
    using reference = T &;

    explicit iterator(ListHook *n = nullptr) : node_(n) {}
    // Only legal when node_ != sentinel; the sentinel is a bare ListHook and is
    // never a real T.
    T &operator*() const { return *static_cast<T *>(node_); }
    T *operator->() const { return static_cast<T *>(node_); }
    iterator &operator++() { node_ = node_->next; return *this; }
    iterator operator++(int) { iterator t = *this; ++*this; return t; }
    iterator &operator--() { node_ = node_->prev; return *this; }
    bool operator==(const iterator &o) const { return node_ == o.node_; }
    bool operator!=(const iterator &o) const { return node_ != o.node_; }
    ListHook *hook() const { return node_; }

  private:
    ListHook *node_;
  };

  iterator begin() { return iterator(sentinel_.next); }
  iterator end() { return iterator(&sentinel_); }
  bool empty() const { return sentinel_.next == &sentinel_; }

  void push_back(T *elem) { hookInsertBefore(&sentinel_, elem); }
  void insertBefore(iterator pos, T *elem) { hookInsertBefore(pos.hook(), elem); }
  static void remove(T *elem) { hookUnlink(elem); } // No container needed!

  std::size_t size() const {
    std::size_t n = 0;
    for (const ListHook *h = sentinel_.next; h != &sentinel_; h = h->next)
      ++n;
    return n;
  }

private:
  ListHook sentinel_;
};

// ---------------------------------------------------------------------------
// Instruction/BasicBlock modeled on LLVM: the instruction knows its parent, so
// it can remove itself from the IR. This self-service API is only possible with
// an intrusive list.
// ---------------------------------------------------------------------------
class BasicBlock;

class Inst : public ListHook {
public:
  explicit Inst(std::string op) : opcode_(std::move(op)) {}
  Inst(const Inst &) = delete;
  Inst &operator=(const Inst &) = delete;

  const std::string &opcode() const { return opcode_; }
  BasicBlock *parent() const { return parent_; }

  void eraseFromParent(); // unlink + destroy
  void moveBefore(Inst *other); // O(1) splice, possibly across blocks

private:
  friend class BasicBlock;
  std::string opcode_;
  BasicBlock *parent_ = nullptr;
};

class BasicBlock {
public:
  explicit BasicBlock(std::string name) : name_(std::move(name)) {}
  ~BasicBlock() {
    // The list is non-owning; the block owns whatever is currently linked into
    // it. Advance the iterator before deleting, since delete destroys the hook.
    for (auto it = insts_.begin(); it != insts_.end();) {
      Inst *dead = &*it;
      ++it;
      delete dead;
    }
  }
  BasicBlock(const BasicBlock &) = delete;
  BasicBlock &operator=(const BasicBlock &) = delete;

  Inst *append(std::string opcode) {
    Inst *inst = new Inst(std::move(opcode)); // 1 allocation, ever, per inst
    inst->parent_ = this;
    insts_.push_back(inst);
    return inst;
  }

  IntrusiveList<Inst> &instructions() { return insts_; }
  const std::string &name() const { return name_; }

  std::vector<std::string> opcodes() {
    std::vector<std::string> out;
    for (Inst &i : insts_)
      out.push_back(i.opcode());
    return out;
  }

private:
  friend class Inst;
  std::string name_;
  IntrusiveList<Inst> insts_;
};

void Inst::eraseFromParent() {
  IntrusiveList<Inst>::remove(this);
  delete this; // Safe: nothing else in the list references us anymore.
}

void Inst::moveBefore(Inst *other) {
  hookUnlink(this);
  hookInsertBefore(other, this);
  parent_ = other->parent_; // Ownership follows list membership.
}

void testIntrusiveList() {
  BasicBlock entry("entry"), loop("loop");
  Inst *i0 = entry.append("alloca");
  Inst *i1 = entry.append("load");
  Inst *i2 = entry.append("add");
  Inst *i3 = entry.append("store");
  (void)i0;

  Inst *l0 = loop.append("phi");
  CHECK((entry.opcodes() == std::vector<std::string>{"alloca", "load", "add",
                                                     "store"}));

  // Editing with only a raw pointer in hand, and zero allocations.
  std::size_t before = g_allocations;
  i2->moveBefore(l0); // hoist "add" into the loop block
  CHECK(g_allocations == before);
  CHECK(i2->parent() == &loop);
  CHECK((entry.opcodes() == std::vector<std::string>{"alloca", "load", "store"}));
  CHECK((loop.opcodes() == std::vector<std::string>{"add", "phi"}));

  // Pointers to unrelated instructions survived the edit.
  CHECK(i1->opcode() == "load");
  CHECK(i3->opcode() == "store");

  before = g_allocations;
  i1->eraseFromParent(); // self-removal: impossible with std::list alone
  CHECK(g_allocations == before);
  CHECK((entry.opcodes() == std::vector<std::string>{"alloca", "store"}));

  // Safe erase-while-iterating, the standard LLVM pattern.
  auto &list = loop.instructions();
  for (auto it = list.begin(); it != list.end();) {
    Inst *cur = &*it;
    ++it; // advance first
    if (cur->opcode() == "add")
      cur->eraseFromParent();
  }
  CHECK((loop.opcodes() == std::vector<std::string>{"phi"}));
}

// ---------------------------------------------------------------------------
// std::list for comparison. To store polymorphic/identity IR nodes you must
// store pointers, so each element costs a *second* allocation for the list
// node; and to erase by pointer you need a side table of iterators, which is a
// third allocation source plus a hash lookup per edit.
// ---------------------------------------------------------------------------
void testStdListComparison() {
  std::vector<std::unique_ptr<Inst>> storage;
  std::list<Inst *> insts;
  std::unordered_map<Inst *, std::list<Inst *>::iterator> positions;

  std::size_t before = g_allocations;
  for (int i = 0; i < 4; ++i) {
    storage.push_back(std::make_unique<Inst>("op" + std::to_string(i)));
    insts.push_back(storage.back().get());
    positions[storage.back().get()] = std::prev(insts.end());
  }
  std::size_t stdListCost = g_allocations - before;

  BasicBlock bb("cmp");
  before = g_allocations;
  for (int i = 0; i < 4; ++i)
    bb.append("op" + std::to_string(i));
  std::size_t intrusiveCost = g_allocations - before;

  // Same number of instructions, strictly fewer allocations intrusively.
  CHECK(intrusiveCost < stdListCost);
  std::printf("       allocations for 4 instructions: intrusive=%zu, "
              "std::list<Inst*>+index=%zu\n",
              intrusiveCost, stdListCost);

  // Erase by pointer requires the side table; note the extra indirection.
  Inst *target = storage[2].get();
  insts.erase(positions[target]);
  positions.erase(target);
  CHECK(insts.size() == 3);
}

} // namespace intrusive

namespace tagged {

// ---------------------------------------------------------------------------
// std::variant as an AST node.
//
// Two node-representation styles show up in real compilers:
//   * Inheritance + kind tag (LLVM IR, Clang AST): open to new node types,
//     nodes are heap objects with stable addresses, dispatch via dyn_cast.
//   * Closed tagged union (many Rust/ML-style frontends, and C++ frontends
//     using std::variant): the alternative set is fixed, so std::visit gives
//     *compile-time exhaustiveness* — add a new alternative and every visitor
//     that lacks a handler fails to compile. That guarantee is worth a lot in a
//     frontend AST where forgetting a case is a silent miscompile.
// Costs of variant: every node is as large as the largest alternative, and the
// node set cannot be extended by a downstream dialect (which is exactly why
// MLIR uses open registration, not variant).
// ---------------------------------------------------------------------------

struct Expr;
using ExprPtr = std::unique_ptr<Expr>; // Indirection: a variant cannot contain
                                       // an incomplete/recursive copy of itself.

struct NumLit {
  double value;
};
struct VarRef {
  std::string name;
};
struct BinOp {
  char op; // '+', '-', '*'
  ExprPtr lhs, rhs;
};
struct LetExpr {
  std::string name;
  ExprPtr value, body;
};

struct Expr {
  std::variant<NumLit, VarRef, BinOp, LetExpr> node;
};

// C++17 overload-set trick: inherit operator() from a pack of lambdas. The
// deduction guide is required in C++17 (C++20 makes it implicit) — worth
// remembering, since LLVM code targets C++17.
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

ExprPtr num(double v) { return ExprPtr(new Expr{NumLit{v}}); }
ExprPtr var(std::string n) { return ExprPtr(new Expr{VarRef{std::move(n)}}); }
ExprPtr bin(char op, ExprPtr l, ExprPtr r) {
  return ExprPtr(new Expr{BinOp{op, std::move(l), std::move(r)}});
}
ExprPtr let(std::string n, ExprPtr v, ExprPtr b) {
  return ExprPtr(new Expr{LetExpr{std::move(n), std::move(v), std::move(b)}});
}

using Env = std::unordered_map<std::string, double>;

double applyOp(char op, double a, double b) {
  switch (op) {
  case '+': return a + b;
  case '-': return a - b;
  case '*': return a * b;
  default: return 0.0;
  }
}

// Interpreter over the variant. If you add a fifth alternative to Expr, this
// std::visit stops compiling until you handle it.
double eval(const Expr &e, Env &env) {
  return std::visit(
      overloaded{
          [](const NumLit &n) { return n.value; },
          [&](const VarRef &v) { return env.at(v.name); },
          [&](const BinOp &b) {
            return applyOp(b.op, eval(*b.lhs, env), eval(*b.rhs, env));
          },
          [&](const LetExpr &l) {
            double saved = 0;
            bool had = env.count(l.name) != 0;
            if (had)
              saved = env[l.name];
            env[l.name] = eval(*l.value, env);
            double result = eval(*l.body, env);
            // Restore the shadowed binding: lexical scoping, by hand.
            if (had)
              env[l.name] = saved;
            else
              env.erase(l.name);
            return result;
          }},
      e.node);
}

// Constant folding as a tree rewrite. Returns a fresh tree; real compilers
// mutate in place, but a pure version is much easier to reason about and test.
ExprPtr constantFold(const Expr &e) {
  return std::visit(
      overloaded{
          [](const NumLit &n) { return num(n.value); },
          [](const VarRef &v) { return var(v.name); },
          [](const BinOp &b) {
            ExprPtr l = constantFold(*b.lhs);
            ExprPtr r = constantFold(*b.rhs);
            // holds_alternative is the variant equivalent of isa<>.
            if (std::holds_alternative<NumLit>(l->node) &&
                std::holds_alternative<NumLit>(r->node))
              return num(applyOp(b.op, std::get<NumLit>(l->node).value,
                                 std::get<NumLit>(r->node).value));
            return bin(b.op, std::move(l), std::move(r));
          },
          [](const LetExpr &l) {
            return let(l.name, constantFold(*l.value), constantFold(*l.body));
          }},
      e.node);
}

std::string print(const Expr &e) {
  return std::visit(
      overloaded{
          [](const NumLit &n) {
            std::string s = std::to_string(n.value);
            s.erase(s.find_last_not_of('0') + 1);
            if (!s.empty() && s.back() == '.')
              s.pop_back();
            return s;
          },
          [](const VarRef &v) { return v.name; },
          [](const BinOp &b) {
            return "(" + print(*b.lhs) + std::string(1, b.op) + print(*b.rhs) +
                   ")";
          },
          [](const LetExpr &l) {
            return "let " + l.name + " = " + print(*l.value) + " in " +
                   print(*l.body);
          }},
      e.node);
}

void testVariantAST() {
  // let x = (2 * 3) + 4 in (x * y)
  ExprPtr ast = let("x", bin('+', bin('*', num(2), num(3)), num(4)),
                    bin('*', var("x"), var("y")));

  Env env{{"y", 5.0}};
  CHECK(eval(*ast, env) == 50.0);
  CHECK(env.count("x") == 0); // scope restored after the let

  ExprPtr folded = constantFold(*ast);
  CHECK(print(folded->node.index() == 3 ? *folded : *folded) ==
        "let x = 10 in (x*y)");
  CHECK(eval(*folded, env) == 50.0); // folding preserved semantics

  // The value part of the let is now a literal, not a BinOp.
  const auto &l = std::get<LetExpr>(folded->node);
  CHECK(std::holds_alternative<NumLit>(l.value->node));
  CHECK(std::get<NumLit>(l.value->node).value == 10.0);

  CHECK(ast->node.index() == 3); // LetExpr is alternative #3
  std::printf("       folded AST: %s\n", print(*folded).c_str());
}

} // namespace tagged

int main() {
  intrusive::testIntrusiveList();
  intrusive::testStdListComparison();
  tagged::testVariantAST();
  return ts::report("04_ir_data_structures");
}