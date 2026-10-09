// 03_pointer_int_pair.cpp — tagged pointers (llvm::PointerIntPair, PointerUnion).
//
// An object with alignof == 8 always lives at an address whose low 3 bits are
// zero. Those bits are free storage. LLVM uses them everywhere:
//   * PointerIntPair<Value*, 2, unsigned> packs a pointer and a 2-bit field
//     into one word (e.g. Use's link to its User plus a tag).
//   * PointerUnion<A*, B*> stores "either an A* or a B*" in one word, with the
//     discriminator in the low bit — a tagged union with no extra tag field.
//   * MLIR's Type/Attribute handles are a single pointer for the same reason:
//     one word, passed in a register, compared with a single instruction.
// Saving a word per Use/edge matters when a module has tens of millions of them.

#include "test_support.h"

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace tagged {

template <typename T, unsigned IntBits> class PointerIntPair {
  static constexpr std::uintptr_t IntMask = (std::uintptr_t(1) << IntBits) - 1;
  static_assert(alignof(T) >= (1u << IntBits),
                "not enough free low bits in T* for the requested int width");

public:
  PointerIntPair() = default;
  PointerIntPair(T *p, unsigned i) {
    setPointer(p);
    setInt(i);
  }

  T *getPointer() const { return reinterpret_cast<T *>(bits_ & ~IntMask); }
  unsigned getInt() const { return static_cast<unsigned>(bits_ & IntMask); }

  void setPointer(T *p) {
    auto raw = reinterpret_cast<std::uintptr_t>(p);
    assert((raw & IntMask) == 0 && "pointer is not sufficiently aligned");
    bits_ = raw | (bits_ & IntMask);
  }
  void setInt(unsigned i) {
    assert(i <= IntMask && "int does not fit in the reserved bits");
    bits_ = (bits_ & ~IntMask) | i;
  }

private:
  std::uintptr_t bits_ = 0;
};

// PointerUnion is the same trick with the int used as a type discriminator.
template <typename A, typename B> class PointerUnion {
public:
  PointerUnion(A *a) : pair_(reinterpret_cast<Slot *>(a), 0) {}
  PointerUnion(B *b) : pair_(reinterpret_cast<Slot *>(b), 1) {}

  template <typename T> bool is() const {
    return pair_.getInt() == (std::is_same<T, A>::value ? 0u : 1u);
  }
  template <typename T> T *get() const {
    assert(is<T>() && "wrong PointerUnion member");
    return reinterpret_cast<T *>(pair_.getPointer());
  }

private:
  // A stand-in type whose alignment is the weaker of A's and B's.
  struct alignas(alignof(A) < alignof(B) ? alignof(A) : alignof(B)) Slot {};
  PointerIntPair<Slot, 1> pair_;
};

struct alignas(8) Instruction { int id; };
struct alignas(8) Argument { int index; };

enum UseFlags : unsigned { None = 0, IsDebugUse = 1, IsDead = 2 };

void testPointerIntPair() {
  Instruction inst{42};
  PointerIntPair<Instruction, 2> p(&inst, IsDebugUse);

  CHECK(sizeof(p) == sizeof(void *)); // Pointer + flags, one word.
  CHECK(p.getPointer() == &inst);
  CHECK(p.getInt() == IsDebugUse);

  p.setInt(IsDebugUse | IsDead); // Changing the flags leaves the pointer alone…
  CHECK(p.getPointer()->id == 42);
  CHECK(p.getInt() == 3);

  Instruction other{7};
  p.setPointer(&other); // …and changing the pointer leaves the flags alone.
  CHECK(p.getInt() == 3);
  CHECK(p.getPointer()->id == 7);
}

void testPointerUnion() {
  Instruction inst{1};
  Argument arg{2};
  PointerUnion<Instruction, Argument> u = &inst;

  CHECK(sizeof(u) == sizeof(void *)); // vs. 16 bytes for std::variant<I*, A*>.
  CHECK(u.is<Instruction>() && !u.is<Argument>());
  CHECK(u.get<Instruction>()->id == 1);

  u = &arg;
  CHECK(u.is<Argument>());
  CHECK(u.get<Argument>()->index == 2);
}

} // namespace tagged

int main() {
  tagged::testPointerIntPair();
  tagged::testPointerUnion();
  return ts::report("memory_layout/03_pointer_int_pair");
}
