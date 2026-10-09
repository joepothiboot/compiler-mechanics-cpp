// 01_string_interning.cpp — interned identifiers (StringPool, mlir::StringAttr,
// clang::IdentifierInfo).
//
// A compiler compares names constantly: symbol lookup, attribute names, op
// names ("arith.addi"), keywords. Comparing std::strings is O(length) and
// hashing them is O(length) every time. Interning stores each distinct string
// exactly once and hands out a pointer to that canonical copy. After that:
//   * equality is a pointer comparison, O(1)
//   * hashing is hashing the pointer, O(1)
//   * a Symbol is one word, cheap to copy and store in maps
// clang's lexer interns every identifier into an IdentifierTable as it reads
// it; MLIR interns op names and string attributes in the MLIRContext.
//
// Interning is only valid within one pool: Symbols from different pools must
// never be compared, which is why the pool lives in the global context object.

#include "test_support.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace interning {

class Symbol {
public:
  Symbol() = default;
  std::string_view str() const { return *s_; }
  bool operator==(Symbol o) const { return s_ == o.s_; } // Pointer compare.
  bool operator!=(Symbol o) const { return s_ != o.s_; }
  const void *opaque() const { return s_; }

private:
  friend class StringPool;
  explicit Symbol(const std::string *s) : s_(s) {}
  const std::string *s_ = nullptr;
};

struct SymbolHash {
  std::size_t operator()(Symbol s) const {
    return std::hash<const void *>{}(s.opaque()); // O(1), whatever the length.
  }
};

class StringPool {
public:
  Symbol intern(std::string_view s) {
    // unordered_set is node-based: element addresses survive rehashing, so
    // the pointer inside a Symbol stays valid as the pool grows.
    // ponytail: C++17 unordered_set has no heterogeneous lookup, so a lookup
    // builds a std::string; use C++20 transparent hashing or a custom table
    // if interning shows up in profiles.
    auto it = pool_.emplace(s).first;
    return Symbol(&*it);
  }
  std::size_t size() const { return pool_.size(); }

private:
  std::unordered_set<std::string> pool_;
};

void testInterning() {
  StringPool pool;
  std::string built = std::string("arith.") + "addi"; // Different buffer.

  Symbol a = pool.intern("arith.addi");
  Symbol b = pool.intern(built);
  Symbol c = pool.intern("arith.muli");

  CHECK(a == b); // Same text, same pointer, regardless of where it came from.
  CHECK(a != c);
  CHECK(a.str() == "arith.addi");
  CHECK(pool.size() == 2);

  // Force many rehashes; earlier Symbols must remain valid and equal.
  for (int i = 0; i < 10000; ++i)
    pool.intern("tmp" + std::to_string(i));
  CHECK(pool.intern("arith.addi") == a);
  CHECK(a.str() == "arith.addi");

  // Symbols as map keys: hashing and equality never touch the characters.
  std::unordered_map<Symbol, int, SymbolHash> opCounts;
  ++opCounts[a];
  ++opCounts[b];
  ++opCounts[c];
  CHECK(opCounts[a] == 2);
  CHECK(opCounts[c] == 1);
}

} // namespace interning

int main() {
  interning::testInterning();
  return ts::report("uniquing/01_string_interning");
}
