#pragma once

#include <string>

namespace koma {

// Natural sort comparator (case-insensitive, numeric runs compared numerically: 1, 2, ..., 10).
bool natural_less(const std::string& a, const std::string& b);

} // namespace koma
