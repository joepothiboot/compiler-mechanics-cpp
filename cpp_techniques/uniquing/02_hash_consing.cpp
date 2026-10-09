// 02_hash_consing.cpp — structural uniquing of types (MLIR TypeStorage,
// llvm::FoldingSet, LLVM's uniqued Constants and types).
//
// String interning, generalized to trees. `i32`, `ptr<i32>` and
// `(i32, ptr<i32>) -> i32` are built from parts, and two types with the same
// structure must compare equal. Instead of deep structural comparison
// everywhere, the context guarantees each distinct structure is created once:
// a get() first looks up (kind, params, children) and returns the existing
// node if there is one. Because children are themselves uniqued, comparing
// children is a pointer comparison too, so the lookup key stays shallow.
//
// Consequences that show up all over LLVM/MLIR:
//   * Type equality is `a == b` on a one-word handle (mlir::Type is a pointer
//     to its uniqued storage; llvm::Type* is compared with ==).
//   * Types are immutable: mutating a shared node would change every user.
//   * Types live as long as the context; they are never individually freed.

#include "test_support.h"

#include <cstddef>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace hashcons {

enum class Kind { Integer, Pointer, Function };

struct TypeStorage {
  Kind kind;
  unsigned width;                         // Integer only.
  std::vector<const TypeStorage *> elems; // Pointer: {pointee}; Function: {result, params...}
  unsigned id;                            // Creation order; not part of the key.
};

// Value-typed handle, like mlir::Type: one pointer, compared by identity.
class Type {
public:
  explicit Type(const TypeStorage *s) : s_(s) {}
  bool operator==(Type o) const { return s_ == o.s_; }
  bool operator!=(Type o) const { return s_ != o.s_; }
  Kind kind() const { return s_->kind; }
  const TypeStorage *storage() const { return s_; }

  std::string str() const {
    switch (s_->kind) {
    case Kind::Integer:
      return "i" + std::to_string(s_->width);
    case Kind::Pointer:
      return "ptr<" + Type(s_->elems[0]).str() + ">";
    case Kind::Function: {
      std::string out = "(";
      for (std::size_t i = 1; i < s_->elems.size(); ++i)
        out += (i > 1 ? ", " : "") + Type(s_->elems[i]).str();
      return out + ") -> " + Type(s_->elems[0]).str();
    }
    }
    return "?";
  }

private:
  const TypeStorage *s_;
};

class TypeContext {
public:
  Type getInteger(unsigned width) { return get(Kind::Integer, width, {}); }
  Type getPointer(Type pointee) {
    return get(Kind::Pointer, 0, {pointee.storage()});
  }
  Type getFunction(Type result, std::vector<Type> params) {
    std::vector<const TypeStorage *> elems{result.storage()};
    for (Type p : params)
      elems.push_back(p.storage());
    return get(Kind::Function, 0, std::move(elems));
  }
  std::size_t numUniqueTypes() const { return types_.size(); }

private:
  // Shallow key: children are compared by id, which is sound because children
  // are already unique. ids rather than raw pointers keep the ordering total.
  struct KeyLess {
    static std::vector<unsigned> ids(const TypeStorage &t) {
      std::vector<unsigned> out;
      for (const TypeStorage *e : t.elems)
        out.push_back(e->id);
      return out;
    }
    bool operator()(const TypeStorage &a, const TypeStorage &b) const {
      return std::make_tuple(a.kind, a.width, ids(a)) <
             std::make_tuple(b.kind, b.width, ids(b));
    }
  };

  Type get(Kind k, unsigned width, std::vector<const TypeStorage *> elems) {
    // std::set nodes never move, so pointers to elements are stable handles.
    // ponytail: ordered set + per-compare id vectors is O(log n * arity);
    // use a hash table keyed on a precomputed hash if type creation is hot.
    auto it = types_
                  .insert(TypeStorage{k, width, std::move(elems),
                                      static_cast<unsigned>(types_.size())})
                  .first;
    return Type(&*it);
  }

  std::set<TypeStorage, KeyLess> types_;
};

void testUniquing() {
  TypeContext ctx;
  Type i32 = ctx.getInteger(32);
  Type i64 = ctx.getInteger(64);

  CHECK(ctx.getInteger(32) == i32);
  CHECK(i32 != i64);

  // Built twice, independently: same node, so == is a pointer compare.
  Type f1 = ctx.getFunction(i32, {i32, ctx.getPointer(i32)});
  Type f2 = ctx.getFunction(ctx.getInteger(32),
                            {ctx.getInteger(32), ctx.getPointer(ctx.getInteger(32))});
  CHECK(f1 == f2);
  CHECK(f1.str() == "(i32, ptr<i32>) -> i32");

  // Structure, not just shape, matters.
  CHECK(ctx.getPointer(i32) != ctx.getPointer(i64));
  CHECK(ctx.getFunction(i32, {i64}) != ctx.getFunction(i64, {i32}));

  // Many requests, few nodes: i32, i64, ptr<i32>, ptr<i64>, 3 function types.
  for (int i = 0; i < 1000; ++i)
    ctx.getFunction(i32, {i32, ctx.getPointer(i32)});
  CHECK(ctx.numUniqueTypes() == 7);
}

} // namespace hashcons

int main() {
  hashcons::testUniquing();
  return ts::report("uniquing/02_hash_consing");
}
