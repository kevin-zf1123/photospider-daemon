#include "photospider/ipc/client.hpp"

/**
 * @brief Verifies installed client construction uses only public packages.
 * @return Zero when the disconnected client has the expected initial state.
 * @throws std::bad_alloc If client private state allocation fails.
 * @note No daemon connection or external state is created.
 */
int main() {
  ps::ipc::Client client;
  return client.connected() ? 1 : 0;
}
