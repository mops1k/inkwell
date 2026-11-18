#pragma once

// Extra ESP-IDF symbols the real firmware sources expect from Arduino.h on the
// device. Host builds get plain deterministic stubs so memory-accounting code
// paths stay harmless.
#ifdef __cplusplus
extern "C" {
#endif

unsigned esp_get_free_heap_size(void);
unsigned esp_get_minimum_free_heap_size(void);

#ifdef __cplusplus
}
#endif
