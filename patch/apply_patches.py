#!/usr/bin/env python3

# Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Script to apply patches from patches directory to Zephyr and modules/lib/hostap.
This script is designed to be called before west build to apply necessary patches.
Supports both Windows and Linux/Ubuntu platforms.
"""

import sys
import os
import subprocess
from pathlib import Path

# File and Directory Paths
SCRIPT_DIR = Path(__file__).resolve().parent
WORKSPACE_ROOT_PATH = SCRIPT_DIR.parent.parent.resolve()

# Per-repo configuration.  Each entry describes one target repository:
#   label       - human-readable name used in log output
#   repo_path   - root of the git repository to patch
#   patches_dir - directory containing *.patch files for this repo
#   marker_file - file written after patches are successfully applied
REPO_CONFIGS = [
    {
        "label": "zephyr",
        "repo_path": WORKSPACE_ROOT_PATH / "zephyr",
        "patches_dir": SCRIPT_DIR,              # flat *.patch files in qapp/patch/
        "marker_file": WORKSPACE_ROOT_PATH / "zephyr" / ".patches_applied",
    },
    {
        "label": "hostap",
        "repo_path": WORKSPACE_ROOT_PATH / "modules" / "lib" / "hostap",
        "patches_dir": SCRIPT_DIR / "hostap",   # qapp/patch/hostap/*.patch
        "marker_file": WORKSPACE_ROOT_PATH / "modules" / "lib" / "hostap" / ".patches_applied",
    },
]


def run(cmd, cwd=None, check=True, capture_output=False, text=False):
    """Run a shell command, optionally capturing output."""
    print(f"Running: {' '.join(cmd)} (in {cwd or os.getcwd()})")

    kwargs = {
        'cwd': cwd,
        'check': check,
        'universal_newlines': text,
    }
    if capture_output:
        kwargs['stdout'] = subprocess.PIPE
        kwargs['stderr'] = subprocess.PIPE

    return subprocess.run(cmd, **kwargs)


# ---------------------------------------------------------------------------
# Marker-file helpers (all take explicit paths — no globals)
# ---------------------------------------------------------------------------

def _check_if_patches_applied(marker_file):
    """Return list of raw lines from marker file, or [] if absent."""
    if marker_file.exists():
        with open(marker_file, 'r') as f:
            content = f.read().strip()
            if content:
                return [p for p in content.split('\n') if p]
    return []


def _get_patch_checksum(patch_file):
    """MD5 checksum of a patch file."""
    import hashlib
    with open(patch_file, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()


def _mark_patches_applied(marker_file, patch_files):
    """Write (or overwrite) the marker file with name:checksum entries."""
    from datetime import datetime
    with open(marker_file, 'w') as f:
        f.write("# Patches Applied - DO NOT EDIT MANUALLY\n")
        f.write(f"# Applied on: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write("# Format: patch_filename:checksum\n")
        for patch_file in patch_files:
            checksum = _get_patch_checksum(patch_file)
            f.write(f"{patch_file.name}:{checksum}\n")
    print(f"Patch marker file created: {marker_file}")


def _validate_applied_patches(marker_file, patches_dir):
    """Verify that every recorded patch still matches its on-disk checksum.

    Returns (is_valid: bool, invalid_list: list[str]).
    """
    applied = _check_if_patches_applied(marker_file)
    if not applied:
        return True, []

    invalid = []
    for entry in applied:
        if ':' not in entry or entry.startswith('#'):
            continue
        name, expected = entry.split(':', 1)
        patch_file = patches_dir / name
        if not patch_file.exists():
            invalid.append(f"{name} (file missing)")
            continue
        if _get_patch_checksum(patch_file) != expected:
            invalid.append(f"{name} (checksum mismatch)")

    return len(invalid) == 0, invalid


def _remove_marker(marker_file):
    if marker_file.exists():
        marker_file.unlink()
        print(f"Patch marker file removed: {marker_file}")


# ---------------------------------------------------------------------------
# Core per-repo operations
# ---------------------------------------------------------------------------

def _apply_patches_to_repo(label, repo_path, patches_dir, marker_file, force=False):
    """Apply all *.patch files in patches_dir to repo_path.

    Returns True on full success, False on any failure.
    """
    print(f"\n{'='*60}")
    print(f"  Repo   : {label}  ({repo_path})")
    print(f"  Patches: {patches_dir}")
    print(f"{'='*60}")

    if not patches_dir.exists():
        print(f"Patches directory not found: {patches_dir}")
        return False

    patch_files = sorted(patches_dir.glob('*.patch'))
    if not patch_files:
        print(f"No patch files found in {patches_dir}")
        return True  # nothing to do — not an error

    # Skip if already applied (unless forced)
    if not force:
        applied = _check_if_patches_applied(marker_file)
        if applied:
            is_valid, invalid = _validate_applied_patches(marker_file, patches_dir)
            if is_valid:
                print(f"Patches already applied and valid. Use 'force' to reapply.")
                print(f"Applied: {len([p for p in applied if not p.startswith('#')])}")
                return True
            else:
                print(f"\u26a0\ufe0f  Applied patches have integrity issues:")
                for item in invalid:
                    print(f"  - {item}")
                print("Use 'force' to reapply or 'revert' to clean up.")
                return False

    if force:
        revert_patches()

    print(f"Found {len(patch_files)} patch file(s) to apply:")
    for i, pf in enumerate(patch_files, 1):
        print(f"  {i:02d}. {pf.name}")

    # Pre-validate all patches before touching the repo
    print(f"\n\U0001f50d Pre-validating all patches...")
    validation_failed = False
    for pf in patch_files:
        result = run(
            ['git', 'apply', '--check', '--ignore-whitespace', str(pf)],
            cwd=repo_path,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print(f"  \u274c {pf.name}: Cannot be applied")
            print(f"     {result.stderr.strip()}")
            validation_failed = True
        else:
            print(f"  \u2705 {pf.name}: Ready to apply")

    if validation_failed and not force:
        print(f"\n\u274c Pre-validation failed. Use 'force' to attempt applying anyway.")
        return False

    # Apply patches in order
    print(f"\n\U0001f527 Applying patches...")
    success_count = 0
    failed_patches = []
    applied_successfully = []

    for i, pf in enumerate(patch_files, 1):
        print(f"\n[{i}/{len(patch_files)}] Applying: {pf.name}")
        result = run(
            ['git', 'apply', '--ignore-whitespace', str(pf)],
            cwd=repo_path,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            print(f"  \u2705 Successfully applied: {pf.name}")
            success_count += 1
            applied_successfully.append(pf)
        else:
            print(f"  \u274c Failed to apply: {pf.name}")
            print(f"     {result.stderr.strip()}")
            failed_patches.append(pf.name)
            if not force:
                print(f"\n\u26a0\ufe0f  Stopping due to failure. Use 'revert' to clean up.")
                break

    print(f"\n\U0001f4ca Results for [{label}]:")
    print(f"  \u2705 Applied: {success_count}")
    print(f"  \u274c Failed:  {len(failed_patches)}")

    if applied_successfully:
        _mark_patches_applied(marker_file, applied_successfully)

    if not failed_patches:
        print(f"\n\U0001f389 All {success_count} patch(es) applied successfully!")
        return True
    else:
        print(f"\n\u26a0\ufe0f  {len(failed_patches)} patch(es) failed.")
        return False


def _revert_repo(label, repo_path, marker_file):
    """Hard-reset a repo and remove its marker file."""
    print(f"\n{'='*60}")
    print(f"  Reverting: {label}  ({repo_path})")
    print(f"{'='*60}")
    run(['git', 'reset', '--hard'], cwd=repo_path)
    run(['git', 'clean', '-fd'], cwd=repo_path)
    _remove_marker(marker_file)
    print(f"\u2713 Reverted [{label}]")


def _status_repo(label, repo_path, patches_dir, marker_file):
    """Print applied/available patch status for one repo."""
    print(f"\n--- {label} ({repo_path}) ---")
    applied = _check_if_patches_applied(marker_file)

    if applied:
        data_lines = [p for p in applied if not p.startswith('#')]
        print(f"Patches applied ({len(data_lines)}):")
        for entry in data_lines:
            name = entry.split(':')[0] if ':' in entry else entry
            print(f"  \u2713 {name}")
    else:
        print("  No patches applied")

    if patches_dir.exists():
        patch_files = sorted(patches_dir.glob('*.patch'))
        if patch_files:
            applied_names = set()
            for entry in applied:
                if ':' in entry and not entry.startswith('#'):
                    applied_names.add(entry.split(':')[0])
            print(f"Available patches in {patches_dir}:")
            for pf in patch_files:
                mark = "\u2713" if pf.name in applied_names else " "
                print(f"  [{mark}] {pf.name}")


# ---------------------------------------------------------------------------
# Top-level commands (iterate over all repos)
# ---------------------------------------------------------------------------

def apply_patches(force=False):
    """Apply patches to all repos. Returns True only if all succeed."""
    all_ok = True
    for cfg in REPO_CONFIGS:
        ok = _apply_patches_to_repo(
            cfg["label"], cfg["repo_path"],
            cfg["patches_dir"], cfg["marker_file"],
            force=force,
        )
        all_ok = all_ok and ok
    return all_ok


def revert_patches():
    """Revert patches in all repos."""
    for cfg in REPO_CONFIGS:
        _revert_repo(cfg["label"], cfg["repo_path"], cfg["marker_file"])


def status():
    """Show patch status for all repos."""
    for cfg in REPO_CONFIGS:
        _status_repo(cfg["label"], cfg["repo_path"],
                     cfg["patches_dir"], cfg["marker_file"])


def usage():
    """Print usage help."""
    print(f"Usage: {sys.argv[0]} [command]")
    print("\nAvailable Commands:")
    print("  apply     Apply all patches to zephyr and modules/lib/hostap")
    print("  force     Force apply patches even if already applied")
    print("  revert    Revert all patches (git reset --hard in each repo)")
    print("  status    Show patch application status")
    print("  help      Show this help message")
    print("\nExamples:")
    print(f"  {sys.argv[0]} apply      # Apply patches")
    print(f"  {sys.argv[0]} force      # Force reapply patches")
    print(f"  {sys.argv[0]} revert     # Revert all patches")
    print(f"  {sys.argv[0]} status     # Show status")
    print("\nPatch Directories:")
    for cfg in REPO_CONFIGS:
        print(f"  [{cfg['label']}] {cfg['patches_dir']}")
    sys.exit(1)


def main():
    """Main function"""
    print(f"Patch Application Script (zephyr + hostap)")
    print(f"Python version: {sys.version}")
    print(f"Workspace root: {WORKSPACE_ROOT_PATH}\n")

    args = sys.argv[1:]
    if not args or any(arg in ('help', '-h', '--help') for arg in args):
        usage()

    for arg in args:
        if arg == "apply":
            apply_patches(force=False)
        elif arg == "force":
            apply_patches(force=True)
        elif arg == "revert":
            revert_patches()
        elif arg == "status":
            status()
        else:
            print(f"Invalid parameter: {arg}", file=sys.stderr)
            sys.exit(2)

    print("\nDone.")


if __name__ == "__main__":
    main()
