// 01_array_ref_string_ref.cpp — non-owning views (llvm::ArrayRef, StringRef).
//
// A function that reads a list of operands shouldn't care whether the caller
// holds a std::vector, a SmallVector, a C array or a single element, and it
// certainly shouldn't force a copy into one of those. LLVM's answer is a
// two-word view: pointer + length, no ownership.
//   * ArrayRef<T>  — read-only view of contiguous T (C++20 calls it std::span)
//   * StringRef    — read-only view of chars (std::string_view since C++17)
// Passing views by value is the LLVM norm. Slicing a view is free: no
// allocation, no copying, just a new pointer and length.
//
// The hazard is the flip side of "no ownership": a view must not outlive the
// storage it points into. The classic bug:
//     ArrayRef<int> r = {1, 2, 3};   // initializer_list dies at the semicolon
//     use(r);                        // dangling
//     StringRef s = std::string("tmp"); // same bug with a temporary string

#include "alloc_counter.h"
#include "test_support.h"

#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace views {

template <typename T> class ArrayRef {
public:
  ArrayRef() = default;
  ArrayRef(const T &one) : data_(&one), size_(1) {}
  ArrayRef(const T *data, std::size_t n) : data_(data), size_(n) {}
  template <std::size_t N> ArrayRef(const T (&arr)[N]) : data_(arr), size_(N) {}
  ArrayRef(const std::vector<T> &v) : data_(v.data()), size_(v.size()) {}

  const T *begin() const { return data_; }
  const T *end() const { return data_ + size_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  const T &operator[](std::size_t i) const {
    assert(i < size_ && "ArrayRef index out of range");
    return data_[i];
  }

  ArrayRef slice(std::size_t start, std::size_t n) const {
    assert(start + n <= size_ && "slice out of range");
    return ArrayRef(data_ + start, n);
  }
  ArrayRef drop_front(std::size_t n = 1) const { return slice(n, size_ - n); }

private:
  const T *data_ = nullptr;
  std::size_t size_ = 0;
};

// One signature serves every caller below.
int sum(ArrayRef<int> xs) {
  int s = 0;
  for (int x : xs)
    s += x;
  return s;
}

// StringRef-style parsing: split "%a, %b, %c" without allocating a single
// substring. Each piece points back into the original buffer.
std::size_t countOperands(std::string_view s) {
  std::size_t n = 0;
  while (!s.empty()) {
    std::size_t comma = s.find(',');
    std::string_view tok = s.substr(0, comma);
    while (!tok.empty() && tok.front() == ' ')
      tok.remove_prefix(1);
    if (!tok.empty())
      ++n;
    if (comma == std::string_view::npos)
      break;
    s.remove_prefix(comma + 1);
  }
  return n;
}

// The same split done with std::string, for the allocation comparison.
std::size_t countOperandsCopying(const std::string &s) {
  std::size_t n = 0, pos = 0;
  while (pos <= s.size()) {
    std::size_t comma = s.find(',', pos);
    std::string tok = s.substr(pos, comma - pos); // Allocates if > SSO size.
    if (tok.find_first_not_of(' ') != std::string::npos)
      ++n;
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  return n;
}

void testArrayRef() {
  std::vector<int> vec{1, 2, 3, 4};
  int arr[] = {10, 20};
  int one = 7;

  std::size_t before = g_allocations;
  CHECK(sum(vec) == 10);
  CHECK(sum(arr) == 30);
  CHECK(sum(one) == 7);
  CHECK(sum(ArrayRef<int>(vec).drop_front(2)) == 7);
  CHECK(sum(ArrayRef<int>(vec).slice(1, 2)) == 5);
  CHECK(g_allocations == before); // No conversion copied anything.

  CHECK(sizeof(ArrayRef<int>) == 2 * sizeof(void *)); // Pass by value.
}

void testStringRef() {
  // Operand names longer than any std::string small-buffer, so the copying
  // version can't hide behind SSO.
  std::string line = "%a_very_long_operand_name_number_one, "
                     "%a_very_long_operand_name_number_two, "
                     "%a_very_long_operand_name_number_three";

  std::size_t before = g_allocations;
  CHECK(countOperands(line) == 3);
  CHECK(g_allocations == before);

  before = g_allocations;
  CHECK(countOperandsCopying(line) == 3);
  CHECK(g_allocations >= before + 3); // One per substring, at least.
}

} // namespace views

int main() {
  views::testArrayRef();
  views::testStringRef();
  return ts::report("views_and_callables/01_array_ref_string_ref");
}
