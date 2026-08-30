#!/usr/bin/env python3
"""Provide one fail-closed subprocess environment for loader-sensitive gates."""

from __future__ import annotations

import os
from collections.abc import Mapping


LOADER_OVERRIDE_VARIABLES = (
    "LD_LIBRARY_PATH",
    "LD_PRELOAD",
    "LIBPATH",
    "SHLIB_PATH",
    "DYLD_LIBRARY_PATH",
    "DYLD_FALLBACK_LIBRARY_PATH",
    "DYLD_FRAMEWORK_PATH",
    "DYLD_FALLBACK_FRAMEWORK_PATH",
    "DYLD_INSERT_LIBRARIES",
)
"""Environment keys that can redirect ELF or Mach-O dependency loading."""


def clean_loader_environment(
    source: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """@brief Copy an environment without dynamic-loader path overrides.

    @param source Optional environment mapping. ``os.environ`` is copied when
      this argument is absent.
    @return An independent string mapping with every maintained ELF/Mach-O
      loader override removed and every unrelated variable retained.
    @throws None Mapping iteration and dictionary construction have no expected
      recoverable failure beyond ordinary Python allocation failure.
    @note Callers pass the returned mapping explicitly to every configure,
      build, inspection, and executable subprocess. The input mapping is never
      mutated.
    """

    inherited = os.environ if source is None else source
    return {
        name: value
        for name, value in inherited.items()
        if name not in LOADER_OVERRIDE_VARIABLES
    }
