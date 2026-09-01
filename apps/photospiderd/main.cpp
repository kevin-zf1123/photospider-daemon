#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "server/server.hpp"

namespace {

/** @brief Complete parsed local daemon command-line configuration. */
struct Options final {
  /** @brief Explicit Unix-domain socket path. */
  std::string socket_path;
  /** @brief Fixed global worker count. */
  std::uint32_t maximum_concurrency = 1U;
  /** @brief Fixed retained execution bound. */
  std::uint32_t maximum_jobs = 1024U;
  /** @brief Whether the optional local GPU lane is configured. */
  bool gpu_enabled = false;
  /** @brief Whether help was requested. */
  bool help = false;
};

/**
 * @brief Prints the exact local-only daemon command surface.
 * @throws std::ios_base::failure If configured stream exceptions are enabled.
 * @note No network, policy, plugin-path, or persistence option is advertised.
 */
void print_help() {
  std::cout
      << "Usage: photospiderd --socket PATH [options]\n"
      << "\n"
      << "Options:\n"
      << "  --socket PATH          Unix-domain socket path (required)\n"
      << "  --max-concurrency N    Global execution worker count (default 1)\n"
      << "  --max-jobs N           Retained ephemeral execution bound (default "
         "1024)\n"
      << "  --gpu                   Enable the optional local GPU lane\n"
      << "  --help                  Show this help\n";
}

/**
 * @brief Parses one positive uint32 command argument.
 * @param text Candidate decimal text.
 * @param option Option name used in diagnostics.
 * @return Positive uint32 value.
 * @throws std::invalid_argument If syntax/range is invalid.
 * @throws std::out_of_range If decimal conversion exceeds uint64.
 * @throws std::bad_alloc If conversion/diagnostic allocation fails.
 * @note Zero is rejected for both maintained resource bounds.
 */
std::uint32_t positive_uint32(const std::string& text,
                              const std::string& option) {
  std::size_t consumed = 0U;
  const std::uint64_t value = std::stoull(text, &consumed, 10);
  if (consumed != text.size() || value == 0U ||
      value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(option + " requires a positive uint32");
  }
  return static_cast<std::uint32_t>(value);
}

/**
 * @brief Parses the complete bounded local daemon command line.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Complete options.
 * @throws std::invalid_argument For unknown, missing, or malformed arguments.
 * @throws std::out_of_range If one numeric argument exceeds uint64.
 * @throws std::bad_alloc If argument storage allocation fails.
 * @note Parsing has no filesystem or daemon side effect.
 */
Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help") {
      options.help = true;
    } else if (argument == "--gpu") {
      options.gpu_enabled = true;
    } else if (argument == "--socket" || argument == "--max-concurrency" ||
               argument == "--max-jobs") {
      if (index + 1 >= argc) {
        throw std::invalid_argument(argument + " requires a value");
      }
      const std::string value = argv[++index];
      if (argument == "--socket") {
        options.socket_path = value;
      } else if (argument == "--max-concurrency") {
        options.maximum_concurrency = positive_uint32(value, argument);
      } else {
        options.maximum_jobs = positive_uint32(value, argument);
      }
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  if (!options.help && options.socket_path.empty()) {
    throw std::invalid_argument("--socket PATH is required");
  }
  return options;
}

}  // namespace

/**
 * @brief Runs the non-persistent same-user local orchestration daemon.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Zero after graceful shutdown, two for usage errors, or one for
 * listener/runtime failure.
 * @throws Nothing for standard exceptions; they are reported and mapped to a
 * nonzero process result.
 * @note The daemon never backgrounds itself or persists process state.
 */
int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    if (options.help) {
      print_help();
      return 0;
    }
    ps::ipc::internal::Server server(ps::ipc::internal::ServerConfig{
        options.socket_path,
        ps::ipc::internal::ServiceConfig{options.maximum_concurrency,
                                         options.maximum_jobs,
                                         options.gpu_enabled},
        32});
    const ps::Status status = server.run();
    if (!status.ok()) {
      std::cerr << status.message << '\n';
      return 1;
    }
    return 0;
  } catch (const std::invalid_argument& error) {
    std::cerr << error.what() << '\n';
    print_help();
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
