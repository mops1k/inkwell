#pragma once

// Forced-include prelude for the host UI simulator. The device's Arduino.h
// transitively provides these; the host shims do not, so they are injected via
// one -include flag (CMake de-duplicates repeated -include options, hence the
// single header rather than several flags).
#include <cmath>

#include "esp_extra.h"
