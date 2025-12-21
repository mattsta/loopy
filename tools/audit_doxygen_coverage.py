#!/usr/bin/env python3
"""
Doxygen Documentation Coverage Audit for loopy headers - CORRECTED & OPTIMIZED

This script audits all public header files to verify Doxygen documentation
coverage on function prototypes. Can be integrated into CI/CD.

The detection algorithm:
1. Scans the file sequentially line by line
2. Tracks whether we're inside a /** */ Doxygen block
3. When a function declaration is found, checks if it was preceded by an
   unclosed Doxygen block
4. Handles multi-line comments correctly with proper state tracking

Usage:
  ./audit_doxygen_coverage.py [--fail-on-missing] [--full] [--verbose]

Options:
  --fail-on-missing   Exit with code 1 if any undocumented functions found
  --full              Show full per-file breakdown (default: only missing)
  --verbose           Show detailed output for each function
"""

import re
import os
import sys
import argparse
from pathlib import Path
from typing import Dict, List, Tuple, Optional


class DoxygenAudit:
    """Correctly audits Doxygen documentation coverage"""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self.results = {}

    def extract_function_name(self, decl_line: str) -> Optional[str]:
        """Extract function name from a declaration line"""
        match = re.search(r"\b([a-zA-Z_][a-zA-Z0-9_]*)\s*\(", decl_line)
        if match:
            return match.group(1)
        return None

    def is_function_declaration(self, line: str) -> bool:
        """Check if a line is a function declaration"""
        stripped = line.strip()

        if not stripped.endswith(");"):
            return False

        if any(x in stripped for x in ["typedef", "#define", "#include"]):
            return False

        if stripped.startswith("#"):
            return False

        if stripped.startswith("//") or stripped.startswith("*"):
            return False

        if "(" not in stripped:
            return False

        if not re.match(r"^[a-zA-Z_*\s]", stripped):
            return False

        return True

    def analyze_header(self, filepath: str) -> Tuple[int, int, List[Tuple[str, int]]]:
        """
        Analyze a header file for Doxygen documentation coverage.

        Returns:
            (total_functions, documented_functions, list_of_undocumented_with_line_numbers)
        """
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.readlines()

        total_functions = 0
        documented_functions = 0
        undocumented = []

        # State tracking
        in_doxygen_block = False
        last_doxygen_ended_line = -1
        brace_depth = 0  # Track depth inside function bodies

        for line_num, line in enumerate(lines):
            stripped = line.strip()

            # Track brace depth to skip content inside static inline function bodies
            brace_depth += line.count('{') - line.count('}')
            if brace_depth < 0:
                brace_depth = 0

            # Track Doxygen blocks
            if "/**" in stripped:
                in_doxygen_block = True

            if in_doxygen_block and "*/" in stripped:
                in_doxygen_block = False
                last_doxygen_ended_line = line_num
                continue

            if in_doxygen_block:
                continue

            if not stripped or stripped.startswith("//") or stripped.startswith("*"):
                continue

            # Skip function calls inside function bodies
            if brace_depth > 0:
                continue

            if self.is_function_declaration(line):
                func_name = self.extract_function_name(stripped)

                if func_name:
                    total_functions += 1

                    is_documented = (
                        last_doxygen_ended_line >= 0
                        and last_doxygen_ended_line < line_num
                    )

                    if is_documented:
                        documented_functions += 1
                        if self.verbose:
                            print(f"  ✓ {func_name:30} (line {line_num + 1})")
                    else:
                        undocumented.append((func_name, line_num + 1))
                        if self.verbose:
                            print(f"  ✗ {func_name:30} (line {line_num + 1})")

                    last_doxygen_ended_line = -1

        return total_functions, documented_functions, undocumented

    def run(self, search_dir: str = "src") -> Dict[str, Dict]:
        """Audit all header files in directory"""
        src_dir = Path(search_dir)
        if not src_dir.exists():
            print(f"Error: Directory {search_dir} does not exist")
            sys.exit(1)

        headers = sorted(src_dir.glob("loopy*.h"))

        if not headers:
            print(f"Error: No loopy*.h files found in {search_dir}")
            sys.exit(1)

        for header in headers:
            if self.verbose:
                print(f"\nAnalyzing {header.name}...")

            total, documented, undocumented = self.analyze_header(str(header))

            self.results[header.name] = {
                "total": total,
                "documented": documented,
                "undocumented": undocumented,
                "coverage": (documented / total * 100) if total > 0 else 100.0,
                "filepath": str(header),
            }

        return self.results


def print_report_missing_only(results: Dict[str, Dict]) -> bool:
    """Print ONLY the missing documentation (compact mode). Returns True if complete."""

    incomplete = {f: info for f, info in results.items() if info["coverage"] < 100}

    total_funcs = sum(info["total"] for info in results.values())
    total_docs = sum(info["documented"] for info in results.values())
    missing_count = total_funcs - total_docs

    if not incomplete:
        print("\n" + "=" * 80)
        print("✓✓✓ ALL FILES HAVE 100% DOCUMENTATION COVERAGE! ✓✓✓")
        print("=" * 80)
        print(
            f"\nTotal: {total_docs}/{total_funcs} functions documented ({100 if total_funcs > 0 else 0:.1f}%)"
        )
        print("\n" + "=" * 80 + "\n")
        return True

    print("\n" + "=" * 80)
    print("DOXYGEN DOCUMENTATION COVERAGE - MISSING ITEMS")
    print("=" * 80)

    for filename in sorted(incomplete.keys()):
        info = incomplete[filename]
        missing_list = info["undocumented"]
        coverage = info["coverage"]

        print(f"\n{filename} ({coverage:.1f}% - {len(missing_list)} missing):")
        for func_name, line_num in missing_list:
            print(f"  - {func_name:40} (line {line_num})")

    print("\n" + "=" * 80)
    print("SUMMARY:")
    print("=" * 80)
    print(f"Total functions: {total_funcs}")
    print(f"Documented: {total_docs}")
    print(f"Missing: {missing_count}")
    print(
        f"Coverage: {(total_docs / total_funcs * 100) if total_funcs > 0 else 100:.1f}%"
    )
    print(f"Incomplete files: {len(incomplete)}/{len(results)}")
    print("\n" + "=" * 80 + "\n")

    return len(incomplete) == 0


def print_report_full(results: Dict[str, Dict]) -> bool:
    """Print full detailed report with all files"""

    print("\n" + "=" * 80)
    print("DOXYGEN DOCUMENTATION COVERAGE AUDIT - FULL REPORT")
    print("=" * 80)

    print("\nPER-FILE SUMMARY:")
    print("-" * 80)

    total_funcs = 0
    total_docs = 0

    for filename in sorted(results.keys()):
        info = results[filename]
        total = info["total"]
        documented = info["documented"]
        missing_list = info["undocumented"]
        coverage = info["coverage"]

        total_funcs += total
        total_docs += documented

        status = "✓ COMPLETE" if coverage == 100 else "✗ INCOMPLETE"

        print(f"\nFILE: {filename}")
        print(f"  Status: {status}")
        print(f"  Total functions: {total}")
        print(f"  With Doxygen docs: {documented}")
        print(f"  WITHOUT docs (MISSING): {len(missing_list)}")
        print(f"  Coverage: {coverage:.1f}%")

        if missing_list:
            print(f"  Missing documentation for:")
            for func_name, line_num in missing_list:
                print(f"    * {func_name:30} (line {line_num})")

    # Overall summary
    print("\n" + "=" * 80)
    print("OVERALL SUMMARY:")
    print("=" * 80)
    print(f"Total functions across all headers: {total_funcs}")
    print(f"Total with docs: {total_docs}")
    print(f"Total missing: {total_funcs - total_docs}")

    if total_funcs > 0:
        coverage = total_docs / total_funcs * 100
        print(f"Overall coverage: {coverage:.1f}%")

    # Files with incomplete coverage
    incomplete = [f for f in sorted(results.keys()) if results[f]["coverage"] < 100]

    if incomplete:
        print(f"\n{len(incomplete)} files with INCOMPLETE coverage (< 100%):")
        for filename in incomplete:
            info = results[filename]
            missing_count = len(info["undocumented"])
            print(f"  - {filename} ({info['coverage']:.1f}% - {missing_count} missing)")
    else:
        print("\n✓✓✓ ALL FILES HAVE 100% DOCUMENTATION COVERAGE! ✓✓✓")

    print("\n" + "=" * 80 + "\n")

    return len(incomplete) == 0


def main():
    parser = argparse.ArgumentParser(
        description="Audit Doxygen documentation coverage on function prototypes"
    )
    parser.add_argument(
        "--fail-on-missing",
        action="store_true",
        help="Exit with code 1 if any undocumented functions found",
    )
    parser.add_argument(
        "--full",
        action="store_true",
        help="Show full per-file breakdown (default: only show missing items)",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Show detailed output for each function",
    )
    parser.add_argument(
        "--dir", default="src", help="Directory to scan for headers (default: src)"
    )

    args = parser.parse_args()

    # Resolve directory path
    search_dir = Path(args.dir)
    if not search_dir.exists():
        parent_search = Path("..") / args.dir
        if parent_search.exists():
            search_dir = parent_search
        else:
            print(f"Error: Directory {args.dir} not found")
            sys.exit(1)

    audit = DoxygenAudit(verbose=args.verbose)
    results = audit.run(search_dir=str(search_dir))

    # Print appropriate report
    if args.full:
        success = print_report_full(results)
    else:
        success = print_report_missing_only(results)

    if args.fail_on_missing:
        sys.exit(0 if success else 1)
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
