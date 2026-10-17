#!/usr/bin/env python3
"""Back-compat wrapper: rounds/round-1 prompts are rendered by benchmark/make_prompts.py."""
import subprocess
import sys
from pathlib import Path
subprocess.run([sys.executable, str(Path(__file__).resolve().parent.parent.parent / "make_prompts.py"),
                "--round", "round-1"], check=True)
