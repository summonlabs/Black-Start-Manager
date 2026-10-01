// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "detail/crash_point.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace black_start_manager::detail {
namespace {

[[nodiscard]] bool names_point(const char* list, const char* name) noexcept {
  if (list == nullptr || name == nullptr) {
    return false;
  }
  const std::string_view text(list);
  if (text.empty()) {
    return false;
  }
  if (text == "*") {
    return true;
  }
  const std::string_view wanted(name);
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    if (text.substr(start, end - start) == wanted) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return false;
}

}  // namespace

void crash_point(const char* name) noexcept {
  char* armed = nullptr;
#if defined(_WIN32)
  std::size_t armed_size = 0;
  if (::_dupenv_s(&armed, &armed_size, "BSM_CRASH_AT") != 0) {
    return;
  }
#else
  armed = std::getenv("BSM_CRASH_AT");
#endif
  if (armed == nullptr || name == nullptr) {
#if defined(_WIN32)
    std::free(armed);
#endif
    return;
  }
  const bool matched = names_point(armed, name);
#if defined(_WIN32)
  std::free(armed);
#endif
  if (!matched) {
    return;
  }
  // Report the exact point on stderr (already flushed) so a harness can attribute the
  // exit, then terminate without unwinding.
  std::fprintf(stderr, "bsm: crash injected at %s\n", name);
  std::fflush(nullptr);
  std::_Exit(kCrashExitCode);
}

}  // namespace black_start_manager::detail
