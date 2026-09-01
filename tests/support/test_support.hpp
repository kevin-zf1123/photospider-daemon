#pragma once

#include <iostream>

namespace ps::ipc::test {

/**
 * @brief Reports one failed condition with source location.
 * @param condition Evaluated condition.
 * @param expression Source expression text.
 * @param file Source filename.
 * @param line Source line.
 * @return True when the condition passed.
 * @throws Nothing with the default non-throwing diagnostic stream.
 * @note The helper never changes product state.
 */
inline bool check(bool condition, const char* expression, const char* file,
                  int line) noexcept {
  if (!condition) {
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
  }
  return condition;
}

}  // namespace ps::ipc::test

#define PS_IPC_CHECK(expression)                                            \
  do {                                                                      \
    if (!::ps::ipc::test::check(static_cast<bool>(expression), #expression, \
                                __FILE__, __LINE__)) {                      \
      return 1;                                                             \
    }                                                                       \
  } while (false)
