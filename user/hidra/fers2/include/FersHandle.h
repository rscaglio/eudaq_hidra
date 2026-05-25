// RAII wrapper for FERS device handles
#ifndef HIDRA_FERS2_FERSHANDLE_H_
#define HIDRA_FERS2_FERSHANDLE_H_

#include <string>
#include "FERSlib.h"
#include "FersException.h"

namespace hidra {
namespace fers2 {

class FersHandle {
 public:
  FersHandle() noexcept : handle_(-1) {}
  explicit FersHandle(int h) noexcept : handle_(h) {}

  // Open device by path; throws FersError on failure.
  explicit FersHandle(const std::string& path) {
    int h = -1;
    int ret = FERS_OpenDevice(const_cast<char*>(path.c_str()), &h);
    if (ret != 0) {
      throw FersError("FERS_OpenDevice failed for '" + path + "'", ret);
    }
    handle_ = h;
  }

  // Non-copyable, movable
  FersHandle(const FersHandle&) = delete;
  FersHandle& operator=(const FersHandle&) = delete;

  FersHandle(FersHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }
  FersHandle& operator=(FersHandle&& other) noexcept {
    reset();
    handle_ = other.handle_;
    other.handle_ = -1;
    return *this;
  }

  ~FersHandle() noexcept {
    reset();
  }

  int get() const noexcept { return handle_; }
  explicit operator bool() const noexcept { return handle_ >= 0; }

  void reset() noexcept {
    if (handle_ >= 0) {
      FERS_CloseDevice(handle_);
      handle_ = -1;
    }
  }

 private:
  int handle_ = -1;
};

}  // namespace fers2
}  // namespace hidra

#endif  // HIDRA_FERS2_FERSHANDLE_H_
