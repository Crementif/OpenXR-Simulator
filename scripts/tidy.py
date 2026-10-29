#!/usr/bin/env python3
"""Run clang-format and clang-tidy over the repository's C++ sources (run from the repository root).

  scripts/tidy.py                  format, then run clang-tidy and report warnings
  scripts/tidy.py --check          fail on unformatted files or clang-tidy warnings, change nothing
  scripts/tidy.py --fix            format and apply clang-tidy fix-its
  scripts/tidy.py --format-only    skip clang-tidy
  scripts/tidy.py --changed        limit to files changed against --base (default HEAD), for pre-commit
  scripts/tidy.py FILE...          limit to the given files

clang-tidy uses <build>/compile_commands.json and only checks files in it, so platform files the
build does not compile are formatted but not tidied.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

ROOTS = ("src", "probes", "tests")
EXTENSIONS = (".cpp", ".h", ".hpp")


def tool(name):
    path = shutil.which(name)
    if not path:
        sys.exit(f"{name} not found; install clang-tools-extra")
    return path


def source_files(arguments):
    if arguments.files:
        files = arguments.files
    elif arguments.changed:
        files = subprocess.check_output(["git", "-c", "core.safecrlf=false", "diff", "--name-only", "--diff-filter=d", arguments.base, "--", *ROOTS], text=True).splitlines()
        files += subprocess.check_output(["git", "-c", "core.safecrlf=false", "ls-files", "--others", "--exclude-standard", "--", *ROOTS], text=True).splitlines()
    else:
        files = subprocess.check_output(["git", "-c", "core.safecrlf=false", "ls-files", "--", *ROOTS], text=True).splitlines()
    return sorted({file for file in files if file.endswith(EXTENSIONS) and Path(file).is_file()})


def compile_database(build):
    database = build / "compile_commands.json"
    if not database.is_file():
        subprocess.check_call(["cmake", "-S", ".", "-B", str(build), "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"])
    return database


def format_files(files, check):
    if not files:
        return True
    command = [tool("clang-format"), "--style=file"]
    command += ["--dry-run", "-Werror"] if check else ["-i"]
    return subprocess.call(command + files) == 0


def tidy(files, build, fix, jobs):
    compile_database(build)
    command = [tool("run-clang-tidy"), "-quiet", "-p", str(build), "-j", str(jobs)]
    if fix:
        command += ["-fix", "-format"]
    # run-clang-tidy treats the file arguments as regular expressions over absolute paths.
    command += [str(Path(file).resolve()).replace(".", r"\.") + "$" for file in files]
    result = subprocess.run(command, text=True, capture_output=True)
    warnings = sorted({line for line in result.stdout.splitlines() if ": warning: " in line or ": error: " in line})
    for line in warnings:
        print(line)
    if result.returncode != 0 and not warnings:
        sys.stderr.write(result.stdout + result.stderr)
        return False
    return not warnings


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="*")
    parser.add_argument("--check", action="store_true", help="report problems without changing files")
    parser.add_argument("--fix", action="store_true", help="apply clang-tidy fix-its")
    parser.add_argument("--format-only", action="store_true")
    parser.add_argument("--tidy-only", action="store_true")
    parser.add_argument("--changed", action="store_true", help="only files changed against --base")
    parser.add_argument("--base", default="HEAD")
    parser.add_argument("--build", type=Path, default=Path("build-linux"))
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    arguments = parser.parse_args()
    if arguments.check and arguments.fix:
        parser.error("--check and --fix are exclusive")

    files = source_files(arguments)
    ok = True
    if not arguments.tidy_only:
        ok = format_files(files, arguments.check) and ok
    if not arguments.format_only and files:
        ok = tidy(files, arguments.build, arguments.fix, arguments.jobs) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
