## Graphify

This project has a Graphify knowledge graph at `graphify-out/`.

### HARD GATE — Graphify first

If `graphify-out/graph.json` exists, Graphify MUST be the first repository-orientation tool used for any task involving:

- repository or architecture understanding;
- multiple files, modules, or symbols;
- dependencies or cross-file relationships;
- call flow, data flow, or control flow;
- unfamiliar-code exploration;
- repository-wide implementation, review, audit, refactor, debugging, or verification.

Repository-wide code reviews and audits ALWAYS qualify for this Graphify-first gate when `graphify-out/graph.json` exists.

Before the required Graphify step, DO NOT use broad `grep`, `rg`, `find`, recursive listings, mass file reads, directory-by-directory exploration, or equivalent repository-discovery methods.

Use the narrowest applicable Graphify command:

- `graphify query "<question>"`
- `graphify explain "<concept>"`
- `graphify path "<A>" "<B>"`

A detailed or highly specific user prompt does NOT waive this requirement.

Use Graphify output to identify the smallest relevant set of files and symbols, then inspect the actual source files as needed.

For Graphify-specific workflows or syntax beyond simple `query`, `explain`, or `path`, use the installed `graphify` skill as the authoritative instructions. Do not invent Graphify commands.

### Allowed bypass

Skip Graphify-first ONLY when one of these conditions is objectively true:

1. `graphify-out/graph.json` does not exist;
2. Graphify is unavailable or the attempted Graphify command fails;
3. the graph is known to be stale for code relevant to the task;
4. the user explicitly requests inspection of specific named files or symbols and no broader repository understanding is required;
5. an exact implementation detail must be verified after Graphify has already narrowed the relevant scope.

Do NOT infer condition 4 merely because the prompt is detailed, prescriptive, or already names technologies, subsystems, requirements, or expected behavior.

If Graphify is skipped, unavailable, stale, or fails, state the applicable reason before broader repository exploration.

If broad repository exploration was started before satisfying this gate, STOP that exploration, run the required Graphify step, and then continue from the narrowed scope.

### Graph navigation

If `graphify-out/wiki/index.md` exists, prefer it for broad navigation instead of browsing raw source files.

Read `graphify-out/GRAPH_REPORT.md` only for broad architecture review or when `graphify query`, `graphify explain`, or `graphify path` do not provide enough context.

Do not read the entire `graphify-out/graph.json` into context unless there is a specific, justified reason to do so.

### Verification

Graphify is an orientation and relationship tool, not a substitute for reading the actual implementation.

Verify important implementation details against the source files identified by Graphify before modifying code or making correctness claims.

Do not treat inferred graph relationships as proof of runtime behavior.

If relevant code changed after the graph was last updated, refresh the graph before relying on it for subsequent cross-file or repository-wide reasoning.

### Graph maintenance

After completing a coherent batch of code changes, run:

`graphify update .`

before using the graph again or before final repository-wide verification.

If installed Graphify Git hooks have already updated the graph successfully for those changes, do not perform a redundant update.
