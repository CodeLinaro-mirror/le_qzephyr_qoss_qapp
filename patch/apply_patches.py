#!/usr/bin/env python3

# Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
# SPDX-License-Identifier: BSD-3-Clause

"""
Script to apply patches from patches directory to Zephyr source code.
This script is designed to be called before west build to apply necessary patches.
Supports both Windows and Linux/Ubuntu platforms.
"""

import sys
import os
import subprocess
from pathlib import Path
import shutil
import platform

# File and Directory Paths
SCRIPT_DIR = Path(__file__).resolve().parent
PATCHES_DIR = SCRIPT_DIR  # Patches are now in the same directory as the script
QCC730_ROOT_PATH = SCRIPT_DIR.parent.resolve()
WORKSPACE_ROOT_PATH = QCC730_ROOT_PATH.parent.resolve()
ZEPHYR_PATH = WORKSPACE_ROOT_PATH / "zephyr"

# Marker file to track if patches have been applied
PATCH_MARKER_FILE = ZEPHYR_PATH / ".patches_applied"


def run(cmd, cwd=None, check=True, capture_output=False, text=False):
    """Run command in a certain directory"""
    print(f"Running: {' '.join(cmd)} (in {cwd or os.getcwd()})")

    kwargs = {
        'cwd': cwd,
        'check': check,
        'universal_newlines': text
    }

    if capture_output:
        kwargs['stdout'] = subprocess.PIPE
        kwargs['stderr'] = subprocess.PIPE

    return subprocess.run(cmd, **kwargs)


def check_if_patches_applied():
    """Check if patches have already been applied"""
    if PATCH_MARKER_FILE.exists():
        with open(PATCH_MARKER_FILE, 'r') as f:
            content = f.read().strip()
            if content:
                applied_patches = content.split('\n')
                return [p for p in applied_patches if p]  # Filter out empty lines
        return []
    return []


def get_patch_checksum(patch_file):
    """Calculate checksum for patch file to detect changes"""
    import hashlib
    with open(patch_file, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()


def mark_patches_applied(patch_files):
    """Mark that patches have been applied with checksums for integrity"""
    from datetime import datetime
    with open(PATCH_MARKER_FILE, 'w') as f:
        f.write("# Zephyr Patches Applied - DO NOT EDIT MANUALLY\n")
        f.write(f"# Applied on: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write("# Format: patch_filename:checksum\n")
        for patch_file in patch_files:
            checksum = get_patch_checksum(patch_file)
            f.write(f"{patch_file.name}:{checksum}\n")
    print(f"Patch marker file created: {PATCH_MARKER_FILE}")


def validate_applied_patches():
    """Validate that applied patches haven't changed"""
    applied_patches = check_if_patches_applied()
    if not applied_patches:
        return True, []
    
    invalid_patches = []
    for patch_info in applied_patches:
        if ':' not in patch_info or patch_info.startswith('#'):
            continue
        
        patch_name, expected_checksum = patch_info.split(':', 1)
        patch_file = PATCHES_DIR / patch_name
        
        if not patch_file.exists():
            invalid_patches.append(f"{patch_name} (file missing)")
            continue
            
        current_checksum = get_patch_checksum(patch_file)
        if current_checksum != expected_checksum:
            invalid_patches.append(f"{patch_name} (checksum mismatch)")
    
    return len(invalid_patches) == 0, invalid_patches


def remove_patch_marker():
    """Remove the patch marker file"""
    if PATCH_MARKER_FILE.exists():
        PATCH_MARKER_FILE.unlink()
        print(f"Patch marker file removed: {PATCH_MARKER_FILE}")


def apply_patches(force=False):
    """Apply all patches from patches directory to Zephyr"""
    if not PATCHES_DIR.exists():
        print(f"Patches directory not found: {PATCHES_DIR}")
        return False

    # Get all patch files (sorted by name for consistent ordering)
    patch_files = sorted(PATCHES_DIR.glob('*.patch'))
    
    if not patch_files:
        print(f"No patch files found in {PATCHES_DIR}")
        return True  # No patches to apply is considered success

    # Validate existing patches if not forcing
    if not force:
        applied_patches = check_if_patches_applied()
        if applied_patches:
            # Validate integrity of applied patches
            is_valid, invalid_patches = validate_applied_patches()
            if is_valid:
                print("Patches have already been applied and are valid. Use 'force' to reapply.")
                print(f"Applied patches: {len([p for p in applied_patches if not p.startswith('#')])}")
                return True
            else:
                print("⚠️  Applied patches have integrity issues:")
                for invalid in invalid_patches:
                    print(f"  - {invalid}")
                print("Use 'force' to reapply or 'revert' to clean up.")
                return False

    print(f"Found {len(patch_files)} patch file(s) to apply:")
    for i, patch_file in enumerate(patch_files, 1):
        print(f"  {i:02d}. {patch_file.name}")

    # Pre-validate all patches before applying any
    print(f"\n🔍 Pre-validating all patches...")
    validation_failed = False
    for patch_file in patch_files:
        result = run(
            ['git', 'apply', '--check', '--ignore-whitespace', str(patch_file)],
            cwd=ZEPHYR_PATH,
            check=False,
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            print(f"  ❌ {patch_file.name}: Cannot be applied")
            print(f"     {result.stderr.strip()}")
            validation_failed = True
        else:
            print(f"  ✅ {patch_file.name}: Ready to apply")
    
    if validation_failed and not force:
        print(f"\n❌ Pre-validation failed. Some patches cannot be applied.")
        print("   Use 'force' to attempt applying valid patches only.")
        return False

    # Apply each patch in order
    print(f"\n🔧 Applying patches...")
    success_count = 0
    failed_patches = []
    applied_successfully = []
    
    for i, patch_file in enumerate(patch_files, 1):
        print(f"\n[{i}/{len(patch_files)}] Applying: {patch_file.name}")
        try:
            # Apply the patch (with whitespace tolerance)
            result = run(
                ['git', 'apply', '--ignore-whitespace', str(patch_file)],
                cwd=ZEPHYR_PATH,
                check=False,
                capture_output=True,
                text=True
            )
            
            if result.returncode == 0:
                print(f"  ✅ Successfully applied: {patch_file.name}")
                success_count += 1
                applied_successfully.append(patch_file)
            else:
                print(f"  ❌ Failed to apply: {patch_file.name}")
                print(f"     {result.stderr.strip()}")
                failed_patches.append(patch_file.name)
                
                # If we fail to apply a patch, we should stop to maintain consistency
                if not force:
                    print(f"\n⚠️  Stopping patch application due to failure.")
                    print(f"   Use 'revert' to clean up and try again.")
                    break
            
        except subprocess.CalledProcessError as e:
            print(f"  ❌ Error applying patch {patch_file.name}: {e}")
            failed_patches.append(patch_file.name)
            if not force:
                break

    # Report results and mark patches as applied
    print(f"\n📊 Patch Application Results:")
    print(f"  ✅ Successfully applied: {success_count}")
    print(f"  ❌ Failed: {len(failed_patches)}")
    
    if success_count > 0 and not failed_patches:
        mark_patches_applied(applied_successfully)
        print(f"\n🎉 All {success_count} patch(es) applied successfully!")
        return True
    elif failed_patches:
        print(f"\n⚠️  Partial success - {len(failed_patches)} patch(es) failed:")
        for failed in failed_patches:
            print(f"    - {failed}")
        if applied_successfully:
            mark_patches_applied(applied_successfully)
            print(f"  ℹ️  {len(applied_successfully)} patches were applied successfully.")
        return False
    else:
        print(f"\n❌ No patches were applied")
        return False


def revert_patches():
    """Revert all patches by resetting Zephyr repository"""
    print("Reverting all patches...")
    
    # Reset all changes
    run(['git', 'reset', '--hard'], cwd=ZEPHYR_PATH)
    run(['git', 'clean', '-fd'], cwd=ZEPHYR_PATH)
    
    # Remove marker file
    remove_patch_marker()
    
    print("✓ All patches reverted")


def status():
    """Show patch application status"""
    applied_patches = check_if_patches_applied()
    
    if applied_patches:
        print("Patches currently applied:")
        for patch in applied_patches:
            print(f"  ✓ {patch}")
    else:
        print("No patches currently applied")
    
    # List available patches
    if PATCHES_DIR.exists():
        patch_files = sorted(PATCHES_DIR.glob('*.patch'))
        if patch_files:
            print(f"\nAvailable patches in {PATCHES_DIR}:")
            for patch_file in patch_files:
                status_mark = "✓" if patch_file.name in applied_patches else " "
                print(f"  [{status_mark}] {patch_file.name}")


def usage():
    """Print usage help"""
    print(f"Usage: {sys.argv[0]} [command]")
    print("\nAvailable Commands:")
    print("  apply     Apply all patches from patches directory to Zephyr")
    print("  force     Force apply patches even if already applied")
    print("  revert    Revert all patches (reset Zephyr repository)")
    print("  status    Show patch application status")
    print("  help      Show this help message")
    print("\nExamples:")
    print(f"  {sys.argv[0]} apply      # Apply patches")
    print(f"  {sys.argv[0]} force      # Force reapply patches")
    print(f"  {sys.argv[0]} revert     # Revert all patches")
    print(f"  {sys.argv[0]} status     # Show status")
    print("\nPatch Directory:")
    print(f"  {PATCHES_DIR}")
    sys.exit(1)


def main():
    """Main function"""
    print(f"Zephyr Patch Application Script")
    print(f"Python version: {sys.version}")
    print(f"Zephyr path: {ZEPHYR_PATH}")
    print(f"Patches directory: {PATCHES_DIR}\n")
    
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
