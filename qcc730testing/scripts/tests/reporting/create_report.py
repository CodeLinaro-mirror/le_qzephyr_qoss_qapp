#!/usr/bin/env python3

# Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
# SPDX-License-Identifier: BSD-3-Clause

"""
Aggregate Zephyr test result JSON files and generates a consolidated HTML
report.

Usage:
    python3 create_report.py

"""

import json
from datetime import datetime
from pathlib import Path
import webbrowser
import argparse

# --- Configuration ---
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent.parent.resolve()
REPORTS_DIR = REPO_ROOT / "reports"
OUTPUT_FILENAME = REPORTS_DIR / "summary_test_report.html"

# --- Template File Paths ---
TEMPLATE_FILE = SCRIPT_DIR / "templates" / "report_template.html"
CSS_FILE = SCRIPT_DIR / "templates" / "report_style.css"


class ReportGenerator:  # pylint: disable=R0903
    """
    Scans for JSON test reports, parses them, and generates a
    consolidated HTML report from external template files.
    """

    def __init__(self, reports_dir, output_path):
        """Initializes the ReportGenerator."""
        self.reports_dir = reports_dir
        self.output_path = output_path
        self.flat_test_cases = []
        self.passed_tests = 0
        self.failed_tests = 0
        self.skipped_tests = 0
        self.not_run_tests = 0

    def _parse_report_file(self, report_file_path):
        """Parses a single JSON report file."""
        try:
            with report_file_path.open("r") as f:
                return json.load(f)
        except json.JSONDecodeError:
            print(f"Warning: Could not decode JSON from {report_file_path}")
        return None

    def _extract_test_cases_from_data(self, report_data, report_file_path):
        """
        Extracts test cases from the parsed report data and adds them
        to the instance's list of test cases.
        """
        report_date = datetime.fromtimestamp(
            report_file_path.stat().st_mtime).strftime("%Y-%m-%d %H:%M:%S")
        if "testsuites" in report_data and isinstance(
                report_data["testsuites"], list):
            for suite in report_data["testsuites"]:
                platform_name = suite.get("platform", "N/A").split("/")[0]
                for case in suite.get("testcases", []):
                    status = case.get("status", "unknown")
                    if status == "passed":
                        self.passed_tests += 1
                    elif status == "failed":
                        self.failed_tests += 1
                    elif status == "skipped":
                        self.skipped_tests += 1
                    elif status == "not run":
                        self.not_run_tests += 1

                    self.flat_test_cases.append(
                        {
                            "report_date": report_date,
                            "platform": platform_name,
                            "name": case.get(
                                "identifier",
                                "Unknown Test Case"),
                            "time": case.get(
                                "execution_time",
                                "N/A"),
                            "status": status,
                        })

    def _find_and_parse_reports(self):
        """
        Recursively finds all JSON files, parses them, and extracts the
        test case data.
        """
        if not self.reports_dir.is_dir():
            print(
                f"Error: Reports directory not found at '{self.reports_dir}'")
            return False

        print(
            f"Scanning for JSON reports in: {
                self.reports_dir} and its subdirectories.")
        json_files = list(self.reports_dir.rglob("*.json"))

        if not json_files:
            print("No JSON reports found.")
            return False

        for report_file in json_files:
            report_data = self._parse_report_file(report_file)
            if report_data:
                self._extract_test_cases_from_data(report_data, report_file)

        return bool(self.flat_test_cases)

    def _generate_html_report(self):
        """Generates a technical HTML table report from the collected test data."""
        try:
            with TEMPLATE_FILE.open("r") as f:
                html_template = f.read()
            with CSS_FILE.open("r") as f:
                css_style = f.read()
        except FileNotFoundError as e:
            print(
                f"Error: Could not find template file. Make sure '{
                    e.filename}' is in the same directory as the script."
            )
            return

        self.flat_test_cases.sort(key=lambda x: x.get("name", ""))
        table_rows = ""
        for case in self.flat_test_cases:
            status = case.get("status", "unknown").replace(" ", "_")
            table_rows += f"""
                <tr>
                    <td>{case.get('platform')}</td>
                    <td>{case.get('report_date')}</td>
                    <td>{case.get('name')}</td>
                    <td>{case.get('time')}</td>
                    <td><span class="status {status}">{status}</span></td>
                </tr>
            """

        total_tests = self.passed_tests + self.failed_tests
        pass_rate = (self.passed_tests / total_tests *
                     100) if total_tests > 0 else 0

        html_content = html_template.format(
            css_style=css_style,
            table_rows=table_rows,
            generation_date=datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            total_passed=self.passed_tests,
            total_failed=self.failed_tests,
            total_skipped=self.skipped_tests,
            total_not_run=self.not_run_tests,
            pass_rate=f"{pass_rate:.2f}%",
        )

        with self.output_path.open("w") as f:
            f.write(html_content)
        print(f"Successfully generated HTML report: {self.output_path}")

    def _open_in_browser(self):
        """Open generated report in default browser."""
        webbrowser.open(f"file://{self.output_path.resolve()}")
        print("Opened report in default web browser.")

    def run(self, open_browser=True):
        """Executes the report generation process."""
        if self._find_and_parse_reports():
            self._generate_html_report()
            if open_browser:
                self._open_in_browser()
        else:
            print("Report generation failed: No test data found.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate Zephyr test report")
    parser.add_argument(
        "--ci",
        action="store_true",
        help="Run in CI mode (do not open browser)")
    args = parser.parse_args()

    report_generator = ReportGenerator(
        reports_dir=REPORTS_DIR, output_path=OUTPUT_FILENAME)
    report_generator.run(open_browser=not args.ci)
