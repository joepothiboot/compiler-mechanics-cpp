// test_support.h — dependency-free check harness.
// Compiler codebases assert aggressively (LLVM's assertions builds are the
// default for development), so the idiom you want to internalize is: cheap
// checks everywhere, and a build mode where they're compiled out.
#pragma once

#include <cstdio>
#include <string>

namespace ts {

inline int &failureCount() {
  static int count = 0;
  return count;
}

inline void check(bool cond, const char *expr, const char *file, int line,
                  const std::string &msg = {}) {
  if (cond)
    return;
  ++failureCount();
  std::fprintf(stderr, "FAIL %s:%d: %s %s\n", file, line, expr, msg.c_str());
}

inline int report(const char *suite) {
  if (failureCount() == 0) {
    std::printf("[ OK ] %s: all checks passed\n", suite);
    return 0;
  }
  std::printf("[FAIL] %s: %d check(s) failed\n", suite, failureCount());
  return 1;
}

} // namespace ts

#define CHECK(cond) ::ts::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_MSG(cond, msg) ::ts::check((cond), #cond, __FILE__, __LINE__, (msg))