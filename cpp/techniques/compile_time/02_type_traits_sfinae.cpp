// 02_type_traits_sfinae.cpp — compile-time type questions, LLVM-style.
//
// LLVM's ADT headers are full of small metaprograms that ask questions about
// types at compile time and pick an implementation accordingly:
//   * llvm::is_one_of<T, Ts...>     — "is T any of these?" (variadic + fold)
//   * isa<A, B, C>(v)               — variadic isa, one check per type
//   * detection idiom (void_t)      — "does T have a classof?" so isa<> can
//                                     give a readable error instead of a wall
//                                     of template spew
//   * if constexpr / enable_if      — pick a hashing strategy per type
//                                     (llvm::hash_value overloads, DenseMapInfo)
// All of it resolves at compile time; none of it exists in the binary.

#include "test_support.h"

#include <cstddef>
#include <functional>
#include <string>
#include <type_traits>

namespace traits {

// ---------------------------------------------------------------------------
// is_one_of: a variadic fold over std::is_same.
// ---------------------------------------------------------------------------
template <typename T, typename... Ts>
struct is_one_of : std::bool_constant<(std::is_same<T, Ts>::value || ...)> {};

static_assert(is_one_of<int, char, int, long>::value, "");
static_assert(!is_one_of<float, char, int, long>::value, "");

// ---------------------------------------------------------------------------
// Detection idiom: has_classof<To, From> is true only when
// To::classof(const From*) is a valid expression.
// ---------------------------------------------------------------------------
template <typename To, typename From, typename = void>
struct has_classof : std::false_type {};
template <typename To, typename From>
struct has_classof<To, From,
                   std::void_t<decltype(To::classof(std::declval<const From *>()))>>
    : std::true_type {};

struct Value {
  enum Kind { K_Const, K_Arg, K_Inst } kind;
};
struct Constant : Value {
  static bool classof(const Value *v) { return v->kind == K_Const; }
};
struct Argument : Value {
  static bool classof(const Value *v) { return v->kind == K_Arg; }
};
struct NotInHierarchy {}; // No classof: isa<NotInHierarchy> must not compile.

static_assert(has_classof<Constant, Value>::value, "");
static_assert(!has_classof<NotInHierarchy, Value>::value, "");

// isa<A, B, ...>(v): true if any matches. The pack expands into one classof
// call per type; From is deduced after the explicitly-given pack.
template <typename To, typename... Rest, typename From>
bool isa(const From *v) {
  static_assert(has_classof<To, From>::value,
                "isa<To>: To must declare static bool classof(const From*)");
  if constexpr (sizeof...(Rest) == 0)
    return To::classof(v);
  else
    return To::classof(v) || isa<Rest...>(v);
}

// ---------------------------------------------------------------------------
// if constexpr: pick a hashing strategy per type. Types that know how to hash
// themselves (a .hash() member) use it; everything else defers to std::hash.
// ---------------------------------------------------------------------------
template <typename T, typename = void> struct has_hash_member : std::false_type {};
template <typename T>
struct has_hash_member<T, std::void_t<decltype(std::declval<const T &>().hash())>>
    : std::true_type {};

struct InternedName {
  std::size_t precomputed;
  std::size_t hash() const { return precomputed; } // O(1): already hashed.
};

template <typename T> std::size_t hashValue(const T &v) {
  if constexpr (has_hash_member<T>::value)
    return v.hash();
  else
    return std::hash<T>{}(v);
}

// enable_if variant of the same dispatch, as found in pre-C++17 code (and
// still all over LLVM): SFINAE removes the overload that doesn't apply.
template <typename T>
std::enable_if_t<std::is_integral<T>::value, const char *> describe(T) {
  return "integer";
}
template <typename T>
std::enable_if_t<std::is_pointer<T>::value, const char *> describe(T) {
  return "pointer";
}

void testTraits() {
  Value c{Value::K_Const}, a{Value::K_Arg}, i{Value::K_Inst};
  CHECK(isa<Constant>(&c));
  CHECK(!isa<Constant>(&a));
  CHECK((isa<Constant, Argument>(&a)));
  CHECK(!(isa<Constant, Argument>(&i)));
  // isa<NotInHierarchy>(&c); // ← fails with the static_assert message above.

  CHECK(hashValue(InternedName{1234}) == 1234);
  CHECK(hashValue(std::string("x")) == std::hash<std::string>{}("x"));

  int n = 0;
  CHECK(std::string(describe(42)) == "integer");
  CHECK(std::string(describe(&n)) == "pointer");
}

} // namespace traits

int main() {
  traits::testTraits();
  return ts::report("compile_time/02_type_traits_sfinae");
}
