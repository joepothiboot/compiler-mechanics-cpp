// 02_function_ref.cpp — non-owning type-erased callables (llvm::function_ref).
//
// Compilers are full of "walk this, call me back" APIs: walk every operation
// in a region, visit every use of a value, run a predicate over every block.
// The callback is used during the call and never stored. std::function is the
// wrong tool for that: it OWNS a copy of the callable, so a lambda with a big
// capture gets heap-allocated on every call to the walker.
//
// function_ref<R(Args...)> is two words: a void* to the caller's callable and
// a pointer to a trampoline that knows its real type. Nothing is copied,
// nothing is allocated. Same rule as ArrayRef: it must not outlive the
// callable it refers to, so never store one in a member.
// (C++26 standardizes this as std::function_ref.)

#include "alloc_counter.h"
#include "test_support.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace callables {

template <typename Fn> class function_ref;

template <typename R, typename... Args> class function_ref<R(Args...)> {
public:
  template <typename Callable,
            // Don't hijack the copy constructor.
            typename = std::enable_if_t<
                !std::is_same<std::decay_t<Callable>, function_ref>::value>>
  function_ref(Callable &&c)
      : obj_(const_cast<void *>(
            static_cast<const void *>(std::addressof(c)))),
        call_(&trampoline<std::remove_reference_t<Callable>>) {}

  R operator()(Args... args) const {
    return call_(obj_, std::forward<Args>(args)...);
  }

private:
  template <typename Callable> static R trampoline(void *obj, Args... args) {
    return (*static_cast<Callable *>(obj))(std::forward<Args>(args)...);
  }

  void *obj_;
  R (*call_)(void *, Args...);
};

struct Op {
  int opcode;
  std::vector<Op> body; // Nested region, like an MLIR op with a region.
};

// The MLIR Operation::walk shape: pre-order, callback per op.
void walk(const Op &op, function_ref<void(const Op &)> fn) {
  fn(op);
  for (const Op &child : op.body)
    walk(child, fn);
}

void walkOwning(const Op &op, const std::function<void(const Op &)> &fn) {
  fn(op);
  for (const Op &child : op.body)
    walkOwning(child, fn);
}

void testFunctionRef() {
  Op module{0, {Op{1, {}}, Op{2, {Op{1, {}}, Op{3, {}}}}}};

  // A lambda carrying its own 128-byte histogram by value: too big for any
  // std::function small buffer.
  int visited = 0;
  long adds = 0;
  struct Histogram { long counts[16] = {}; };
  auto count = [&visited, &adds, h = Histogram{}](const Op &o) mutable {
    ++h.counts[o.opcode];
    ++visited;
    adds = h.counts[1];
  };

  std::size_t before = g_allocations;
  walk(module, count);
  CHECK(g_allocations == before); // Zero allocations for the walk itself.
  CHECK(visited == 5);
  CHECK(adds == 2); // The histogram updated in place: nothing was copied.

  before = g_allocations;
  walkOwning(module, count); // Copies the 128-byte lambda onto the heap.
  CHECK(g_allocations > before);

  CHECK(sizeof(function_ref<void(const Op &)>) == 2 * sizeof(void *));

  // Return values and argument forwarding work like any callable.
  // Note the named lambda: binding function_ref to a temporary lambda would
  // dangle at the end of the statement, exactly like ArrayRef = {1, 2, 3}.
  auto plus = [](int a, int b) { return a + b; };
  function_ref<int(int, int)> add = plus;
  CHECK(add(2, 3) == 5);
}

} // namespace callables

int main() {
  callables::testFunctionRef();
  return ts::report("views_and_callables/02_function_ref");
}
