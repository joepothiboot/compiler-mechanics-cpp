// 01_bump_allocator.cpp — arena (bump-pointer) allocation + placement new.
//
// llvm::BumpPtrAllocator and MLIR's StorageUniquer hand out memory by bumping a
// pointer inside large slabs. Nothing is freed individually; the whole arena
// dies at once. Why compilers want this:
//   1. Allocation is an align-up plus an add: no free lists, no locking.
//   2. Nodes created together sit together in memory (cache locality when a
//      pass walks them in creation order, which is most passes).
//   3. Tearing down a module is O(slabs), not O(nodes).
// The price: destructors never run. Arena objects must be trivially
// destructible, or something must run destructors explicitly
// (llvm::SpecificBumpPtrAllocator<T> does, because it knows T).

#include "test_support.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace arena {

class BumpAllocator {
public:
  explicit BumpAllocator(std::size_t slabSize = 4096) : slabSize_(slabSize) {}
  BumpAllocator(const BumpAllocator &) = delete;
  BumpAllocator &operator=(const BumpAllocator &) = delete;
  ~BumpAllocator() {
    for (char *s : slabs_)
      std::free(s);
  }

  void *allocate(std::size_t size, std::size_t align) {
    assert((align & (align - 1)) == 0 && "alignment must be a power of two");
    std::uintptr_t p = alignUp(reinterpret_cast<std::uintptr_t>(cur_), align);
    if (cur_ && p + size <= reinterpret_cast<std::uintptr_t>(end_)) {
      cur_ = reinterpret_cast<char *>(p + size);
      return reinterpret_cast<void *>(p);
    }

    // Oversized request: give it a private slab and leave the current slab
    // alone, so one big object doesn't waste the rest of it. LLVM keeps these
    // in a separate CustomSizedSlabs list for the same reason.
    std::size_t need = size + align;
    if (need > slabSize_)
      return reinterpret_cast<void *>(
          alignUp(reinterpret_cast<std::uintptr_t>(newSlab(need)), align));

    cur_ = newSlab(slabSize_);
    end_ = cur_ + slabSize_;
    return allocate(size, align); // Guaranteed to fit now.
  }

  template <typename T, typename... Args> T *create(Args &&...args) {
    static_assert(std::is_trivially_destructible<T>::value,
                  "the arena never runs destructors");
    return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
  }

  std::size_t numSlabs() const { return slabs_.size(); }

private:
  static std::uintptr_t alignUp(std::uintptr_t v, std::size_t a) {
    return (v + a - 1) & ~static_cast<std::uintptr_t>(a - 1);
  }
  char *newSlab(std::size_t bytes) {
    char *mem = static_cast<char *>(std::malloc(bytes));
    if (!mem)
      throw std::bad_alloc();
    slabs_.push_back(mem);
    return mem;
  }

  std::size_t slabSize_;
  std::vector<char *> slabs_;
  char *cur_ = nullptr;
  char *end_ = nullptr;
};

// Trivially destructible on purpose: operands are raw, non-owning pointers
// into the same arena, so there's nothing for a destructor to do.
struct Node {
  char op;
  long value;
  const Node *lhs;
  const Node *rhs;
};

long eval(const Node *n) {
  switch (n->op) {
  case '+': return eval(n->lhs) + eval(n->rhs);
  case '*': return eval(n->lhs) * eval(n->rhs);
  default:  return n->value;
  }
}

void testBumpAllocator() {
  BumpAllocator a(1024);

  const Node *two = a.create<Node>(Node{'#', 2, nullptr, nullptr});
  const Node *three = a.create<Node>(Node{'#', 3, nullptr, nullptr});
  const Node *mul = a.create<Node>(Node{'*', 0, two, three});
  CHECK(eval(mul) == 6);

  // Back-to-back allocations are adjacent: the "locality" claim, measured.
  CHECK(reinterpret_cast<const char *>(three) -
            reinterpret_cast<const char *>(two) ==
        static_cast<std::ptrdiff_t>(sizeof(Node)));
  CHECK(reinterpret_cast<std::uintptr_t>(mul) % alignof(Node) == 0);
  CHECK(a.numSlabs() == 1);

  // A mixed-alignment request still comes back aligned.
  a.allocate(1, 1);
  void *d = a.allocate(sizeof(double), alignof(double));
  CHECK(reinterpret_cast<std::uintptr_t>(d) % alignof(double) == 0);

  // An oversized request gets its own slab and doesn't abandon the current
  // one: the next small node still lands right after the previous one.
  const Node *before = a.create<Node>(Node{'#', 0, nullptr, nullptr});
  a.allocate(10000, 8);
  const Node *after = a.create<Node>(Node{'#', 0, nullptr, nullptr});
  CHECK(a.numSlabs() == 2);
  CHECK(after == before + 1);

  // Filling past one slab starts a new one; nodes from the old slab stay valid.
  for (int i = 0; i < 200; ++i)
    a.create<Node>(Node{'#', i, nullptr, nullptr});
  CHECK(a.numSlabs() > 2);
  CHECK(eval(mul) == 6);
}

} // namespace arena

int main() {
  arena::testBumpAllocator();
  return ts::report("memory_layout/01_bump_allocator");
}
