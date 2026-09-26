# Pact benchmark pilot v1

This directory contains the public task set in `pilot-v1.json`. The separate
evaluator-only key is stored at `../benchmark-private/pilot-v1-answer-key.json`.
Do not put that key in model context, adapter prompts, or participant bundles;
exclude `benchmark-private/` when packaging participant materials. It contains
exact stdout checks for all 24 tasks. The manifest has eight generation, eight
editing, and eight repair tasks, each with a 300-second deadline and a
language-neutral prompt. Editing and repair tasks provide equivalent
`starter_sources` for Rust, Python, strict TypeScript, Pact text, and Pact JSON.
Generation tasks set `starter_sources` to null.

## Running a matched comparison

For each model, language, sample, and task, provide the same prompt and the
matching `starter_sources` entry where present. Record one trial row for every
task in every model/language/sample batch. The summarizer requires complete 24-task
batches for all five languages and identical sample ID sets per model; it rejects
unmatched records. Use the same model configuration, sampling settings,
300-second task budget, retry policy, and tool policy across the matched
languages. Provide equally scoped language reference documentation and the same
typed tool contract in each condition, and record their versions. Keep the exposed tools disabled except for the marked
`math.double` tasks; provide the same signed-i64 wrapping doubling behavior in
each language adapter. The language-specific starter is pre-task material and
is supplied before timing starts.

Use strict TypeScript with `bigint` for integer operations (an ES2020 or newer
target). Python/Rust/TypeScript adapters should preserve Pact's signed-i64
wrapping arithmetic where a task exercises it. Pact JSON starters use the
version-1 JSON AST. Pact text and JSON programs are compiled or run with the
repository's current Pact frontend/runtime. The public task prompt remains
identical across all five languages.

## Trial JSONL record contract

Each non-empty JSONL line is one JSON object with these required fields:

| Field | Contract |
| --- | --- |
| `schema_version` | Integer `1`. |
| `pilot_version` | String `"pilot-v1"`. |
| `task_id` | One ID from the manifest. |
| `language` | One of `rust`, `python`, `typescript-strict`, `pact-text`, `pact-json`. |
| `model_id` | Stable exact model identifier used for the trial. |
| `model_family` | Stable family/provider grouping label. |
| `sample_id` | Positive integer identifying a repeated sample. |
| `status` | One of `correct`, `incorrect`, `compile_error`, `runtime_error`, `timeout`, `aborted`. `correct` is assigned only after the evaluator verifies the answer check. |
| `elapsed_ms` | Finite non-negative wall time in milliseconds for the timing scope below. |
| `deadline_seconds` | Must match that task's manifest deadline. |
| `timing_ms` | Object with finite non-negative `generation`, `check_repair`, `compile`, `tool_wait`, and `execution` durations in milliseconds. |
| `versions` | Object with non-empty string fields `compiler`, `runtime`, and `language_format`. Use an explicit value such as `"none"` when a component does not apply. |

The `details` and `raw_failure` fields are optional. They may include evaluator
diagnostics, captured output, source artifacts, compiler/runtime error text, or
other failure evidence. Keep the complete failed trial record in reports so
failures can be audited. Do not put answer-key expected values in public trial
metadata.

`elapsed_ms` uses the scope `prompt_to_verified_result`: start when the model is
shown the task prompt and its language-matched starter; stop when the final
candidate has been checked, compiled if needed, executed, and its result
captured. It includes generation, correction/check-repair loops, compilation,
tool waiting, and execution. `timing_ms` breaks that wall duration into
exclusive phases; the summarizer rejects records when their sum differs from
`elapsed_ms` by more than the larger of 50 ms or 1% of elapsed time. Exclude installation, harness startup, and preparation of the
language-specific starter. If the result is not correct before the deadline,
the summary charges that trial the full per-task deadline regardless of its
recorded elapsed time. Completion rate is verified-correct rows divided by
rows in the evaluated task batch. Median and p90 time-to-correct use the
deadline-charged duration for every failed row.

## Version recording

Record the actual toolchain/runtime versions used for each row. Suggested
`language_format` identifiers are Rust edition plus relevant crate/API
contract, Python language version, TypeScript strict configuration and target,
`pact-text-v1`, and `pact-json-v1`. For Pact JSON, the manifest's AST has
`version: 1`; record the Pact compiler and runtime revisions independently.
Version fields are part of trial provenance and must not be inferred later from
the current checkout.

## Pilot limits

This pilot has no program input: every task computes one deterministic result
and checks stdout. It therefore measures a narrow expression-generation,
editing, and repair slice, not general programming ability or interactive
work. The task set, language-specific starter translations, result checks, and
tool parity need human review before drawing conclusions. Pact currently has
only i64 values, boolean comparisons, conditionals, and the optional mock
`math.double` capability available to these tasks. No measurements have been
collected or performance claims are made here.
