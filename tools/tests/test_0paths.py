"""Puts tools/ on sys.path, so plain `python3 -m unittest discover -s tools/tests`
works without PYTHONPATH=tools. unittest imports the test modules in sorted
order, and this name sorts first. It holds no tests."""
import sys
from pathlib import Path

TOOLS = str(Path(__file__).resolve().parents[1])
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)
