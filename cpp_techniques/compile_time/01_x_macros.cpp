// 01_x_macros.cpp — one list, many tables (the .def file pattern).
//
// An opcode has a name, an enum value, an arity, flags, maybe a cost. Keeping
// those in five hand-written tables guarantees they drift apart. LLVM keeps the
// list once, in a .def file (llvm/IR/Instruction.def, clang's TokenKinds.def,
// BuiltinTypes.def, ...), and #includes it repeatedly with a different
// definition of the row macro each time:
//
//     #define HANDLE_BINARY_INST(N, OPC, CLASS) OPC = N,
//     #include "llvm/IR/Instruction.def"
//
// Here the list is a function-like macro instead of a separate file, which is
// the same technique in one translation unit. Adding an opcode is a one-line
// change, and every table below, plus the static_asserts, updates with it.
// (TableGen is this idea grown into its own language.)

#include "test_support.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace xmacro {

//      enum name, spelling, arity, commutative
#define OPCODE_LIST(X)                                                         \
  X(Add, "add", 2, true)                                                       \
  X(Sub, "sub", 2, false)                                                      \
  X(Mul, "mul", 2, true)                                                       \
  X(Neg, "neg", 1, false)                                                      \
  X(Ret, "ret", 1, false)

enum class Opcode : std::uint8_t {
#define X(Name, Str, Arity, Comm) Name,
  OPCODE_LIST(X)
#undef X
};

#define X(Name, Str, Arity, Comm) +1
constexpr unsigned NumOpcodes = 0 OPCODE_LIST(X);
#undef X

constexpr const char *OpcodeNames[] = {
#define X(Name, Str, Arity, Comm) Str,
    OPCODE_LIST(X)
#undef X
};

constexpr unsigned getArity(Opcode op) {
  switch (op) {
#define X(Name, Str, Arity, Comm)                                              \
  case Opcode::Name:                                                           \
    return Arity;
    OPCODE_LIST(X)
#undef X
  }
  return 0; // Unreachable; keeps -Wreturn-type quiet.
}

constexpr bool isCommutative(Opcode op) {
  switch (op) {
#define X(Name, Str, Arity, Comm)                                              \
  case Opcode::Name:                                                           \
    return Comm;
    OPCODE_LIST(X)
#undef X
  }
  return false;
}

const char *getName(Opcode op) {
  return OpcodeNames[static_cast<unsigned>(op)];
}

// Parsing reuses the same list: the textual IR reader can't fall out of sync
// with the printer.
bool parseOpcode(const std::string &s, Opcode &out) {
#define X(Name, Str, Arity, Comm)                                              \
  if (s == Str) {                                                              \
    out = Opcode::Name;                                                        \
    return true;                                                               \
  }
  OPCODE_LIST(X)
#undef X
  return false;
}

// The tables are constexpr, so their consistency is checked at compile time.
static_assert(NumOpcodes == 5, "count derived from the list");
static_assert(sizeof(OpcodeNames) / sizeof(OpcodeNames[0]) == NumOpcodes,
              "name table covers every opcode");
static_assert(getArity(Opcode::Neg) == 1, "");
static_assert(isCommutative(Opcode::Mul) && !isCommutative(Opcode::Sub), "");

void testTables() {
  CHECK(std::strcmp(getName(Opcode::Add), "add") == 0);
  CHECK(std::strcmp(getName(Opcode::Ret), "ret") == 0);

  // Round trip every opcode through the printer and parser.
  for (unsigned i = 0; i < NumOpcodes; ++i) {
    Opcode op = static_cast<Opcode>(i), parsed{};
    CHECK(parseOpcode(getName(op), parsed));
    CHECK(parsed == op);
  }
  Opcode dummy{};
  CHECK(!parseOpcode("div", dummy));
}

} // namespace xmacro

int main() {
  xmacro::testTables();
  return ts::report("compile_time/01_x_macros");
}
