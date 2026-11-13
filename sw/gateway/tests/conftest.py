import sys
from pathlib import Path

# Gateway modules import each other as top-level modules (as when running main.py).
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
