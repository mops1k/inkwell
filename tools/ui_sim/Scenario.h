#pragma once

#include <string>
#include <vector>

namespace uisim {

// Reads {"keys":["Right","Right","Confirm"]} from a scenario file.
// Deliberately tiny: the simulator exists for visual checks, not for a script
// language. Unknown fields are ignored; an unreadable file yields no keys.
std::vector<std::string> readScenarioKeys(const std::string& path);

}  // namespace uisim
