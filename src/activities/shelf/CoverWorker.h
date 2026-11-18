#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "ShelfCoverService.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace shelfui {

// Decodes book artwork outside the activity thread.
//
// Why this exists: a .fb2.zip cover means inflating a whole FB2 XML out of the
// archive, which takes ~1 s on the C3 and much longer in the QEMU model. Doing
// that on the activity thread is what made moving the focus in the grid wait for
// the decoder whenever a key fell into the decode window. The worker owns its own
// ShelfCoverService instance and talks to the UI only through the two queues
// below, so a key press can never be stuck behind an image.
//
// SD safety: HalStorage serialises every operation with a recursive mutex and the
// shared SPI bus lock (SPI outer, storage inner — the same order the display
// uses), so reading/writing thumbnails from this task is safe next to the UI.
class CoverWorker {
 public:
  struct Result {
    int32_t cellIndex = -1;
    std::string thumbPath;
  };

  // Called from the UI loop with every cover that just landed. The cell index is
  // whatever was passed to request(); -1 means the cell is gone.
  using ResultFn = std::function<void(const Result&)>;

  CoverWorker() = default;
  ~CoverWorker();

  // Spawns the task on first use. Safe to call again after stop().
  bool start();
  void stop();

  // Queues a cover. Returns false when the queue is full or the worker is not
  // running — the caller keeps its placeholder and can retry on the next paint.
  bool request(int32_t cellIndex, const std::string& path, shelf::ShelfCoverSize size);

  // Drains finished covers. Never blocks: results are collected with a zero
  // timeout, so the caller stays inside its normal frame budget.
  void poll(const ResultFn& onResult);

  // While paused the task drops queued work instead of decoding: the shelf is not
  // visible, and the reader must not wait for an image on the shared SD card. A
  // decode already in progress finishes; the queue behind it is discarded.
  void setPaused(bool paused) { paused_ = paused; }
  void cancelPending();

  bool running() const { return task_ != nullptr; }

 private:
  struct Request {
    int32_t cellIndex = -1;
    shelf::ShelfCoverSize size = shelf::ShelfCoverSize::Cell;
    char path[192] = {};
  };

  static void taskEntry(void* arg);
  void run();
  // Runs the whole cover ladder for one book: synchronous attempt, then the
  // sliced extraction/decode that .fb2.zip and EPUB need, then a final attempt.
  std::string makeCover(const std::string& path, shelf::ShelfCoverSize size);

  QueueHandle_t requests_ = nullptr;
  QueueHandle_t results_ = nullptr;
  TaskHandle_t task_ = nullptr;
  // Single writer (the UI task), single reader (the worker): a plain volatile
  // flag is enough on the 32-bit core, same as the serial-mute flag in Logging.
  volatile bool paused_ = false;
  // The service is a plain class in the global namespace (ShelfActivity owns one
  // too); only the worker wrapper lives in shelfui.
  ShelfCoverService service_;
};

}  // namespace shelfui
