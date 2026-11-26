# The in-game test system's pytest set-up: everything is in bt_plugin.py (docs/ingame-tests.md).
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
pytest_plugins = ["bt_plugin"]
