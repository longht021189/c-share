"""Looks up known vulnerabilities in the vcpkg dependencies a build installed.

    python3 scripts/check_dependencies.py <build dir> [--sarif <file>] [--markdown <file>]

vcpkg writes an SBOM (vcpkg.spdx.json) for each package it installs, naming
the upstream repository and tag (or commit) it built. Each tag is resolved
to its commit, and OSV (https://osv.dev) is asked which vulnerabilities that
commit is affected by. Asking by commit is what works for C/C++: OSV has no
vcpkg ecosystem, and vcpkg's SBOMs carry no PURL or CPE that generic SBOM
scanners (osv-scanner, Trivy, Grype) could match.

Prints a table, and optionally writes the findings as SARIF (for GitHub's
code scanning) and as Markdown. Exits 1 if any dependency has a known
vulnerability, 2 if the lookup itself failed.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import urllib.request

OSV_API = "https://api.osv.dev/v1"
GIT_SOURCE = re.compile(r"git\+(https://github\.com/[^@]+)@(.+)")
COMMIT = re.compile(r"[0-9a-f]{40}")
# SARIF's security-severity, for advisories that rate themselves in words.
SEVERITY_SCORES = {"critical": 9.5, "high": 8.0, "moderate": 5.5, "medium": 5.5, "low": 3.0}


def post(path: str, body: dict) -> dict:
    request = urllib.request.Request(
        f"{OSV_API}/{path}", json.dumps(body).encode(), {"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        result = json.load(response)
    return result


def get(path: str) -> dict:
    with urllib.request.urlopen(f"{OSV_API}/{path}", timeout=60) as response:
        result = json.load(response)
    return result


def find_dependencies(build_dir: str) -> list:
    """[{port, version, repo, ref}] from the build's vcpkg SBOMs."""
    dependencies = []
    for sbom in sorted(glob.glob(os.path.join(build_dir, "vcpkg_installed", "*", "share", "*", "vcpkg.spdx.json"))):
        port = os.path.basename(os.path.dirname(sbom))
        with open(sbom, encoding="utf-8") as f:
            packages = json.load(f)["packages"]
        version = next((p.get("versionInfo") for p in packages if p.get("name") == port), None)
        for package in packages:
            match = GIT_SOURCE.match(package.get("downloadLocation", ""))
            if match and "/Microsoft/vcpkg" not in match[1]:
                dependencies.append({"port": port, "version": version or match[2], "repo": match[1], "ref": match[2]})
    return dependencies


def resolve_commit(repo: str, ref: str) -> str:
    if COMMIT.fullmatch(ref):
        return ref
    # An annotated tag's own object comes first, the commit it points to (^{}) last.
    out = subprocess.run(
        ["git", "ls-remote", repo, f"refs/tags/{ref}", f"refs/tags/{ref}^{{}}"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    if not out:
        raise RuntimeError(f"{repo} has no tag {ref}")
    return out[-2]


def severity(vuln: dict) -> tuple:
    """(label, SARIF security-severity or None)."""
    label = (vuln.get("database_specific") or {}).get("severity")
    if isinstance(label, str) and label.lower() in SEVERITY_SCORES:
        return label.capitalize(), SEVERITY_SCORES[label.lower()]
    vectors = [s["score"] for s in vuln.get("severity", []) if s.get("score")]
    return (vectors[0] if vectors else "unrated"), None


def fixed_versions(vuln: dict) -> list:
    return sorted({
        event["fixed"]
        for affected in vuln.get("affected", [])
        for r in affected.get("ranges", [])
        if r.get("type") != "GIT"
        for event in r.get("events", [])
        if "fixed" in event
    })


def manifest_location(root: str, port: str) -> tuple:
    """(path, line) to point an alert at: the overlay port's version, else the manifest.

    Both are in vcpkg-share (VCPKG_SHARE_PATH, else next to this checkout); the
    path is relative to root, as on CI, where vcpkg-share is checked out inside it.
    """
    share = os.environ.get("VCPKG_SHARE_PATH") or os.path.join(root, "..", "vcpkg-share")
    overlay = os.path.join(share, "ports", port, "vcpkg.json")
    if os.path.exists(overlay):
        full, needle = overlay, '"version'
    else:
        full, needle = os.path.join(share, "vcpkg.json"), f'"{port}"'
    path = os.path.relpath(full, root)
    with open(full, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            if needle in line:
                return path, number
    return path, 1


def sarif(root: str, findings: list) -> dict:
    rules, results = {}, []
    for dep, vuln in findings:
        label, score = severity(vuln)
        fixed = fixed_versions(vuln)
        summary = vuln.get("summary") or vuln.get("details", "").split("\n")[0][:200]
        rule = {
            "id": vuln["id"],
            "shortDescription": {"text": f"{dep['port']}: {summary}"},
            "fullDescription": {"text": vuln.get("details") or summary},
            "helpUri": f"https://osv.dev/vulnerability/{vuln['id']}",
            "help": {"text": f"{summary}\nSeverity: {label}\nFixed in: {', '.join(fixed) or 'see OSV'}"},
            "properties": {"tags": ["security", "dependency", dep["port"]]},
        }
        if score is not None:
            rule["properties"]["security-severity"] = str(score)
        rules[vuln["id"]] = rule
        path, line = manifest_location(root, dep["port"])
        results.append({
            "ruleId": vuln["id"],
            "level": "error" if (score or 0) >= 7 else "warning",
            "message": {"text": f"{dep['port']} {dep['version']} ({dep['ref']}) is affected by {vuln['id']}: "
                                f"{summary}. Severity: {label}. Fixed in: {', '.join(fixed) or 'see OSV'}."},
            "locations": [{"physicalLocation": {"artifactLocation": {"uri": path}, "region": {"startLine": line}}}],
            "partialFingerprints": {"dependency": f"{dep['port']}/{vuln['id']}"},
        })
    return {
        "$schema": "https://json.schemastore.org/sarif-2.1.0.json",
        "version": "2.1.0",
        "runs": [{
            "tool": {"driver": {"name": "c-share dependency check", "informationUri": "https://osv.dev",
                                "rules": list(rules.values())}},
            "results": results,
        }],
    }


def markdown(dependencies: list, findings: list) -> str:
    rows = ["### Dependency vulnerabilities (OSV)", "", "| Dependency | Source | Known vulnerabilities |", "|---|---|---|"]
    for dep in dependencies:
        ids = [f"[{v['id']}](https://osv.dev/vulnerability/{v['id']}) ({severity(v)[0]})"
               for d, v in findings if d is dep]
        rows.append(f"| {dep['port']} {dep['version']} | `{dep['ref']}` | {', '.join(ids) or 'none'} |")
    return "\n".join(rows) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("build_dir", help="a configured build, e.g. build/linux")
    parser.add_argument("--sarif", help="write the findings as SARIF here")
    parser.add_argument("--markdown", help="write the summary table as Markdown here")
    args = parser.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    dependencies = find_dependencies(args.build_dir)
    if not dependencies:
        print(f"check_dependencies: no vcpkg SBOMs under {args.build_dir}/vcpkg_installed; "
              "configure the build first", file=sys.stderr)
        return 2
    try:
        commits = [resolve_commit(d["repo"], d["ref"]) for d in dependencies]
        results = post("querybatch", {"queries": [{"commit": c} for c in commits]})["results"]
        findings = [(dep, get(f"vulns/{v['id']}"))
                    for dep, result in zip(dependencies, results)
                    for v in result.get("vulns", [])]
    except Exception as error:  # noqa: BLE001 - any failure to look up is reported the same way
        print(f"check_dependencies: lookup failed: {error}", file=sys.stderr)
        return 2

    for dep in dependencies:
        mine = [v for d, v in findings if d is dep]
        print(f"{dep['port']} {dep['version']} ({dep['ref']}): {len(mine)} known vulnerabilities")
        for vuln in mine:
            fixed = ", ".join(fixed_versions(vuln)) or "see OSV"
            summary = vuln.get("summary") or vuln.get("details", "").split("\n")[0][:100]
            print(f"  {vuln['id']} [{severity(vuln)[0]}] {summary} (fixed in: {fixed})")

    if args.sarif:
        with open(args.sarif, "w", encoding="utf-8") as f:
            json.dump(sarif(root, findings), f, indent=2)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(markdown(dependencies, findings))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
