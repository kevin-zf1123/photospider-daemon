#!/usr/bin/env python3
"""Verify loader-environment sanitation and installed-prefix fail-closed rules."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
"""Canonical source root used only to import maintained test helpers."""

sys.path.insert(0, str(REPOSITORY_ROOT / "tools"))
sys.path.insert(0, str(REPOSITORY_ROOT / "tests" / "integration"))

from installed_consumer_smoke import validate_resolved_loader_paths  # noqa: E402
from loader_environment import (  # noqa: E402
    LOADER_OVERRIDE_VARIABLES,
    clean_loader_environment,
)


class LoaderEnvironmentContractTest(unittest.TestCase):
    """@brief Exercise the durable loader-sensitive subprocess boundary.

    @throws AssertionError When sanitation or prefix validation accepts a
      polluted or non-installed dependency record.
    @note Tests use temporary synthetic paths and a real Python child process;
      they never start ``photospiderd`` or mutate the caller environment.
    """

    def test_all_loader_overrides_are_removed_from_real_child(self) -> None:
        """@brief Prove every maintained loader override is absent in a child.

        @return None after the sanitized real child reports no override keys.
        @throws AssertionError If sanitation drops an unrelated key, retains an
          override, or the child cannot report its environment.
        @note The polluted mapping is passed through the production helper
          before process creation, so invalid loader values never affect the
          test interpreter itself.
        """

        polluted = {
            "PATH": "/maintained/path",
            "PHOTOSPIDER_DAEMON_TEST_SENTINEL": "retained",
            **{name: f"polluted-{name}" for name in LOADER_OVERRIDE_VARIABLES},
        }
        cleaned = clean_loader_environment(polluted)
        self.assertEqual(cleaned["PATH"], "/maintained/path")
        self.assertEqual(
            cleaned["PHOTOSPIDER_DAEMON_TEST_SENTINEL"], "retained"
        )
        self.assertTrue(
            all(name not in cleaned for name in LOADER_OVERRIDE_VARIABLES)
        )

        child = subprocess.run(
            [
                sys.executable,
                "-c",
                (
                    "import json, os; "
                    "print(json.dumps(dict(os.environ), sort_keys=True))"
                ),
            ],
            env=cleaned,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        observed = json.loads(child.stdout)
        self.assertEqual(
            observed["PHOTOSPIDER_DAEMON_TEST_SENTINEL"], "retained"
        )
        self.assertTrue(
            all(name not in observed for name in LOADER_OVERRIDE_VARIABLES)
        )

    def test_loader_records_require_the_installed_runtime_prefix(self) -> None:
        """@brief Accept only the expected installed operation-runtime record.

        @return None after the positive fixture passes and two invalid fixtures
          fail closed.
        @throws AssertionError If a missing or source-tree runtime is accepted.
        @note Real ``ldd``/``otool`` parsing remains exercised by the installed
          consumer; this fixture isolates the ownership decision deterministically.
        """

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected_prefix = root / "installed-photospider"
            expected_runtime = (
                expected_prefix / "lib" / "libphotospider_operation_runtime.so"
            )
            expected_runtime.parent.mkdir(parents=True)
            expected_runtime.touch()
            source_root = root / "photospider-source"
            source_runtime = (
                source_root / "build" / "libphotospider_operation_runtime.so"
            )
            source_runtime.parent.mkdir(parents=True)
            source_runtime.touch()

            validate_resolved_loader_paths(
                binary_label="fixture",
                resolved_paths=[expected_runtime.resolve(strict=True)],
                expected_dependency_prefix=expected_prefix.resolve(strict=True),
                forbidden_roots=(source_root.resolve(strict=True),),
                allowed_roots=(expected_prefix.resolve(strict=True),),
            )
            with self.assertRaisesRegex(RuntimeError, "operation runtime"):
                validate_resolved_loader_paths(
                    binary_label="fixture",
                    resolved_paths=[],
                    expected_dependency_prefix=expected_prefix.resolve(strict=True),
                    forbidden_roots=(source_root.resolve(strict=True),),
                    allowed_roots=(expected_prefix.resolve(strict=True),),
                )
            with self.assertRaisesRegex(RuntimeError, "outside the expected"):
                validate_resolved_loader_paths(
                    binary_label="fixture",
                    resolved_paths=[source_runtime.resolve(strict=True)],
                    expected_dependency_prefix=expected_prefix.resolve(strict=True),
                    forbidden_roots=(source_root.resolve(strict=True),),
                    allowed_roots=(expected_prefix.resolve(strict=True),),
                )


if __name__ == "__main__":
    unittest.main()
