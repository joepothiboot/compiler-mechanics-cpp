// 02_small_vector.cpp — small-buffer optimization (llvm::SmallVector).
//
// Most containers in a compiler are tiny: an instruction has 1–3 operands, a
// block has 1–2 predecessors, a worklist rarely exceeds a dozen entries. A
// std::vector heap-allocates for its first element no matter what.
// SmallVector<T, N> stores the first N elements inline (inside the object,
// usually on the stack) and only touches the heap once it outgrows them.
//
// LLVM's coding standard says to prefer SmallVector for local vectors, and
// pass them around as SmallVectorImpl<T>& so callees don't hard-code N.
// This version keeps only the mechanism: inline buffer, grow-to-heap,
// placement new, explicit destruction. Copy/move are deleted to keep it short.

#include "alloc_counter.h"
#include "test_support.h"

#include <cstddef>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace sbo {

template <typename T, unsigned N> class SmallVector {
  static_assert(N > 0, "use std::vector when no inline capacity is wanted");

public:
  SmallVector() = default;
  SmallVector(const SmallVector &) = delete;
  SmallVector &operator=(const SmallVector &) = delete;
  ~SmallVector() {
    clear();
    if (!isSmall())
      ::operator delete(begin_);
  }

  void push_back(T v) {
    if (size_ == capacity_)
      grow();
    new (begin_ + size_) T(std::move(v));
    ++size_;
  }
  void pop_back() { begin_[--size_].~T(); }
  void clear() {
    while (size_)
      pop_back();
  }

  T &operator[](std::size_t i) { return begin_[i]; }
  T *begin() { return begin_; }
  T *end() { return begin_ + size_; }
  std::size_t size() const { return size_; }
  std::size_t capacity() const { return capacity_; }
  bool isSmall() const {
    return begin_ == reinterpret_cast<const T *>(inline_);
  }

private:
  void grow() {
    std::size_t newCap = capacity_ * 2;
    T *mem = static_cast<T *>(::operator new(newCap * sizeof(T)));
    for (std::size_t i = 0; i < size_; ++i) {
      new (mem + i) T(std::move(begin_[i]));
      begin_[i].~T();
    }
    if (!isSmall())
      ::operator delete(begin_);
    begin_ = mem;
    capacity_ = newCap;
  }

  alignas(T) unsigned char inline_[N * sizeof(T)];
  T *begin_ = reinterpret_cast<T *>(inline_);
  std::size_t size_ = 0;
  std::size_t capacity_ = N;
};

int g_live = 0; // Tracks constructions vs destructions of Tracked.
struct Tracked {
  std::string s;
  explicit Tracked(std::string v) : s(std::move(v)) { ++g_live; }
  Tracked(Tracked &&o) : s(std::move(o.s)) { ++g_live; }
  ~Tracked() { --g_live; }
};

void testInlineThenHeap() {
  std::size_t before = g_allocations;
  {
    SmallVector<int, 4> ops;
    for (int i = 0; i < 4; ++i)
      ops.push_back(i);
    CHECK(ops.isSmall());
    CHECK(g_allocations == before); // Four elements, zero heap allocations.

    ops.push_back(4); // Fifth element: spill to the heap, exactly once.
    CHECK(!ops.isSmall());
    CHECK(g_allocations == before + 1);
    CHECK(ops.capacity() == 8);

    int sum = 0;
    for (int v : ops)
      sum += v;
    CHECK(sum == 0 + 1 + 2 + 3 + 4);
  }

  // The same four pushes into std::vector cost at least one allocation
  // (usually three: capacities 1, 2, 4).
  before = g_allocations;
  {
    std::vector<int> v;
    for (int i = 0; i < 4; ++i)
      v.push_back(i);
  }
  CHECK(g_allocations > before);
}

void testNonTrivialElements() {
  {
    SmallVector<Tracked, 2> v;
    v.push_back(Tracked("a"));
    v.push_back(Tracked("b"));
    v.push_back(Tracked("c")); // Moves a, b to the heap and destroys originals.
    CHECK(v.size() == 3);
    CHECK(v[0].s == "a" && v[2].s == "c");
    CHECK(g_live == 3); // No leaked or double-destroyed elements after grow.
  }
  CHECK(g_live == 0);
}

void testSize() {
  // The trade-off: the inline buffer makes the object itself bigger.
  CHECK(sizeof(SmallVector<int, 8>) >= 8 * sizeof(int) + sizeof(int *));
  CHECK(sizeof(SmallVector<int, 8>) > sizeof(std::vector<int>));
}

} // namespace sbo

int main() {
  sbo::testInlineThenHeap();
  sbo::testNonTrivialElements();
  sbo::testSize();
  return ts::report("memory_layout/02_small_vector");
}
