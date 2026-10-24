#!/usr/bin/env python3
"""Cross-platform entry point for the pinned Kraken companion build."""
import runpy
from pathlib import Path

if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).with_name('build-kraken-macos.py')), run_name='__main__')
