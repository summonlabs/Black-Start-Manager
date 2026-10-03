# Contributing to Black Start Manager

Thank you for your interest in contributing to Black Start Manager. This project
is published by Summon Software Labs under the Apache License, Version 2.0.

## License and copyright

By contributing, you agree that your contributions will be licensed to the project
under the Apache License, Version 2.0, consistent with the LICENSE and NOTICE files
in this repository. There is no Contributor License Agreement (CLA) and no
copyright assignment: you retain copyright in your contribution and grant the
project a perpetual, worldwide, non-exclusive, royalty-free license to use it under
the terms of the Apache License, Version 2.0.

## Attribution

Please do not remove or alter the copyright notice or the NOTICE file. Any
distribution of derivative works must carry the attribution notices contained in
the NOTICE file, as required by Section 4 of the Apache License. Do not add
`Co-authored-by` trailers or other attribution trailers to commits in this
repository.

## License headers

New source files carry the following header:

```
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
```

## Building and testing

Requirements: CMake 3.21 or newer and a C++20 compiler. The primary exercised
platform is Windows with MSVC (Visual Studio 2022, toolset 19.44 or newer).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The build must be warning-free. First-party warnings are errors: MSVC builds with
`/W4 /WX /permissive-`, other compilers with `-Wall -Wextra -Wpedantic -Werror`.
Do not disable a warning globally to make a change compile; fix the defect or, when
a warning is genuinely wrong for a specific construct, suppress it narrowly at the
site with a comment that explains why.

## What a change must preserve

- A black-start plan is not actuation authority. Every consequential request is
  bounded by the plan, the bound authority generation, and the readiness gate that
  the request is executing.
- Acknowledgement, observation, and verification stay distinct. A controller reply
  never satisfies a readiness gate.
- Evidence is never promoted to current by recovery. Persisted evidence is
  recovered evidence, and recovered evidence does not satisfy a gate.
- Durable state changes are published through one explicit commit point and are
  never inferred from partial writes.
- Nothing in the library infers authority from existence, observation,
  acknowledgement, recovered state, cached values, names, or apparent health.
- Canonical output must not depend on map iteration order, hash order, thread
  timing, or wall-clock time.

## Reporting issues

Report defects with the exact command line, the printed random seed for a
randomized failure, and the observed versus expected canonical state. Result
digests and journal offsets are the useful identifiers: include them rather than a
description of the screen.
