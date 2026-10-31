#pragma once

// The fallback implementation is compiled in its own namespace. Preload every
// system header it uses globally, so the baseline sources' include guards keep
// standard-library and SDK declarations out of namespace fallback.
#include <algorithm>
#include <array>
#include <bit>
#include <cambc.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <tuple>
#include <variant>
#include <vector>
#include <wasm_simd128.h>

namespace fallback {
#include "baseline/bot.h"
#include "baseline/draft/draft.h"
} // namespace fallback
