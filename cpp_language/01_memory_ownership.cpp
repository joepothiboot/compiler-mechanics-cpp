// 01_memory_ownership.cpp — ownership models used in LLVM/MLIR.
//
// The single biggest readability barrier in LLVM for someone coming from a GC'd
// language is that *nothing* is reference counted. Ownership is either (a) a
// unique_ptr held by exactly one place, or (b) a long-lived context/arena that
// owns everything and hands out raw pointers. Raw pointers in LLVM mean
// "non-owning reference", not "maybe leak".

#include "test_support.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ownership {

// ---------------------------------------------------------------------------
// Instrumented IR node: lets tests observe destruction, which is how you verify
// ownership claims instead of assuming them.
// ---------------------------------------------------------------------------
int g_liveInstructions = 0;

class Instruction {
public:
  explicit Instruction(std::string opcode) : opcode_(std::move(opcode)) {
    ++g_liveInstructions;
  }
  ~Instruction() { --g_liveInstructions; }

  // IR nodes are identity objects: two "add" instructions are different values
  // even with identical fields. Deleting the copy ops makes accidental
  // value-copies a compile error rather than a subtle miscompile.
  Instruction(const Instruction &) = delete;
  Instruction &operator=(const Instruction &) = delete;

  const std::string &opcode() const { return opcode_; }

private:
  std::string opcode_;
};

using InstPtr = std::unique_ptr<Instruction>;

// ---------------------------------------------------------------------------
// Factory returning unique_ptr. This is the LLVM signature style for anything
// that transfers ownership (e.g. parseIRFile returning unique_ptr<Module>):
// the return type documents the contract, so no doc comment can go stale.
// ---------------------------------------------------------------------------
InstPtr createInstruction(std::string opcode) {
  return std::make_unique<Instruction>(std::move(opcode));
}

// ---------------------------------------------------------------------------
// "Sink" parameter: by-value unique_ptr means this function *consumes* the
// object. Pass-by-value + std::move at the call site is how LLVM hands a Module
// to a pass pipeline or a JIT — after the call the caller provably has nothing.
// ---------------------------------------------------------------------------
std::string consumeInstruction(InstPtr inst) {
  return inst ? inst->opcode() : std::string("<null>");
}

// ---------------------------------------------------------------------------
// THE MOVED-FROM QUESTION (common interview probe).
//
// After `b = std::move(a)`, why is `a = makeSomething()` still legal?
//
// Because std::move is *only a cast to rvalue* — it moves nothing. The move
// happens inside unique_ptr's move-assignment operator, which the standard
// specifies exactly: it stores the source's pointer, then sets the source's
// pointer to nullptr. So `a` is not "destroyed" or "invalid"; it is a fully
// alive unique_ptr object that now holds nullptr. Every operation is legal on
// it, including assignment (which deletes its current pointer — a no-op for
// nullptr — and takes the new one).
//
// Contrast: moved-from std::string/std::vector are "valid but *unspecified*".
// You may assign to them or call clear(), but you may not assume the contents.
// unique_ptr is stronger: its moved-from state is *specified* to be empty.
// ---------------------------------------------------------------------------
void testUniquePtrMove() {
  InstPtr a = createInstruction("add");
  Instruction *raw = a.get(); // Non-owning observer; stays valid across moves
                              // because the heap object itself never relocates.

  InstPtr b = std::move(a);
  CHECK(a.get() == nullptr);
  CHECK(b.get() == raw);

  a = createInstruction("sub"); // Legal: `a` is an empty-but-alive unique_ptr.
  CHECK(a && a->opcode() == "sub");
  CHECK(g_liveInstructions == 2);

  // std::move on its own does nothing observable — proof that it is a cast.
  (void)std::move(b);
  CHECK(b && b->opcode() == "add");

  // reset() destroys the owned object now; release() gives up ownership WITHOUT
  // destroying (used when handing a pointer to a C API or an arena).
  Instruction *leakedOnPurpose = a.release();
  CHECK(a == nullptr);
  CHECK(g_liveInstructions == 2);
  delete leakedOnPurpose;
  CHECK(g_liveInstructions == 1);

  std::string name = consumeInstruction(std::move(b));
  CHECK(name == "add");
  CHECK(b == nullptr);
  CHECK(g_liveInstructions == 0);
}

// ---------------------------------------------------------------------------
// Context-owned arena: the dominant ownership model in MLIR (MLIRContext) and
// LLVM (LLVMContext/BumpPtrAllocator). One object owns every node; everyone
// else traffics in raw pointers with lifetime tied to the context. This is why
// MLIR APIs return `Operation *` and nobody worries about freeing it.
// ---------------------------------------------------------------------------
class IRContext {
public:
  Instruction *create(std::string opcode) {
    arena_.push_back(std::make_unique<Instruction>(std::move(opcode)));
    return arena_.back().get(); // Stable: unique_ptr targets don't move when
                                // the vector reallocates — only the pointers
                                // inside the vector do.
  }
  size_t size() const { return arena_.size(); }

private:
  std::vector<InstPtr> arena_;
};

void testContextOwnership() {
  Instruction *observer = nullptr;
  {
    IRContext ctx;
    Instruction *i0 = ctx.create("load");
    for (int i = 0; i < 1000; ++i)
      ctx.create("nop"); // Forces vector reallocation.
    CHECK(i0->opcode() == "load"); // i0 still valid after reallocation.
    observer = i0;
    CHECK(g_liveInstructions == 1001);
  }
  CHECK(g_liveInstructions == 0); // Context death frees the whole IR at once.
  (void)observer;                 // Now dangling — exactly the MLIR failure mode
                                  // of using an Operation* past its context.
}

// ---------------------------------------------------------------------------
// RAII scope guard, modeled on mlir::OpBuilder::InsertionGuard.
//
// Compiler passes constantly need "do X with the builder positioned here, then
// restore". Early returns and assertion failures make manual save/restore a bug
// farm; a destructor makes restoration unconditional and exception-safe.
// ---------------------------------------------------------------------------
class Builder {
public:
  using Block = std::vector<std::string>;

  class InsertionGuard {
  public:
    explicit InsertionGuard(Builder &builder)
        : builder_(builder), saved_(builder.insertionPoint_) {}
    ~InsertionGuard() { builder_.insertionPoint_ = saved_; }

    // A movable/copyable guard could restore twice or at the wrong time.
    InsertionGuard(const InsertionGuard &) = delete;
    InsertionGuard &operator=(const InsertionGuard &) = delete;

  private:
    Builder &builder_;
    Block *saved_;
  };

  void setInsertionPoint(Block *block) { insertionPoint_ = block; }
  Block *insertionPoint() const { return insertionPoint_; }
  void createOp(std::string op) { insertionPoint_->push_back(std::move(op)); }

private:
  Block *insertionPoint_ = nullptr;
};

void testRAIIGuard() {
  Builder::Block entry, loopBody;
  Builder b;
  b.setInsertionPoint(&entry);
  b.createOp("alloca");

  {
    Builder::InsertionGuard guard(b);
    b.setInsertionPoint(&loopBody);
    b.createOp("phi");
    b.createOp("add");
    CHECK(b.insertionPoint() == &loopBody);
  } // Guard restores here, even on an early return from the enclosing scope.

  CHECK(b.insertionPoint() == &entry);
  b.createOp("ret");
  CHECK(entry.size() == 2 && loopBody.size() == 2);
  CHECK(entry[1] == "ret");
}

// ---------------------------------------------------------------------------
// shared_ptr and cycles.
//
// LLVM/MLIR almost never use shared_ptr for IR, and cycles are the reason: IR
// graphs are full of back-edges (loop headers, def-use chains, parent links).
// Reference counting cannot reclaim a cycle, so an IR built from shared_ptr
// leaks every loop it contains. Knowing *why* the codebase avoids it is the
// interview-relevant part.
// ---------------------------------------------------------------------------
int g_liveCyclicBlocks = 0;

struct CyclicBlock {
  std::string name;
  std::shared_ptr<CyclicBlock> successor; // strong
  std::shared_ptr<CyclicBlock> predecessor; // strong -> creates the cycle

  explicit CyclicBlock(std::string n) : name(std::move(n)) { ++g_liveCyclicBlocks; }
  ~CyclicBlock() { --g_liveCyclicBlocks; }
};

int g_liveWeakBlocks = 0;

struct WeakBlock {
  std::string name;
  std::shared_ptr<WeakBlock> successor;  // owning edge, forward
  std::weak_ptr<WeakBlock> predecessor;  // observing back-edge: breaks the cycle

  explicit WeakBlock(std::string n) : name(std::move(n)) { ++g_liveWeakBlocks; }
  ~WeakBlock() { --g_liveWeakBlocks; }
};

void testSharedPtrRefcountAndCycle() {
  // Basic refcount mechanics.
  auto p = std::make_shared<CyclicBlock>("entry");
  CHECK(p.use_count() == 1);
  {
    auto q = p;
    CHECK(p.use_count() == 2);
    std::weak_ptr<CyclicBlock> w = q;
    CHECK(p.use_count() == 2); // weak_ptr bumps the weak count, not the strong.
    CHECK(!w.expired());
  }
  CHECK(p.use_count() == 1);
  p.reset();
  CHECK(g_liveCyclicBlocks == 0);

  // The leak: a two-block loop.
  std::weak_ptr<CyclicBlock> observer;
  {
    auto header = std::make_shared<CyclicBlock>("header");
    auto latch = std::make_shared<CyclicBlock>("latch");
    header->successor = latch;
    latch->predecessor = header; // strong back-edge closes the cycle
    observer = header;
    CHECK(g_liveCyclicBlocks == 2);
  }
  // Both locals are gone, yet each block is kept alive by the other.
  CHECK(g_liveCyclicBlocks == 2);
  CHECK(!observer.expired());

  {
    auto revived = observer.lock(); // strong: header=2 (revived + latch)
    revived->successor.reset();     // latch strong count 1 -> 0, destroys latch,
                                    // which releases its ref to header.
  } // revived dies, header count 1 -> 0.
  CHECK(g_liveCyclicBlocks == 0);
  CHECK(observer.expired());

  // The fix: make exactly one direction of every cycle non-owning.
  {
    auto header = std::make_shared<WeakBlock>("header");
    auto latch = std::make_shared<WeakBlock>("latch");
    header->successor = latch;
    latch->predecessor = header;
    CHECK(g_liveWeakBlocks == 2);
    CHECK(latch->predecessor.lock()->name == "header"); // lock() before use
  }
  CHECK(g_liveWeakBlocks == 0);
}

} // namespace ownership

int main() {
  ownership::testUniquePtrMove();
  ownership::testContextOwnership();
  ownership::testRAIIGuard();
  ownership::testSharedPtrRefcountAndCycle();
  return ts::report("01_memory_ownership");
}