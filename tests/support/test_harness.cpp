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

#include "test_harness.hpp"

#include <cstddef>
#include <exception>
#include <string>

namespace bsm_test {

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(std::string name, TestFunction body) {
  cases_.push_back(TestCase{std::move(name), std::move(body)});
}

int run_all(int argc, char** argv, const char* suite_name) {
  const std::vector<TestCase>& cases = Registry::instance().cases();
  std::string filter;
  if (argc > 1) {
    filter = argv[1];
  }
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  for (const TestCase& test : cases) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    Context context;
    try {
      test.body(context);
    } catch (const std::exception& error) {
      context.fail(__FILE__, __LINE__,
                   std::string("unexpected exception: ") + error.what());
    } catch (...) {
      context.fail(__FILE__, __LINE__, "unexpected non-standard exception");
    }
    for (const std::string& note : context.notes()) {
      std::cout << "  note: " << note << '\n';
    }
    if (context.failed()) {
      ++failed;
      std::cout << "[FAIL] " << test.name << ": " << context.message() << '\n';
    } else {
      ++passed;
      std::cout << "[ ok ] " << test.name << '\n';
    }
  }
  std::cout << suite_name << ": " << passed << " passed, " << failed << " failed";
  if (skipped > 0) {
    std::cout << ", " << skipped << " filtered out";
  }
  std::cout << '\n';
  return failed == 0 ? 0 : 1;
}

}  // namespace bsm_test
