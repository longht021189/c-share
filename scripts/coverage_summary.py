"""Turns `llvm-cov export -summary-only` (JSON on stdin) into a Markdown table.

    python3 coverage_summary.py <prefix to strip from file names> <title>

Used by scripts/coverage.sh.
"""

import json
import sys


def cell(summary: dict) -> str:
    if summary["count"] == 0:
        return "-"
    return f"{summary['percent']:.1f}% ({summary['covered']}/{summary['count']})"


def main() -> None:
    prefix, title = sys.argv[1], sys.argv[2]
    data = json.load(sys.stdin)["data"][0]

    kinds = [("lines", "Lines"), ("functions", "Functions"), ("branches", "Branches"), ("regions", "Regions")]
    rows = [
        f"### Coverage ({title})",
        "",
        "| File | " + " | ".join(name for _, name in kinds) + " |",
        "|---" * (len(kinds) + 1) + "|",
    ]
    for file in sorted(data["files"], key=lambda f: f["filename"]):
        if file["summary"]["lines"]["count"] == 0:
            continue  # e.g. a C header of declarations only
        name = file["filename"].removeprefix(prefix)
        rows.append(f"| `{name}` | " + " | ".join(cell(file["summary"][k]) for k, _ in kinds) + " |")
    rows.append("| **Total** | " + " | ".join(f"**{cell(data['totals'][k])}**" for k, _ in kinds) + " |")
    print("\n".join(rows))


if __name__ == "__main__":
    main()
