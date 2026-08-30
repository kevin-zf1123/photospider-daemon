#include <openssl/err.h>
#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kEd25519SeedSize = 32U;
constexpr std::size_t kEd25519SignatureSize = 64U;

using TestPkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using TestDigestDeleter = decltype(&EVP_MD_CTX_free);
using TestDigestContext = std::unique_ptr<EVP_MD_CTX, TestDigestDeleter>;

/**
 * @brief Returns one lowercase hexadecimal nibble value.
 * @param character Candidate canonical hexadecimal character.
 * @return Integer nibble in the inclusive range zero through fifteen.
 * @throws std::runtime_error When the character is not lowercase hexadecimal.
 * @note The test seed fixture deliberately rejects uppercase and separators so
 *   its byte identity remains reviewable in Git.
 */
unsigned char decode_hex_nibble(char character) {
  if (character >= '0' && character <= '9') {
    return static_cast<unsigned char>(character - '0');
  }
  if (character >= 'a' && character <= 'f') {
    return static_cast<unsigned char>(character - 'a' + 10);
  }
  throw std::runtime_error("private seed is not canonical lowercase hex");
}

/**
 * @brief Reads the maintained raw Ed25519 private seed fixture.
 * @param path Exact fixture pathname supplied by CMake.
 * @return The 32 decoded private-seed bytes.
 * @throws std::runtime_error When the file cannot be read or is noncanonical.
 * @note Exactly one trailing LF is accepted; no other whitespace or fields are
 *   permitted.
 */
std::array<unsigned char, kEd25519SeedSize> read_private_seed(
    const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open private seed fixture");
  }
  std::string encoded((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  if (input.bad()) {
    throw std::runtime_error("cannot read private seed fixture");
  }
  if (!encoded.empty() && encoded.back() == '\n') {
    encoded.pop_back();
  }
  if (encoded.size() != kEd25519SeedSize * 2U) {
    throw std::runtime_error("private seed fixture has the wrong length");
  }
  std::array<unsigned char, kEd25519SeedSize> seed{};
  for (std::size_t index = 0; index < seed.size(); ++index) {
    const unsigned char high = decode_hex_nibble(encoded[index * 2U]);
    const unsigned char low = decode_hex_nibble(encoded[index * 2U + 1U]);
    seed[index] = static_cast<unsigned char>((high << 4U) | low);
  }
  return seed;
}

/**
 * @brief Reads the complete canonical manifest as binary message bytes.
 * @param path Exact generated manifest pathname supplied by the caller.
 * @return Complete nonempty manifest bytes.
 * @throws std::runtime_error When the file cannot be read or is empty.
 * @note No newline or text normalization occurs after canonical generation.
 */
std::vector<unsigned char> read_manifest(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open generated trust manifest");
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
  if (input.bad()) {
    throw std::runtime_error("cannot read generated trust manifest");
  }
  if (bytes.empty()) {
    throw std::runtime_error("generated trust manifest is empty");
  }
  return bytes;
}

/**
 * @brief Collects the current OpenSSL diagnostic queue for one failed stage.
 * @param stage Human-readable EVP stage that failed.
 * @return Stage plus every queued OpenSSL diagnostic.
 * @throws std::bad_alloc When diagnostic storage cannot allocate.
 * @note Reading drains the calling thread's OpenSSL error queue.
 */
std::string collect_openssl_errors(const std::string& stage) {
  std::string diagnostic = stage;
  std::array<char, 256U> buffer{};
  for (auto error = ERR_get_error(); error != 0UL; error = ERR_get_error()) {
    ERR_error_string_n(error, buffer.data(), buffer.size());
    diagnostic.append(": ").append(buffer.data());
  }
  return diagnostic;
}

/**
 * @brief Signs one canonical manifest with the maintained raw Ed25519 seed.
 * @param seed Exact 32-byte private seed.
 * @param manifest Complete canonical manifest bytes.
 * @return Exact 64-byte Ed25519 signature.
 * @throws std::runtime_error When key construction or signing fails.
 * @throws std::bad_alloc When OpenSSL-owner or signature storage cannot
 *   allocate.
 * @note Raw-key construction avoids command-line PEM/ASN.1 parsing while using
 *   the same OpenSSL provider implementation as production verification.
 */
std::vector<unsigned char> sign_manifest(
    const std::array<unsigned char, kEd25519SeedSize>& seed,
    const std::vector<unsigned char>& manifest) {
  TestPkey key(EVP_PKEY_new_raw_private_key_ex(nullptr, "ED25519", nullptr,
                                               seed.data(), seed.size()),
               &EVP_PKEY_free);
  if (!key) {
    throw std::runtime_error(
        collect_openssl_errors("cannot construct Ed25519 test key"));
  }
  TestDigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context) {
    throw std::bad_alloc();
  }
  if (EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) !=
      1) {
    throw std::runtime_error(
        collect_openssl_errors("cannot initialize Ed25519 test signing"));
  }
  std::vector<unsigned char> signature(kEd25519SignatureSize);
  std::size_t signature_size = signature.size();
  if (EVP_DigestSign(context.get(), signature.data(), &signature_size,
                     manifest.data(), manifest.size()) != 1) {
    throw std::runtime_error(
        collect_openssl_errors("cannot sign plugin trust manifest"));
  }
  if (signature_size != kEd25519SignatureSize) {
    throw std::runtime_error(
        "Ed25519 signer returned the wrong signature size");
  }
  return signature;
}

}  // namespace

/**
 * @brief Signs one generated plugin trust manifest for maintained tests.
 * @param argc Must be three: program, raw private-seed fixture, and manifest.
 * @param argv Command-line strings described by `argc`.
 * @return Zero after writing exactly 64 signature bytes to stdout; one after a
 *   bounded diagnostic on invalid input or a signing failure; two on misuse.
 * @throws Nothing; all standard and OpenSSL failures are converted to stderr
 *   diagnostics and nonzero process status.
 * @note This helper is build-tree-only and never installs key material or a
 *   signing interface.
 */
int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: plugin_trust_manifest_signer SEED MANIFEST\n";
    return 2;
  }
  try {
    const auto seed = read_private_seed(argv[1]);
    const auto manifest = read_manifest(argv[2]);
    const auto signature = sign_manifest(seed, manifest);
    std::cout.write(reinterpret_cast<const char*>(signature.data()),
                    static_cast<std::streamsize>(signature.size()));
    if (!std::cout) {
      throw std::runtime_error("cannot write Ed25519 signature to stdout");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "plugin trust manifest signer: " << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "plugin trust manifest signer: unknown failure\n";
    return 1;
  }
}
