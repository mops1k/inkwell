#pragma once

namespace shelfui {

// Appends one diagnostic line to /.crosspoint/shelf/cover.log.
//
// Why a file and not the serial log: the emulator's UART stops after a few KB, so a
// cover that never appears leaves no trace at all. The SD card does work in the model
// now, and this file can be read straight from the card image after a run. Writes are
// bounded to the last ~4 KB and are only made by the cover pipeline (a handful of
// lines per book), never in the key-handling path.
void coverTrace(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Empties the file; called once when the shelf is entered so a run starts clean.
void coverTraceReset();

}  // namespace shelfui
