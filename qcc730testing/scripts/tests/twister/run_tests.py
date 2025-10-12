#!/usr/bin/env python3

"""
Zephyr Test and Sample Runner

This script provides a convenient interface for running Zephyr tests using
`west twister` and building Zephyr samples with `west build`. Configuration
for tests and samples is managed via a YAML file.

It automatically applies patches to the Zephyr repository on startup and
cleans them on exit.

Windows Users:
--------------
Please always run with flag `--no-outdir` as Windows has problems with
long paths handling.

Usage Examples:
---------------
# List all available tests and samples:
    python3 run_tests.py --list

# Run a single test on a specific platform:
    python3 run_tests.py --platform qcc730mi --port COM4 tests.drivers.pwm.api

# Run a test and let twister create its default output directory:
    python3 run_tests.py --no-outdir tests.drivers.pwm.api

# Run all tests in the pwm driver domain for all domains:
    python3 run_tests.py tests.drivers.pwm

# Run all tests on all platforms defined in the YAML:
    python3 run_tests.py tests

# Build all samples on a specific platform:
    python3 run_tests.py --platform qcc730mi samples

# Build (but do not run) all tests on all platforms:
    python3 run_tests.py --build tests

# Run all defined tests and build all samples on all platforms:
    python3 run_tests.py --all

# Build all defined tests and samples on all platforms:
    python3 run_tests.py --all --build
"""

import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import yaml

# --- Constants and Configuration ---

# File and Directory Paths
SCRIPT_DIR = Path(__file__).resolve().parent
WORKSPACE_ROOT = SCRIPT_DIR.parent.parent.parent.parent.parent.resolve()
REPO_ROOT = SCRIPT_DIR.parent.parent.parent.resolve()
CONFIG_PATH = SCRIPT_DIR / "test_data" / "test_data.yaml"
TWISTER_OUTPUT_BASE_DIR = WORKSPACE_ROOT / "test_results" / "build_output"
DEFAULT_TWISTER_OUT_DIR = WORKSPACE_ROOT / "twister-out"
SAMPLE_BUILD_DIR = WORKSPACE_ROOT / "build_samples"
REPORTS_ARCHIVE_DIR = REPO_ROOT / "reports"
ZEPHYR_TESTS_DIR = WORKSPACE_ROOT / "zephyr" / "tests"
ZEPHYR_PATCH_SCRIPT_DIR = REPO_ROOT / "scripts" / \
    "tests" / "patch_zephyr" / "patch_zephyr.py"

# YAML and Command Keys
KEY_TESTS = "tests"
KEY_SAMPLES = "samples"
KEY_COMMON_CONFIG = "common_config"
KEY_TEST_COMMON_CONFIG = "test_common_config"
KEY_PLATFORM = "platform"
KEY_TESTSUITE_ROOT = "testsuite-root"

# Disable warnings related to too much arguments passed into constructor
# pylint: disable=R0902,R0913,R0917


class TestRunner:
    """
    Builds and executes a single `west twister` or `west build` command.
    Each instance represents one test/sample run on one platform.
    """

    def __init__(
            self,
            items: List[str],
            platform: str,
            port: Optional[str] = None,
            no_out_dir: bool = False):
        """Initializes the runner for a specific test or sample."""
        self.item_name = items[0]
        self.item_type = items[1]
        self.platform = platform
        self.port = port
        self.no_out_dir = no_out_dir
        self.config: Dict[str, Any] = {}
        self.command: List[str] = []
        self.twister_outdir: Optional[Path] = None

    def _set_twister_outdir(self):
        """Sets the twister output directory path for cleaning and reporting."""
        if self.no_out_dir:
            self.twister_outdir = DEFAULT_TWISTER_OUT_DIR
            print(
                f"NOTE: --no-outdir specified. Using default twister path: {self.twister_outdir}")
        else:
            self.twister_outdir = TWISTER_OUTPUT_BASE_DIR / \
                f"{self.item_name}_{self.platform}"

    def run(self) -> bool:
        """Runs the full process for a single test/sample."""
        header = f" PROCESSING: {self.item_name} ({self.item_type}) ON {self.platform} "
        print(f"\n{header:=^80}")

        if not self._prepare_configuration():
            return False

        if self.item_type == KEY_TESTS:
            self._set_twister_outdir()

        self._handle_pre_test_check()

        if not self._install_python_dependencies():
            return False

        self._build_command()
        command_successful = self._execute_command()

        if self.item_type != KEY_TESTS:
            return command_successful

        if not command_successful:
            return command_successful

        report_valid = self._verify_test_report()
        if not report_valid:
            return report_valid

        post_test_ok = self._handle_post_test_check()
        if not post_test_ok:
            return post_test_ok

        self._archive_test_results()
        return True

    def get_command(self) -> List[str]:
        """Returns the built command for this test/sample run."""
        return self.command

    def _install_python_dependencies(self) -> bool:
        """Checks for and installs Python dependencies from requirements.txt."""
        if KEY_TESTSUITE_ROOT not in self.config:
            return True

        test_dir = Path(self.config[KEY_TESTSUITE_ROOT])
        requirements_file = test_dir / "requirements.txt"

        if requirements_file.is_file():
            print(f"--- Found requirements.txt, installing dependencies... ---")
            try:
                command = [sys.executable, "-m", "pip", "install", "-r", str(requirements_file)]
                process = subprocess.run(
                    command,
                    check=True,
                    capture_output=True,
                    text=True,
                    cwd=WORKSPACE_ROOT,
                )
                if process.stdout:
                    print(process.stdout)
                print("--- Python dependencies installed successfully. ---")
                return True
            except subprocess.CalledProcessError as e:
                print(
                    f"--- FAILED: Could not install dependencies from {requirements_file}. ---",
                    file=sys.stderr)
                print(e.stdout, file=sys.stderr)
                print(e.stderr, file=sys.stderr)
                return False
        return True

    def _prepare_configuration(self) -> bool:
        """Loads, merges, and prepares the configuration for the item."""
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                all_config_data = yaml.safe_load(f)
        except (IOError, yaml.YAMLError) as e:
            print(
                f"Error: Could not load or parse config file at {CONFIG_PATH}: {e}",
                file=sys.stderr)
            return False

        specific_config = all_config_data.get(
            self.item_type, {}).get(self.item_name)
        if not specific_config:
            print(f"Error: No configuration found for '{self.item_name}' of type '{self.item_type}'", file=sys.stderr)
            return False

        merged_config = all_config_data.get(KEY_COMMON_CONFIG, {})
        if self.item_type == KEY_TESTS:
            merged_config.update(all_config_data.get(
                KEY_TEST_COMMON_CONFIG, {}))
        merged_config.update(specific_config)

        merged_config[KEY_PLATFORM] = self.platform
        if self.port:
            merged_config["device-serial"] = self.port

        placeholders = {"WORKSPACE_ROOT": str(WORKSPACE_ROOT)}
        self.config = self._resolve_placeholders(merged_config, placeholders)
        return True

    def _resolve_placeholders(
            self, config: Any, placeholders: Dict[str, str]) -> Any:
        """Recursively resolve placeholder strings in a configuration."""
        if isinstance(config, dict):
            return {k: self._resolve_placeholders(
                v, placeholders) for k, v in config.items()}
        if isinstance(config, list):
            return [self._resolve_placeholders(
                i, placeholders) for i in config]
        if isinstance(config, str):
            return config.format(**placeholders)
        return config

    def _build_command(self):
        """Constructs the final command based on the item type."""
        if self.item_type == KEY_TESTS:
            self._build_twister_command()
        elif self.item_type == KEY_SAMPLES:
            self._build_build_command()

        print(f"\n--- Command for '{self.item_name}' on '{self.platform}' ---")
        print(" ".join(self.command))
        print("-" * (37 + len(self.item_name) + len(self.platform)))

    def _build_twister_command(self):
        """Constructs the `west twister` command."""
        self.command = ["west", "twister"]

        if self.twister_outdir.exists():
            shutil.rmtree(self.twister_outdir)

        if not self.no_out_dir:
            self.command.extend(["--outdir", str(self.twister_outdir)])

        self.command.extend(["--disable-warnings-as-errors"])

        keys_to_skip = {"pre_test_setup",
                        "post_test_setup", "description", "notes"}
        for key, value in self.config.items():
            if key in keys_to_skip:
                continue
            flag = f"--{key.replace('_', '-')}"
            if key == "west-flash":
                self.command.append(f'{flag}="{",".join(map(str, value))}"')
            elif isinstance(value, bool) and value:
                self.command.append(flag)
            elif isinstance(value, (str, int, float)):
                self.command.extend([flag, str(value)])
            elif isinstance(value, list) and key != KEY_PLATFORM:
                self.command.extend([flag, " ".join(map(str, value))])

    def _build_build_command(self):
        """Constructs the `west build` command."""
        sample_outdir = SAMPLE_BUILD_DIR / f"{self.item_name}_{self.platform}"
        if sample_outdir.exists():
            shutil.rmtree(sample_outdir)

        self.command = ["west", "build", "-p", "auto",
                        "-d", str(sample_outdir)]
        self.command.extend(["--board", self.config[KEY_PLATFORM]])
        if KEY_TESTSUITE_ROOT in self.config:
            self.command.append(self.config[KEY_TESTSUITE_ROOT])

    def _execute_command(self) -> bool:
        """Executes the constructed command and streams output."""
        if not self.command:
            print("Error: Command was not built.", file=sys.stderr)
            return False
        try:
            with subprocess.Popen(
                " ".join(self.command),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                bufsize=1,
                cwd=WORKSPACE_ROOT,
                shell=True,
            ) as process:
                if process.stdout:
                    for line in iter(process.stdout.readline, ""):
                        print(line, end="")
                process.wait()
                if process.returncode != 0:
                    print(f"\n--- FAILED: Command for '{self.item_name}' on '{self.platform}' exited with code {process.returncode} ---",file=sys.stderr)
                    return False
                print(f"\n--- SUCCESS: Command for '{self.item_name}' on '{self.platform}' finished. ---")
                return True
        except subprocess.SubprocessError as e:
            print(
                f"An unexpected error occurred during execution: {e}",
                file=sys.stderr)
            return False

    def _verify_test_report(self) -> bool:
        """Verifies the twister.json report, failing if 'testsuites' is empty."""
        assert self.twister_outdir is not None
        source_file = self.twister_outdir / "twister.json"
        if not source_file.exists():
            print(
                f"--- Verification FAILED: Report file not found at {source_file}.",
                file=sys.stderr)
            return False

        with open(source_file, "r", encoding="utf-8") as f:
            report = json.load(f)
        if "testsuites" in report and isinstance(report.get(
                "testsuites"), list) and not report["testsuites"]:
            print(
                "--- Verification FAILED: 'testsuites' in report is empty; treating as failure.",
                file=sys.stderr)
            return False
        print("--- Test report verification PASSED. ---")
        return True

    def _archive_test_results(self):
        """Archives the twister.json report and log file to the reports directory."""
        assert self.twister_outdir is not None
        result_file = self.twister_outdir / "twister.json"
        log_file = self.twister_outdir / "twister.log"
        if not result_file.exists() or not log_file.exists():
            return

        # Get destination paths for both the result and log files
        dest_result_path, dest_log_path = self._get_archive_destination_path()
        if not dest_result_path or not dest_log_path:
            return

        # Helper function to handle path creation, deletion, and copying
        def handle_file_operations(src_file: Path, dest_file: Path):
            try:
                dest_file.parent.mkdir(parents=True, exist_ok=True)
                if dest_file.exists():
                    dest_file.unlink()
                shutil.copy(src_file, dest_file)
            except IOError as e:
                print(f"--- Error archiving {src_file.name}: {e}", file=sys.stderr)
                return False
            return True

        # Archive both the result and log files
        if handle_file_operations(result_file, dest_result_path):
            print(f"--- Successfully archived test results to: {dest_result_path}")
        if handle_file_operations(log_file, dest_log_path):
            print(f"--- Successfully archived test logs to: {dest_log_path}")

    def _get_archive_destination_path(self) -> Optional[Tuple[Path, Path]]:
        """Determines the destination path for the archived report and log file."""
        test_source_path_str = self.config.get(KEY_TESTSUITE_ROOT)
        if not test_source_path_str:
            print(
                f"--- Warning: '{KEY_TESTSUITE_ROOT}' not in config for '{self.item_name}', cannot archive results.",
                file=sys.stderr,
            )
            return None

        zephyr_tests_path = ZEPHYR_TESTS_DIR.resolve()
        test_source_path = Path(test_source_path_str).resolve()

        try:
            rel_test_path = test_source_path.relative_to(zephyr_tests_path)
        except ValueError:
            print(
                f"--- Warning: '{KEY_TESTSUITE_ROOT}' has unexpected prefix. Archiving in 'misc'.",
                file=sys.stderr,
            )
            rel_test_path_str = f"misc/{self.item_name.replace('.', '_')}"
            rel_test_path = Path(rel_test_path_str)

        test_name = rel_test_path.name
        last_subdomain = rel_test_path.parent.name if rel_test_path.parent.name else "general"
        date_str = datetime.now().strftime("%Y%m%d")
        
        # Construct the JSON file path
        dest_filename_json = f"{last_subdomain}_{test_name}_{self.platform}_{date_str}.json"
        dest_json_path = REPORTS_ARCHIVE_DIR / rel_test_path.parent / dest_filename_json

        # Construct the log file path
        dest_filename_log = f"{last_subdomain}_{test_name}_{self.platform}_{date_str}.log"
        dest_log_path = REPORTS_ARCHIVE_DIR / rel_test_path.parent / dest_filename_log

        # Always return both the JSON and the log file paths as a tuple
        return dest_json_path, dest_log_path

    def _handle_pre_test_check(self):
        """Displays pre-test setup instructions if they exist."""
        if "pre_test_setup" in self.config:
            instructions = self.config["pre_test_setup"]
            print("\n=== ATTENTION: Pre-test setup required ===")
            print(instructions.strip())
            input("Press Enter to continue once setup is complete...")

    def _handle_post_test_check(self) -> bool:
        """Handles post-test manual verification if required."""
        if "post_test_setup" in self.config:
            instructions = self.config["post_test_setup"]
            print("\n=== ATTENTION: Post-test verification required ===")
            print(instructions.strip())
            choice = input("Confirm test pass? [y/n]: ").lower().strip()
            if choice == "y":
                print("--- Post-test check PASSED. ---")
                return True
            if choice == "n":
                print("--- Post-test check FAILED. ---", file=sys.stderr)
                return False
        return True


class TestOrchestrator:
    """Manages overall test/sample selection and execution process."""

    def __init__(self):
        """Initializes the orchestrator."""
        self.patches_applied = False
        self.args = self._parse_arguments()
        self.all_tests: Dict[str, Any] = {}
        self.all_samples: Dict[str, Any] = {}
        self.platforms: List[str] = []
        self.items_to_run: List[Dict[str, str]] = []
        self.results = {"passed": [], "failed": []}

    def __del__(self):
        """Removes all applied patches on exit, if applied."""
        if not self.patches_applied:
            return

        print(f"\n{'=' * 20} REVERTING ZEPHYR PATCHES {'=' * 20}")
        if not ZEPHYR_PATCH_SCRIPT_DIR.exists():
            print(
                f"Patcher not found at '{ZEPHYR_PATCH_SCRIPT_DIR}'",
                file=sys.stderr)
            return

        try:
            command = [sys.executable, str(ZEPHYR_PATCH_SCRIPT_DIR), "rm"]
            print(f"Executing: {' '.join(command)}")
            subprocess.run(command, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, cwd=WORKSPACE_ROOT)
            print("--- SUCCESS: Zephyr patches reverted ---")
        except subprocess.SubprocessError as e:
            print(
                f"Warning: Patcher cleanup script failed to run: {e}",
                file=sys.stderr)

    def run(self):
        """Main execution method for the orchestrator."""
        if not self._load_and_validate_config():
            return 1

        if self.args.list:
            self._list_items()
            return 0

        if not self._apply_patches():
            return 1

        if not self._select_items_to_run():
            return 1

        print(f"=== Found {len(self.items_to_run)} item(s) to process: {', '.join(i['name'] for i in self.items_to_run)} ===")
        print(f"=== on platform(s): {', '.join(self.platforms)} ===")

        for platform in self.platforms:
            for item in self.items_to_run:
                items = list([item["name"], item["type"]])
                runner = TestRunner(
                    items,
                    platform,
                    self.args.port,
                    self.args.no_outdir)
                result_key = f"{item['name']} on {platform}"
                if runner.run():
                    self.results["passed"].append(result_key)
                else:
                    self.results["failed"].append(result_key)

        self._print_summary()
        return 1 if self.results["failed"] else 0

    def _apply_patches(self) -> bool:
        """Applies patches to the Zephyr repository."""
        print(f"\n{'=' * 20} APPLYING ZEPHYR PATCHES {'=' * 20}")
        if not ZEPHYR_PATCH_SCRIPT_DIR.exists():
            print(
                f"FATAL: Patcher script not found at '{ZEPHYR_PATCH_SCRIPT_DIR}'",
                file=sys.stderr)
            return False

        try:
            command = [sys.executable, str(
                ZEPHYR_PATCH_SCRIPT_DIR), "rm", "inst"]
            print(f"Executing: {' '.join(command)}")
            result = subprocess.run(
                command,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True,
                cwd=WORKSPACE_ROOT)
            print(result.stdout)
            if result.stderr:
                print(result.stderr, file=sys.stderr)
            print(f"\n{'=' * 20} ZEPHYR PATCHED SUCCESSFULLY {'=' * 20}")
            self.patches_applied = True
            return True
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            print(
                "--- FATAL: Patcher script failed. Aborting. ---",
                file=sys.stderr)
            if hasattr(e, "stdout"):
                print("\n--- Patcher STDOUT ---", file=sys.stderr)
                print(e.stdout, file=sys.stderr)
            if hasattr(e, "stderr"):
                print("\n--- Patcher STDERR ---", file=sys.stderr)
                print(e.stderr, file=sys.stderr)
            return False

    def _parse_arguments(self):
        """Parses and returns command-line arguments."""
        parser = argparse.ArgumentParser(
            description="Run Zephyr tests and build samples using a YAML config.",
            formatter_class=argparse.RawTextHelpFormatter,
        )
        parser.add_argument(
            "item_names",
            nargs="*",
            help="Names of tests/samples (e.g., tests.drivers.pwm, samples.hello_world).")
        parser.add_argument(
            "--platform", help="Target platform. Overrides YAML default.")
        parser.add_argument(
            "--port", help="Serial port for device. Overrides YAML.")
        parser.add_argument("--all", action="store_true",
                            help="Run all tests and build all samples.")
        parser.add_argument("--build", action="store_true",
                            help="Build only, do not run.")
        parser.add_argument(
            "--no-outdir",
            action="store_true",
            help="Do not pass the --outdir argument to twister, letting it use its default path.")
        parser.add_argument(
            "--list",
            action="store_true",
            help="List all available tests and samples and exit.")
        args = parser.parse_args()
        if not args.item_names and not args.all and not args.list:
            parser.error(
                "No action requested. Provide item names, or use --all or --list.")
        return args

    def _load_and_validate_config(self) -> bool:
        """Loads the YAML config, validates platforms, and sets instance variables."""
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                config = yaml.safe_load(f)
                self.all_tests = config.get(KEY_TESTS, {})
                self.all_samples = config.get(KEY_SAMPLES, {})
                all_supported_platforms = config.get(
                    KEY_COMMON_CONFIG, {}).get(KEY_PLATFORM, [])
        except (IOError, yaml.YAMLError) as e:
            print(
                f"Error: Could not load or parse config file at {CONFIG_PATH}: {e}",
                file=sys.stderr)
            return False

        if self.args.platform:
            if self.args.platform not in all_supported_platforms:
                print(f"Error: Platform '{self.args.platform}' is not in the list of supported platforms in YAML.", file=sys.stderr)
                return False
            self.platforms = [self.args.platform]
        else:
            if not all_supported_platforms:
                print(
                    "Error: No platforms specified and no platforms found in YAML.",
                    file=sys.stderr)
                return False
            self.platforms = all_supported_platforms
        return True

    def _select_items_to_run(self) -> bool:
        """Selects items to run based on user arguments."""
        if self.args.all:
            test_keys = self.all_tests.keys()
            sample_keys = self.all_samples.keys()
        else:
            test_keys, sample_keys, unmatched = self._filter_items_by_name(
                self.args.item_names)
            if unmatched:
                print(f"Error: No items found for: {', '.join(unmatched)}", file=sys.stderr)
                print("Note: Names must be fully qualified (e.g., 'tests.drivers.pwm').", file=sys.stderr)
                return False

        if not test_keys and not sample_keys:
            print("No items found or selected to run.", file=sys.stderr)
            return False

        items = [{"name": name, "type": KEY_TESTS}
                 for name in sorted(test_keys)]
        items.extend([{"name": name, "type": KEY_SAMPLES}
                     for name in sorted(sample_keys)])
        self.items_to_run = items
        return True

    def _filter_items_by_name(self, names: List[str]):
        """Filters tests and samples based on a list of name prefixes."""
        selected_tests, selected_samples = set(), set()
        unmatched_names = []

        # Handle keywords 'tests' and 'samples'
        local_names = list(names)
        if KEY_TESTS in local_names:
            selected_tests.update(self.all_tests.keys())
            local_names.remove(KEY_TESTS)
        if KEY_SAMPLES in local_names:
            selected_samples.update(self.all_samples.keys())
            local_names.remove(KEY_SAMPLES)

        # Handle specific prefixes
        for name in local_names:
            matched = False
            key_tests_prefix = f"{KEY_TESTS}."
            if name.startswith(key_tests_prefix):
                prefix = name[len(key_tests_prefix):]
                matches = {k for k in self.all_tests if k ==
                           prefix or k.startswith(f"{prefix}.")}
                if matches:
                    selected_tests.update(matches)
                    matched = True
            key_samples_prefix = f"{KEY_SAMPLES}."
            if name.startswith(key_samples_prefix):
                prefix = name[len(key_samples_prefix):]
                matches = {k for k in self.all_samples if k ==
                           prefix or k.startswith(f"{prefix}.")}
                if matches:
                    selected_samples.update(matches)
                    matched = True

            if not matched:
                unmatched_names.append(name)

        return selected_tests, selected_samples, unmatched_names

    def _list_items(self):
        """Prints a list of all available tests and samples from the config."""
        print("\n" + "=" * 20 + " AVAILABLE TESTS " + "=" * 20)
        if self.all_tests:
            for test_name in sorted(self.all_tests.keys()):
                print(f"  - tests.{test_name}")
        else:
            print("  No tests defined in the configuration.")

        print("\n" + "=" * 20 + " AVAILABLE SAMPLES " + "=" * 20)
        if self.all_samples:
            for sample_name in sorted(self.all_samples.keys()):
                print(f"  - samples.{sample_name}")
        else:
            print("  No samples defined in the configuration.")
        print("=" * 20)

    def _print_summary(self):
        """Prints the final summary of results."""
        print("\n" + "=" * 20 + " OVERALL SUMMARY " + "=" * 20)
        total = len(self.results["passed"]) + len(self.results["failed"])
        print(f"Total items processed: {total}")
        if self.results["passed"]:
            print(f"  - Passed: {len(self.results['passed'])}")
            for item in self.results["passed"]:
                print(f"    - {item}")
        if self.results["failed"]:
            print(f"  - Failed: {len(self.results['failed'])}")
            for item in self.results["failed"]:
                print(f"    - {item}")
        print("=" * 20)


def main():
    """ Run test tool """
    print(f"Running Test Tool with Python version: {sys.version}")
    orchestrator = TestOrchestrator()
    exit_code = orchestrator.run()
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
