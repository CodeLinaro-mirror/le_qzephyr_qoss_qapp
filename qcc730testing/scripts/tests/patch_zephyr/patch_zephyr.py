#!/usr/bin/env python3

# Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
# SPDX-License-Identifier: Apache-2.0

"""
Script to copy overlays, patch Zephyr tests and samples, and generate patches.
"""

import sys
import os
import subprocess
from pathlib import Path
import shutil

# File and Directory Paths
SCRIPT_DIR = Path(__file__).resolve().parent
QCC730_ROOT_PATH = SCRIPT_DIR.parent.parent.parent.resolve()
WORKSPACE_ROOT_PATH = QCC730_ROOT_PATH.parent.parent.resolve()
ZEPHYR_PATH = WORKSPACE_ROOT_PATH / "zephyr"
QCC730_TESTS = QCC730_ROOT_PATH / "tests"
QCC730_SAMPLES = QCC730_ROOT_PATH / "samples"


def usage():
    """Print usage help"""
    print(f"Usage: {sys.argv[0]} [command]")
    print("\nAvailable Commands:")
    print("  rm        Deletes all local changes from the zephyr repository.")
    print("  inst      Installs overlays and applies all patches to zephyr.")
    print("  gen       Generates .patch files from *staged* changes in zephyr's tests and samples.")
    print("  help      Shows this help message.")
    print("\nExamples:")
    print(f"  {sys.argv[0]} rm         # Clean the zephyr repository")
    print(f"  {sys.argv[0]} inst       # Install patches to zephyr")
    print(
        f"  {
            sys.argv[0]} gen        # Generate new patches from your staged changes")
    print(f"  {sys.argv[0]} rm inst    # First clean up and then install")
    sys.exit(1)


def run(cmd, cwd=None, check=True, capture_output=False, text=False):
    """"Run command in a certain directory"""
    print(f"Running: {' '.join(cmd)} (in {cwd or os.getcwd()})")
    return subprocess.run(
        cmd,
        cwd=cwd,
        check=check,
        capture_output=capture_output,
        text=text)


def remove():
    """Reset all changes and clean untracked files in Zephyr repo"""
    run(['git', 'reset', '--hard'], cwd=ZEPHYR_PATH)
    run(['git', 'clean', '-fd'], cwd=ZEPHYR_PATH)


def patch(zephyr_path: Path, patch_path: Path):
    """Copy overlays and apply patches to tests and samples"""
    for overlay in patch_path.rglob('*'):
        if overlay.is_file() and overlay.suffix not in ('.patch', '.md'):
            rel_path = overlay.relative_to(patch_path.parent)
            dest = zephyr_path / rel_path
            dest.parent.mkdir(parents=True, exist_ok=True)
            print(f"Copying {overlay} -> {dest}")
            shutil.copy2(overlay, dest)

    # Apply patches
    for pat in patch_path.glob('**/*.patch'):
        run(['git', 'apply', str(pat)], cwd=zephyr_path)


def generate_patches():
    """
    Generates artifacts from STAGED files in the Zephyr repository.
    - For MODIFIED files in 'tests' and 'samples', it creates .patch files.
    - For NEW files in 'tests' and 'samples', it copies.
    """
    print("Checking for STAGED files in Zephyr repository...")
    cmd = ['git', 'diff', '--name-status', '--cached']
    result = run(
        cmd,
        cwd=ZEPHYR_PATH,
        check=True,
        capture_output=True,
        text=True)
    staged_files_output = result.stdout.strip().split('\n')

    if not staged_files_output or not staged_files_output[0]:
        print("No staged files found. Nothing to do.")
        return

    target_dirs = [QCC730_TESTS.name, QCC730_SAMPLES.name]
    generated_count = 0
    copied_count = 0

    for line in staged_files_output:
        if not line:
            continue

        status, file_rel_path_str = line.split('\t', 1)
        file_rel_path = Path(file_rel_path_str)

        if not file_rel_path.parts or file_rel_path.parts[0] not in target_dirs:
            continue

        print(f"Found staged file: {file_rel_path_str} (Status: {status})")

        if status == 'A':  # Added
            source_file = ZEPHYR_PATH / file_rel_path
            dest_file = QCC730_ROOT_PATH / file_rel_path

            print(f"  -> Copying new file to: {dest_file}")
            dest_file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_file, dest_file)
            copied_count += 1

        elif status == 'M':  # Modified
            dest_patch_path = QCC730_ROOT_PATH / (file_rel_path_str + '.patch')
            dest_patch_path.parent.mkdir(parents=True, exist_ok=True)

            patch_cmd = ['git', 'diff', '--cached', '--', file_rel_path_str]
            patch_content_result = run(
                patch_cmd,
                cwd=ZEPHYR_PATH,
                check=True,
                capture_output=True,
                text=True)
            patch_content = patch_content_result.stdout

            if patch_content:
                print(f"  -> Creating patch: {dest_patch_path}")
                with open(dest_patch_path, 'w', encoding='utf-8') as f:
                    f.write(patch_content)
                generated_count += 1
            else:
                print(
                    f"  -> No content change for {file_rel_path_str}, skipping patch generation.")
        else:
            print(
                f"  -> Unhandled status '{status}' for {file_rel_path_str}, skipping.")

    if generated_count > 0:
        print(f"\nSuccessfully generated {generated_count} patch file(s).")
    if copied_count > 0:
        print(f"Successfully copied {copied_count} new file(s).")
    if generated_count == 0 and copied_count == 0:
        print("\nNo patches were generated or files copied for the staged items.")


def main():
    """Main function"""
    args = sys.argv[1:]

    if not args or any(arg in ('help', '-h', '--help') for arg in args):
        usage()

    for arg in args:
        if arg == "rm":
            remove()
        elif arg == "inst":
            patch(ZEPHYR_PATH, QCC730_TESTS)
            patch(ZEPHYR_PATH, QCC730_SAMPLES)
        elif arg == "gen":
            generate_patches()
        else:
            print(f"Invalid parameter: {arg}", file=sys.stderr)
            sys.exit(2)

    print("\nDone.")


if __name__ == "__main__":
    main()
