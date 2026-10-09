// 01_expected.cpp — error handling without exceptions (llvm::Expected<T>).
//
// LLVM builds with -fno-exceptions, so a parser that hits bad input can't
// throw. It returns llvm::Expected<T>: either a T or an Error. Two things make
// it more than a std::variant:
//   1. Errors must be handled. In assertions builds, destroying an Expected
//      that was never checked aborts with "Expected<T> must be checked before
//      access or destruction". Forgetting to look at an error is a crash in
//      testing, not silent wrong output in production.
//   2. Errors propagate with one line: `if (!x) return x.takeError();` moves
//      the error into the caller's own Expected<U>, whatever U is.
// (std::expected is the C++23 standard-library version of the same idea.)
//
// This version counts unchecked destructions instead of aborting, so the test
// can observe the rule being broken.

#include "test_support.h"

#include <cassert>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace errors {

struct Error {
  std::string message;
};

inline int &uncheckedDestructions() {
  static int n = 0;
  return n;
}

template <typename T> class [[nodiscard]] Expected {
public:
  Expected(T value) : storage_(std::move(value)) {}
  Expected(Error err) : storage_(std::move(err)) {}
  Expected(Expected &&other)
      : storage_(std::move(other.storage_)), checked_(other.checked_) {
    other.checked_ = true; // Responsibility moves with the value.
  }
  Expected(const Expected &) = delete;
  ~Expected() {
    if (!checked_)
      ++uncheckedDestructions(); // LLVM: report_fatal_error / abort here.
  }

  // Testing for success is what marks it checked — even when it succeeded.
  explicit operator bool() {
    checked_ = true;
    return storage_.index() == 0;
  }
  T &operator*() {
    assert(checked_ && "Expected<T> accessed before being checked");
    return std::get<T>(storage_);
  }
  Error takeError() {
    checked_ = true;
    return std::get<Error>(std::move(storage_));
  }

private:
  std::variant<T, Error> storage_;
  bool checked_ = false;
};

// A tiny parser for "add 1, 2"-style instructions.
Expected<int> parseInt(const std::string &tok) {
  if (tok.empty())
    return Error{"expected integer, got end of input"};
  int v = 0;
  for (char ch : tok) {
    if (ch < '0' || ch > '9')
      return Error{"expected integer, got '" + tok + "'"};
    v = v * 10 + (ch - '0');
  }
  return v;
}

Expected<std::vector<int>> parseOperands(const std::vector<std::string> &toks) {
  std::vector<int> out;
  for (const std::string &t : toks) {
    Expected<int> v = parseInt(t);
    if (!v)
      return v.takeError(); // Propagate: Expected<int> → Expected<vector<int>>.
    out.push_back(*v);
  }
  return out;
}

void testExpected() {
  {
    auto ops = parseOperands({"1", "22", "333"});
    CHECK(static_cast<bool>(ops));
    CHECK((*ops == std::vector<int>{1, 22, 333}));
  }
  {
    auto ops = parseOperands({"1", "x2", "3"});
    CHECK(!ops);
    CHECK(ops.takeError().message == "expected integer, got 'x2'");
  }
  CHECK(uncheckedDestructions() == 0);

  // Breaking the rule: a result nobody looked at. [[nodiscard]] catches the
  // fully-ignored call at compile time; this catches the stored-but-ignored one.
  {
    Expected<int> ignored = parseInt("7");
    (void)ignored;
  }
  CHECK(uncheckedDestructions() == 1);
}

} // namespace errors

int main() {
  errors::testExpected();
  return ts::report("error_handling/01_expected");
}
