// Host stubs for ESP-IDF heap queries used by firmware code paths the simulator
// runs (GfxRenderer diagnostics). Values are large and constant on purpose: a
// host build has no PSRAM/DRAM split and must never take an OOM branch.
#include "esp_extra.h"

extern "C" unsigned esp_get_free_heap_size(void) { return 8u * 1024u * 1024u; }

extern "C" unsigned esp_get_minimum_free_heap_size(void) { return 8u * 1024u * 1024u; }
