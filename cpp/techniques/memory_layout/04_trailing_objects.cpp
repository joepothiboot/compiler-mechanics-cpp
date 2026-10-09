// 04_trailing_objects.cpp — variable-length objects (llvm::TrailingObjects).
//
// A call instruction has a different operand count every time. The obvious
// layout, a std::vector<Value*> member, costs a second heap allocation per
// instruction and a pointer chase on every operand access. LLVM instead
// allocates the object and its operands in ONE block, with the operands
// placed directly after the object ("hung off" or "trailing" storage):
//
//     [ CallInst fields | Value* op0 | Value* op1 | ... ]
//       ^ this            ^ reinterpret_cast<Value**>(this + 1)
//
// The rules that make this work:
//   * Construction goes through a static create() that sizes the allocation;
//     the constructor is private so nobody can make one on the stack.
//   * Destruction goes through destroy(), which must free the same block.
//   * Trailing storage must be suitably aligned after sizeof(CallInst).

#include "alloc_counter.h"
#include "test_support.h"

#include <cstddef>
#include <initializer_list>
#include <new>
#include <string>
#include <vector>

namespace trailing {

struct Value { int id; };

class CallInst {
public:
  static CallInst *create(const char *callee,
                          std::initializer_list<Value *> args) {
    static_assert(sizeof(CallInst) % alignof(Value *) == 0,
                  "trailing operands would be misaligned");
    void *mem = ::operator new(sizeof(CallInst) + args.size() * sizeof(Value *));
    auto *call = new (mem) CallInst(callee, static_cast<unsigned>(args.size()));
    Value **ops = call->operands();
    for (Value *a : args)
      *ops++ = a;
    return call;
  }

  static void destroy(CallInst *c) {
    c->~CallInst();
    ::operator delete(c);
  }

  unsigned numOperands() const { return numOps_; }
  Value *getOperand(unsigned i) const { return op_begin()[i]; }
  const char *callee() const { return callee_; }
  Value *const *op_begin() const {
    return reinterpret_cast<Value *const *>(this + 1);
  }

private:
  CallInst(const char *callee, unsigned n) : callee_(callee), numOps_(n) {}
  ~CallInst() = default;

  Value **operands() { return reinterpret_cast<Value **>(this + 1); }

  const char *callee_;
  unsigned numOps_;
};

// The conventional layout, for comparison.
struct VectorCallInst {
  const char *callee;
  std::vector<Value *> operands;
};

void testTrailingOperands() {
  Value a{1}, b{2}, c{3};

  std::size_t before = g_allocations;
  CallInst *call = CallInst::create("printf", {&a, &b, &c});
  CHECK(g_allocations == before + 1); // Object and operands: one allocation.

  CHECK(call->numOperands() == 3);
  CHECK(call->getOperand(0)->id == 1);
  CHECK(call->getOperand(2)->id == 3);

  // Operands start exactly where the object ends.
  CHECK(reinterpret_cast<const char *>(call->op_begin()) ==
        reinterpret_cast<const char *>(call) + sizeof(CallInst));
  CallInst::destroy(call);

  CallInst *noArgs = CallInst::create("abort", {});
  CHECK(noArgs->numOperands() == 0);
  CallInst::destroy(noArgs);

  before = g_allocations;
  auto *vcall = new VectorCallInst{"printf", {&a, &b, &c}};
  CHECK(g_allocations == before + 2); // Object + vector buffer.
  delete vcall;
}

} // namespace trailing

int main() {
  trailing::testTrailingOperands();
  return ts::report("memory_layout/04_trailing_objects");
}
