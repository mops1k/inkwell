#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace shelftest {

// Disposable directory on the host filesystem. The shelf code talks to the
// real-filesystem HalStorage shim, so tests need real paths rather than a
// virtual device tree.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    static std::atomic<int> counter{0};
    dir_ = std::filesystem::temp_directory_path() /
           ("shelf_" + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    std::filesystem::create_directories(dir_, ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string path() const { return dir_.string(); }
  std::string join(const std::string& name) const { return (dir_ / name).string(); }

 private:
  std::filesystem::path dir_;
};

}  // namespace shelftest
