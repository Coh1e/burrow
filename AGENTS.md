# AGENTS.md

## Task

1. Read the project instructions that apply to the task.
2. Identify the required result and the checks that can confirm it.
3. For a request for information, give the findings without changing files.
4. Ask the user when a missing decision changes the required result.
5. Continue work within the agreed scope without repeated approval requests.

## Changes

1. Make only the changes necessary for the task.
2. Use the existing project structure and tools.
3. Preserve changes made by other people.
4. Remove unused code that your changes cause.
5. Ask before you change an existing interface or an agreed requirement.
6. Get approval before you publish, spend money, change access rights, or delete user data.
7. Use existing approval when it covers the proposed action.

## Verification

1. Use the repository's documented commands and tool versions. Prefer project-local tools.
2. Add or update tests for changed behavior. For a bug, reproduce the failure before the fix when practical.
3. Test relevant boundary cases and failure paths. Use the existing test framework.
4. Run formatting checks, the affected build, and relevant tests after the final code edit.
5. Run wider checks when required by CI or by the change's impact.
6. Inspect the final diff for unintended edits, secrets, generated output, and compatibility changes.
7. Report checks that failed or could not run, and say why. Never present a planned check as completed.

Use sanitizers for relevant memory or concurrency risks when supported.
For performance changes, record the workload, environment, baseline, and result.
State that the task is complete only when the results support this statement.

## Communication

1. Give the result first, then the necessary evidence and limits.
2. Read the relevant skill or procedure when its stated conditions apply.

## Cross-platform C and C++

- Write shared product logic in portable C. Keep OS headers, system calls and platform-specific formats in platform implementations. Keep core headers usable from a C translation unit.
- Use MinGW-w64 as the primary Windows toolchain. Use native toolchains on Linux and macOS. Express platform and compiler differences on the affected CMake targets.
- Configure third-party C and C++ dependencies in this repository. Pin each dependency to a version or revision, verify downloaded archives by hash, and keep fetched sources and build outputs under the checkout. A clean checkout must build without libraries from another project or a developer-specific installation path.
- Give C interfaces explicit ownership, buffer sizes, error results and cleanup rules. Test invalid input and partial failure. Preserve existing CLI behavior and stored data formats when moving a module into the core.
- Establish the C contract and cover it with behavior tests before adding a C++ layer. Use C++ where a specific feature simplifies the implementation. Examples include RAII for resource cleanup and concepts for constraints shared by several callers. Keep exceptions and C++ types inside that layer.
- Build the shared core and run the same behavior tests on Windows, Linux and macOS. Verify platform-specific behavior through the CLI on the affected host. Report which targets actually passed when describing platform support.

## Finish

Report the result, key changes, checks run with outcomes, and remaining limitations.
Claim completion only when the evidence supports the agreed acceptance criteria.
Keep the report concise. Include exact commands when they help reproduce verification.

## Agent skills

### Issue tracker

For tasks, specifications and issue dependencies, follow the [GitHub Issues workflow](docs/agents/issue-tracker.md).

### Triage labels

When triaging issues, use the [project label mapping](docs/agents/triage-labels.md).

### Domain docs

Before exploring a feature or proposing a design, consult the [domain documentation guide](docs/agents/domain.md).
