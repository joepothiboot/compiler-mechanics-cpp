// 03_rtti_isa_dyncast.cpp — LLVM's isa<>/cast<>/dyn_cast<> idiom.
//
// LLVM builds with -fno-rtti by default. Reasons, in the order they get cited:
//   1. RTTI emits type_info and inheritance metadata for every polymorphic
//      class, measurably growing the binary (LLVM has thousands of classes).
//   2. dynamic_cast is an opaque runtime library call that walks an inheritance
//      graph; it cannot be inlined or constant-folded. dyn_cast<> is a load of
//      an enum field plus one or two integer comparisons, and inlines away.
//   3. Downstream consumers (and many game/embedded builds) compile without
//      RTTI; a library that requires it can't be linked in.
//   4. The classof mechanism extends to things dynamic_cast cannot handle at
//      all, e.g. MLIR Types/Attributes, which are value-typed handles with no
//      C++ inheritance relationship to switch on.
//
// The price: every class in the hierarchy must participate in a hand-maintained
// kind enum. LLVM's convention of ordering the enum so that each abstract base
// owns a contiguous *range* makes classof a two-comparison range check.

#include "test_support.h"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

namespace llvmstyle {

// ---------------------------------------------------------------------------
// Kind enum with range markers. Order is load-bearing: derived kinds must be
// contiguous inside their base's [Begin, End] window, which is exactly how
// llvm/IR/Value.def and Instruction.def are structured.
// ---------------------------------------------------------------------------
enum ValueKind {
  VK_Argument,
  VK_ConstantInt,

  VK_Instruction_Begin,
  VK_BinaryOp_Begin = VK_Instruction_Begin,
  VK_Add = VK_BinaryOp_Begin,
  VK_Mul,
  VK_BinaryOp_End = VK_Mul,
  VK_Load,
  VK_Call,
  VK_Instruction_End = VK_Call,
};

class Value {
public:
  virtual ~Value() = default; // Kept only so we can delete through Value* and
                              // so the dynamic_cast comparison below compiles.
                              // LLVM's Value has a virtual dtor too — RTTI is
                              // disabled by a compiler flag, not by avoiding
                              // virtual functions.
  ValueKind getKind() const { return kind_; }
  const std::string &name() const { return name_; }

protected:
  Value(ValueKind k, std::string n) : kind_(k), name_(std::move(n)) {}

private:
  const ValueKind kind_; // Set once at construction; the whole mechanism rests
                         // on this never lying.
  std::string name_;
};

// ---------------------------------------------------------------------------
// The cast machinery. Real LLVM (llvm/Support/Casting.h) routes through
// simplify_type/CastInfo traits so the same syntax works for references,
// unique_ptr, and Optional. This is the load-bearing core.
// ---------------------------------------------------------------------------
template <typename To, typename From>
bool isa(const From *val) {
  assert(val && "isa<> used on a null pointer; use isa_and_nonnull<>");
  return To::classof(val); // Static call: no vtable, no runtime type graph.
}

template <typename To, typename From>
bool isa_and_nonnull(const From *val) {
  return val && To::classof(val);
}

// cast<>: you assert the type is right. In an assertions build a mistake traps
// immediately; in a release build it compiles to a free static_cast.
template <typename To, typename From>
To *cast(From *val) {
  assert(isa<To>(val) && "cast<Ty>() argument of incompatible type");
  return static_cast<To *>(val);
}

template <typename To, typename From>
const To *cast(const From *val) {
  assert(isa<To>(val) && "cast<Ty>() argument of incompatible type");
  return static_cast<const To *>(val);
}

// dyn_cast<>: test and convert. Idiomatic use is the C++17 if-init form:
//   if (auto *BO = dyn_cast<BinaryOp>(V)) { ... }
template <typename To, typename From>
To *dyn_cast(From *val) {
  return isa<To>(val) ? static_cast<To *>(val) : nullptr;
}

template <typename To, typename From>
const To *dyn_cast(const From *val) {
  return isa<To>(val) ? static_cast<const To *>(val) : nullptr;
}

template <typename To, typename From>
To *dyn_cast_or_null(From *val) {
  return (val && To::classof(val)) ? static_cast<To *>(val) : nullptr;
}

// ---------------------------------------------------------------------------
// Hierarchy. Note how each classof is either an equality test (leaf) or a
// range test (abstract base) — both are branch-predictor friendly and inline.
// ---------------------------------------------------------------------------
class ConstantInt : public Value {
public:
  explicit ConstantInt(long long v)
      : Value(VK_ConstantInt, "const"), value_(v) {}
  long long value() const { return value_; }
  static bool classof(const Value *v) { return v->getKind() == VK_ConstantInt; }

private:
  long long value_;
};

class Argument : public Value {
public:
  explicit Argument(std::string n) : Value(VK_Argument, std::move(n)) {}
  static bool classof(const Value *v) { return v->getKind() == VK_Argument; }
};

class Instruction : public Value {
public:
  static bool classof(const Value *v) {
    return v->getKind() >= VK_Instruction_Begin &&
           v->getKind() <= VK_Instruction_End; // The range trick.
  }

protected:
  Instruction(ValueKind k, std::string n) : Value(k, std::move(n)) {}
};

class BinaryOp : public Instruction {
public:
  Value *lhs() const { return lhs_; }
  Value *rhs() const { return rhs_; }
  static bool classof(const Value *v) {
    return v->getKind() >= VK_BinaryOp_Begin && v->getKind() <= VK_BinaryOp_End;
  }

protected:
  BinaryOp(ValueKind k, std::string n, Value *l, Value *r)
      : Instruction(k, std::move(n)), lhs_(l), rhs_(r) {}

private:
  Value *lhs_;
  Value *rhs_;
};

class AddInst : public BinaryOp {
public:
  AddInst(Value *l, Value *r) : BinaryOp(VK_Add, "add", l, r) {}
  static bool classof(const Value *v) { return v->getKind() == VK_Add; }
};

class MulInst : public BinaryOp {
public:
  MulInst(Value *l, Value *r) : BinaryOp(VK_Mul, "mul", l, r) {}
  static bool classof(const Value *v) { return v->getKind() == VK_Mul; }
};

class LoadInst : public Instruction {
public:
  explicit LoadInst(Value *addr) : Instruction(VK_Load, "load"), addr_(addr) {}
  Value *address() const { return addr_; }
  static bool classof(const Value *v) { return v->getKind() == VK_Load; }

private:
  Value *addr_;
};

class CallInst : public Instruction {
public:
  explicit CallInst(std::string callee)
      : Instruction(VK_Call, "call"), callee_(std::move(callee)) {}
  const std::string &callee() const { return callee_; }
  static bool classof(const Value *v) { return v->getKind() == VK_Call; }

private:
  std::string callee_;
};

// ---------------------------------------------------------------------------
// Realistic consumer: a peephole constant folder. This shape — dyn_cast in an
// if-init, bail out on failure — is what 80% of LLVM transform code looks like.
// Returns the folded constant value, or nullopt-ish via a bool out-param style.
// ---------------------------------------------------------------------------
bool tryConstantFold(const Value *v, long long &out) {
  const auto *bin = dyn_cast<BinaryOp>(v);
  if (!bin)
    return false;

  const auto *lhs = dyn_cast<ConstantInt>(bin->lhs());
  const auto *rhs = dyn_cast<ConstantInt>(bin->rhs());
  if (!lhs || !rhs)
    return false;

  // Leaf-level discrimination after the range-level check succeeded.
  if (isa<AddInst>(bin))
    out = lhs->value() + rhs->value();
  else if (isa<MulInst>(bin))
    out = lhs->value() * rhs->value();
  else
    return false;
  return true;
}

void testCasting() {
  ConstantInt c2(2), c3(3);
  Argument arg("%x");
  AddInst add(&c2, &c3);
  MulInst mul(&add, &c3);
  LoadInst load(&arg);
  CallInst call("printf");

  Value *v = &add;

  // Leaf checks.
  CHECK(isa<AddInst>(v));
  CHECK(!isa<MulInst>(v));

  // Range checks up the hierarchy — this is what dynamic_cast would cost you a
  // library call for.
  CHECK(isa<BinaryOp>(v));
  CHECK(isa<Instruction>(v));
  CHECK(isa<Instruction>(static_cast<Value *>(&call)));
  CHECK(!isa<BinaryOp>(static_cast<Value *>(&load)));
  CHECK(!isa<Instruction>(static_cast<Value *>(&arg)));

  CHECK(dyn_cast<MulInst>(v) == nullptr);
  CHECK(dyn_cast<AddInst>(v) == &add);
  CHECK(cast<BinaryOp>(v)->rhs() == &c3);

  Value *nothing = nullptr;
  CHECK(dyn_cast_or_null<AddInst>(nothing) == nullptr);
  CHECK(!isa_and_nonnull<AddInst>(nothing));

  long long folded = 0;
  CHECK(tryConstantFold(&add, folded) && folded == 5);
  folded = 0;
  CHECK(!tryConstantFold(&mul, folded)); // lhs is an instruction, not constant
  CHECK(!tryConstantFold(&load, folded));

  MulInst foldable(&c2, &c3);
  CHECK(tryConstantFold(&foldable, folded) && folded == 6);

  // Const-correctness: casting a const Value* yields const.
  const Value *cv = &add;
  const AddInst *ca = dyn_cast<AddInst>(cv);
  CHECK(ca != nullptr);
}

// ---------------------------------------------------------------------------
// dynamic_cast for contrast. Compiled out when RTTI is off, which is precisely
// the portability problem that motivates the isa<> family. Try building this
// file with -fno-rtti and watch this block vanish while everything above works.
// ---------------------------------------------------------------------------
#if defined(__cpp_rtti) || defined(_CPPRTTI)
#define STUDY_HAVE_RTTI 1
#else
#define STUDY_HAVE_RTTI 0
#endif

void testDynamicCastComparison() {
#if STUDY_HAVE_RTTI
  ConstantInt c2(2), c3(3);
  AddInst add(&c2, &c3);
  Value *v = &add;

  CHECK(dynamic_cast<BinaryOp *>(v) != nullptr);
  CHECK(dynamic_cast<MulInst *>(v) == nullptr);
  // Same answers as dyn_cast — same semantics, different cost model and no
  // requirement that the hierarchy be closed/enumerated.
  CHECK((dynamic_cast<BinaryOp *>(v) != nullptr) == isa<BinaryOp>(v));
  std::printf("       (RTTI enabled: dynamic_cast comparison ran)\n");
#else
  std::printf("       (built with -fno-rtti: dynamic_cast path skipped)\n");
#endif
}

} // namespace llvmstyle

int main() {
  llvmstyle::testCasting();
  llvmstyle::testDynamicCastComparison();
  return ts::report("03_rtti_isa_dyncast");
}