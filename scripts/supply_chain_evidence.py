"""Importable helpers for the supply-chain evidence command."""

from __future__ import annotations

import importlib.util
from pathlib import Path


_path = Path(__file__).with_name("supply-chain-evidence.py")
_spec = importlib.util.spec_from_file_location("chtholly_supply_chain_cli", _path)
if _spec is None or _spec.loader is None:
    raise ImportError(f"cannot load {_path}")
_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_module)

digest_file = _module.digest_file
