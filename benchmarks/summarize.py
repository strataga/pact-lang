#!/usr/bin/env python3
"""Validate Pact pilot trial records and summarize outcome/time metrics."""
import argparse
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

SCHEMA_VERSION = 1
STATUSES = {
    "correct", "incorrect", "compile_error", "runtime_error", "timeout",
    "aborted",
}
TIMING_FIELDS = {
    "generation", "check_repair", "compile", "tool_wait", "execution",
}
VERSION_FIELDS = {"compiler", "runtime", "language_format"}


class InputError(Exception):
    pass


def load_manifest(path):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise InputError(f"cannot read manifest {path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema_version") != SCHEMA_VERSION:
        raise InputError("manifest schema_version must be 1")
    if not isinstance(data.get("pilot_version"), str):
        raise InputError("manifest pilot_version must be a string")
    if (not isinstance(data.get("languages"), list) or not data["languages"]
            or any(not isinstance(language, str) or not language for language in data["languages"])
            or len(set(data["languages"])) != len(data["languages"])):
        raise InputError("manifest languages must be a non-empty array of unique strings")
    tasks = data.get("tasks")
    if not isinstance(tasks, list) or not tasks:
        raise InputError("manifest tasks must be a non-empty array")
    task_map = {}
    for task in tasks:
        if not isinstance(task, dict) or not isinstance(task.get("task_id"), str):
            raise InputError("each manifest task must have a string task_id")
        task_id = task["task_id"]
        if task_id in task_map:
            raise InputError(f"duplicate task_id in manifest: {task_id}")
        group = task.get("group")
        deadline = task.get("deadline_seconds", data.get("deadline_seconds"))
        if not isinstance(group, str) or not isinstance(deadline, (int, float)) or isinstance(deadline, bool) or deadline <= 0:
            raise InputError(f"task {task_id} needs a group and positive deadline_seconds")
        task_map[task_id] = {"group": group, "deadline_seconds": float(deadline)}
    if data.get("task_count") != len(task_map):
        raise InputError("manifest task_count does not match tasks length")
    return data, task_map


def load_trials(path, manifest, tasks):
    languages = set(manifest["languages"])
    records = []
    seen = set()
    try:
        stream = Path(path).open(encoding="utf-8")
    except OSError as exc:
        raise InputError(f"cannot read trials {path}: {exc}") from exc
    with stream:
        for line_no, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise InputError(f"{path}:{line_no}: invalid JSON: {exc.msg}") from exc
            prefix = f"{path}:{line_no}"
            if not isinstance(record, dict):
                raise InputError(f"{prefix}: trial must be a JSON object")
            if record.get("schema_version") != SCHEMA_VERSION:
                raise InputError(f"{prefix}: schema_version must be 1")
            if record.get("pilot_version") != manifest["pilot_version"]:
                raise InputError(f"{prefix}: pilot_version does not match manifest")
            task_id = record.get("task_id")
            if not isinstance(task_id, str):
                raise InputError(f"{prefix}: task_id must be a string")
            if task_id not in tasks:
                raise InputError(f"{prefix}: unknown task_id {task_id!r}")
            language = record.get("language")
            if not isinstance(language, str):
                raise InputError(f"{prefix}: language must be a string")
            if language not in languages:
                raise InputError(f"{prefix}: language must be one of {sorted(languages)}")
            for field in ("model_id", "model_family"):
                if not isinstance(record.get(field), str) or not record[field].strip():
                    raise InputError(f"{prefix}: {field} must be a non-empty string")
            sample_id = record.get("sample_id")
            if not isinstance(sample_id, int) or isinstance(sample_id, bool) or sample_id <= 0:
                raise InputError(f"{prefix}: sample_id must be a positive integer")
            status = record.get("status")
            if not isinstance(status, str) or status not in STATUSES:
                raise InputError(f"{prefix}: status must be one of {sorted(STATUSES)}")
            elapsed = record.get("elapsed_ms")
            if not isinstance(elapsed, (int, float)) or isinstance(elapsed, bool) or not math.isfinite(elapsed) or elapsed < 0:
                raise InputError(f"{prefix}: elapsed_ms must be a finite non-negative number")
            deadline = tasks[task_id]["deadline_seconds"]
            if record.get("deadline_seconds") != deadline:
                raise InputError(f"{prefix}: deadline_seconds differs from the manifest")
            if elapsed > deadline * 1000:
                raise InputError(f"{prefix}: elapsed_ms exceeds the task deadline")
            timing = record.get("timing_ms")
            if not isinstance(timing, dict) or set(timing) != TIMING_FIELDS:
                raise InputError(f"{prefix}: timing_ms must contain exactly {sorted(TIMING_FIELDS)}")
            for field, value in timing.items():
                if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value) or value < 0:
                    raise InputError(f"{prefix}: timing_ms.{field} must be finite and non-negative")
            phase_total = sum(timing.values())
            tolerance = max(50.0, elapsed * 0.01)
            if abs(phase_total - elapsed) > tolerance:
                raise InputError(
                    f"{prefix}: timing_ms sum ({phase_total:g}) does not match "
                    f"elapsed_ms ({elapsed:g}) within {tolerance:g} ms"
                )
            versions = record.get("versions")
            if not isinstance(versions, dict) or set(versions) != VERSION_FIELDS or any(not isinstance(v, str) or not v for v in versions.values()):
                raise InputError(f"{prefix}: versions must contain non-empty compiler, runtime, language_format strings")
            key = (
                task_id, language, str(record["model_family"]),
                str(record["model_id"]), str(sample_id),
            )
            if key in seen:
                raise InputError(f"{prefix}: duplicate trial key {key}")
            seen.add(key)
            record["_group"] = tasks[task_id]["group"]
            record["_deadline_ms"] = int(deadline * 1000)
            records.append(record)
    if not records:
        raise InputError("trials file contains no records")
    expected_ids = set(tasks)
    combinations = defaultdict(set)
    for record in records:
        combination = (
            record["language"], str(record["model_family"]),
            str(record["model_id"]), str(record["sample_id"]),
        )
        combinations[combination].add(record["task_id"])
    incomplete = []
    for combination, seen_ids in combinations.items():
        missing = sorted(expected_ids - seen_ids)
        if missing:
            incomplete.append((combination, missing))
    if incomplete:
        combination, missing = incomplete[0]
        raise InputError(
            "incomplete matched task set for language/model/family/sample "
            f"{combination}; missing {len(missing)} task(s): {', '.join(missing)}"
        )
    by_model = defaultdict(lambda: defaultdict(set))
    for record in records:
        model_key = (str(record["model_family"]), str(record["model_id"]))
        by_model[model_key][record["language"]].add(record["sample_id"])
    expected_languages = set(languages)
    for model_key, language_samples in by_model.items():
        missing_languages = sorted(expected_languages - set(language_samples))
        if missing_languages:
            raise InputError(
                f"model {model_key} is missing matched language batches: "
                f"{', '.join(missing_languages)}"
            )
        sample_sets = list(language_samples.values())
        if any(samples != sample_sets[0] for samples in sample_sets[1:]):
            raise InputError(
                f"model {model_key} has different sample_id sets across languages"
            )
    return records


def percentile90(values):
    """Nearest-rank 90th percentile (deterministic for small pilot samples)."""
    ordered = sorted(values)
    return ordered[math.ceil(0.9 * len(ordered)) - 1]


def summarize(records):
    buckets = defaultdict(list)
    for record in records:
        key = (record["language"], str(record["model_family"]), str(record["model_id"]), record["_group"])
        buckets[key].append(record)
    output = []
    for (language, family, model, group), bucket in sorted(buckets.items()):
        correct = sum(r["status"] == "correct" for r in bucket)
        deadline_times = [r["elapsed_ms"] if r["status"] == "correct" else r["_deadline_ms"] for r in bucket]
        output.append({
            "language": language,
            "model_family": family,
            "model_id": model,
            "task_group": group,
            "trials": len(bucket),
            "correct": correct,
            "completion_rate": correct / len(bucket),
            "median_time_to_correct_ms": statistics.median(deadline_times),
            "p90_time_to_correct_ms": percentile90(deadline_times),
            # Keep each original failure record so failed runs remain auditable.
            "failures": [
                {k: v for k, v in r.items() if not k.startswith("_")}
                for r in bucket if r["status"] != "correct"
            ],
        })
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trials", help="JSONL file with one trial record per task/model/language/sample")
    parser.add_argument("--manifest", default=str(Path(__file__).with_name("pilot-v1.json")), help="pilot task manifest")
    parser.add_argument("--output", help="write summary JSON to this file instead of stdout")
    args = parser.parse_args(argv)
    try:
        manifest, tasks = load_manifest(args.manifest)
        records = load_trials(args.trials, manifest, tasks)
        report = {
            "schema_version": SCHEMA_VERSION,
            "pilot_version": manifest["pilot_version"],
            "trial_count": len(records),
            "timing_scope": "elapsed_ms is end-to-end from task receipt through the first correct result; non-correct trials are charged their full task deadline",
            "percentile_method": "nearest-rank",
            "groups": summarize(records),
        }
        rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            try:
                Path(args.output).write_text(rendered, encoding="utf-8")
            except OSError as exc:
                raise InputError(f"cannot write summary {args.output}: {exc}") from exc
        else:
            sys.stdout.write(rendered)
    except InputError as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
