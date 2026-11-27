# AGENTS.md

## Task

Read the applicable instructions and identify the result and its checks. Answer information requests without modifying files. Ask when a missing decision changes the result, and continue within the agreed scope.

## Changes

Use the existing structure and tools. Limit changes to the task, preserve other people's work and remove code made obsolete by your edits. Ask before changing an interface or agreed requirement. Obtain approval to publish, spend money, change access rights or delete user data. Reuse approval that covers the proposed action.

## Verification

Use documented versions and commands, preferably project-local tools. Cover changed behavior, boundary cases and failures in the existing framework. Reproduce bugs before fixing them when practical. After the last edit run formatting, the affected build and tests, plus wider CI checks justified by the impact. Inspect the diff for unintended edits, secrets, generated output and compatibility changes.

Use supported sanitizers for relevant memory or concurrency risks. Record performance workload, environment, baseline and result. For a release candidate, tie the build, host and artifact hashes to the same commit. See the [native release checklist](docs/native-release-acceptance.md).

## Cross-platform C and C++

- Write shared product logic in portable C. Keep OS headers, system calls and platform-specific formats in platform implementations. Keep core headers usable from a C translation unit.
- Use MinGW-w64 as the primary Windows toolchain. Use native toolchains on Linux and macOS. Express platform and compiler differences on the affected CMake targets.
- Configure third-party C and C++ dependencies in this repository. Pin each dependency to a version or revision, verify downloaded archives by hash, and keep fetched sources and build outputs under the checkout. A clean checkout must build without libraries from another project or a developer-specific installation path.
- Give C interfaces explicit ownership, buffer sizes, error results and cleanup rules. Test invalid input and partial failure. Preserve existing CLI behavior and stored data formats when moving a module into the core.
- Establish the C contract and cover it with behavior tests before adding a C++ layer. Use C++ where a specific feature simplifies the implementation. Examples include RAII for resource cleanup and concepts for constraints shared by several callers. Keep exceptions and C++ types inside that layer.
- Build the shared core and run the same behavior tests on Windows, Linux and macOS. Verify platform-specific behavior through the CLI on the affected host. Report which targets actually passed when describing platform support.

## Finish

Report the result first, then key changes, checks, failed or unrun checks and their reasons. Claim completion only when the evidence meets the agreed criteria. Read the relevant skill or procedure when its conditions apply.

## Agent skills

### Issue tracker

For tasks, specifications and issue dependencies, follow the [GitHub Issues workflow](docs/agents/issue-tracker.md).

### Triage labels

When triaging issues, use the [project label mapping](docs/agents/triage-labels.md).

### Domain docs

Before exploring a feature or proposing a design, consult the [domain documentation guide](docs/agents/domain.md).
