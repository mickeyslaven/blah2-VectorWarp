#include <iostream>
#include <vector>
#include "capture/LocalReceiverGate.h"

// Exercise the real Darwin gate using synthetic bytes; no SDK, dlopen or radio.
namespace fs = std::filesystem;
using namespace blah2::local_receiver;
namespace {
void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
void write(const fs::path& file, const std::string& value) {
  std::ofstream stream(file, std::ios::binary | std::ios::trunc);
  stream << value;
  require(stream.good(), "Cannot write fixture " + file.string());
}
struct Environment {
  std::string name, prior;
  bool existed;
  Environment(const char* key, const fs::path& value) : name(key), existed(std::getenv(key)) {
    if (existed) prior = std::getenv(key);
    setenv(key, value.c_str(), 1);
  }
  ~Environment() { if (existed) setenv(name.c_str(), prior.c_str(), 1); else unsetenv(name.c_str()); }
};
struct Temporary {
  fs::path path;
  explicit Temporary(const fs::path& prefix) {
    std::string pattern = prefix.string() + "XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back(0);
    const char* result = mkdtemp(buffer.data());
    require(result != nullptr, "Cannot create private gate fixture");
    path = result;
  }
  ~Temporary() { std::error_code ignored; fs::remove_all(path, ignored); }
};
}
int main() {
  try {
    const char* home = std::getenv("HOME"); require(home && *home, "HOME is required");
    const fs::path support = fs::path(home) / "Library/Application Support";
    const auto app = support / "VectorWarp";
    fs::create_directories(app);
    Temporary fixture(app / ".gate-test-");
    const auto state = fixture.path / "state", bin = fixture.path / "bin", sdk = fixture.path / "sdk";
    const auto base = state / "adapters/rspduo", generation = base / "generations/one", current = base / "current";
    for (const auto& directory : {generation, bin, sdk}) fs::create_directories(directory);
    const auto module = generation / "blah2-receiver-rspduo.dylib", receipt = generation / "receipt.json";
    const auto core = bin / "libblah2-capture-core.dylib", header = sdk / "sdrplay_api.h", library = sdk / "runtime.dylib";
    for (const auto& file : {module, core, header, library}) write(file, "synthetic " + file.filename().string());
    fs::create_directory_symlink(generation, current);
    Environment stateEnv("VECTORWARP_MACOS_STATE", state), includeEnv("BLAH2_SDRPLAY_INCLUDE_DIR", sdk), libraryEnv("BLAH2_SDRPLAY_LIBRARY", library);
    const auto original = std::string("{\"schema\":1,\"kit_id\":\"fixture-kit\",\"cohort\":\"fixture-cohort\",\"module_sha256\":\"") + sha256_user_file(module) +
      "\",\"core_sha256\":\"" + sha256_user_file(core) + "\",\"sdk\":{\"include\":\"" + sha256_user_file(header) +
      "\",\"library\":\"" + sha256_user_file(library) + "\"}}";
    write(receipt, original);
    const auto check = [&] { return verify(bin.string(), "fixture-kit", "fixture-cohort"); };
    unsigned cases = 0;
    const auto rejected = [&](const std::string& label, auto operation) {
      bool failed = false;
      try { operation(); } catch (const std::exception&) { failed = true; }
      require(failed, "Accepted " + label); ++cases;
    };
    const auto accepted = check();
    require(accepted.module == module.string(), "Gate must return the immutable generation path"); ++cases;
    for (const auto& file : {core, header, library}) {
      const auto target = file.string() + ".versioned";
      fs::rename(file, target); fs::create_symlink(target, file);
      require(check().module == module.string(), "Installed core/SDK symlink must resolve to its verified regular target"); ++cases;
      fs::remove(file); fs::rename(target, file);
    }
    for (const auto& raw : {std::string("{"), std::string("null"), std::string("[]"), std::string("{\"schema\":2}"), std::string(8193, 'x')}) {
      write(receipt, raw); rejected("invalid or oversized receipt", check);
    }
    write(receipt, original);
    for (const auto& file : {module, core, header, library}) {
      std::ifstream stream(file); const std::string bytes((std::istreambuf_iterator<char>(stream)), {});
      write(file, "changed"); rejected("changed " + file.filename().string(), check); write(file, bytes);
    }
    rejected("wrong kit", [&] { verify(bin.string(), "other-kit", "fixture-cohort"); });
    rejected("wrong cohort", [&] { verify(bin.string(), "fixture-kit", "other-cohort"); });
    chmod(module.c_str(), 0666); rejected("writable module", check); chmod(module.c_str(), 0600);
    chmod(generation.c_str(), 0777); rejected("writable generation", check); chmod(generation.c_str(), 0700);
    const auto saved = receipt.string() + ".saved";
    fs::rename(receipt, saved); require(mkfifo(receipt.c_str(), 0600) == 0, "Cannot create receipt FIFO");
    rejected("receipt FIFO", check); fs::remove(receipt); fs::rename(saved, receipt);
    fs::rename(library, saved); require(mkfifo(library.c_str(), 0600) == 0, "Cannot create SDK FIFO");
    rejected("SDK FIFO", check); fs::remove(library); fs::rename(saved, library);
    fs::remove(current); fs::create_directory_symlink(sdk, current); rejected("outside generation pointer", check);
    fs::remove(current); fs::create_directories(base / "generations/two"); fs::create_directory_symlink(base / "generations/two", current);
    require(accepted.module == module.string() && fs::exists(accepted.module), "Published pointer change must not redirect a verified module"); ++cases;
    fs::remove(current); fs::create_directory_symlink(generation, current);
    Temporary sibling(support / "VectorWarpSibling-gate-test-");
    fs::copy(state, sibling.path / "state", fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    { Environment wrongState("VECTORWARP_MACOS_STATE", sibling.path / "state"); rejected("sibling state prefix", check); }
    const auto race = fixture.path / "replace-during-read";
    write(race, "original bytes");
    rejected("pathname replacement while reading", [&] {
      read_user_file(race, 1024, [&](const unsigned char*, std::size_t) {
        fs::rename(race, race.string() + ".old"); write(race, "original bytes");
      });
    });
    require(check().module == module.string(), "Restored publication must remain loadable"); ++cases;
    std::cout << "Darwin local receiver gate: " << cases << " synthetic native checks passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
