import json
from pathlib import Path

DATA=json.loads(Path(__file__).with_name('brand.json').read_text(encoding='utf-8'))
PROJECT=DATA['project']
DEVICE=DATA['device']
TOOL=DATA['tool']
VERSION=DATA['version']
DEFAULT_BIN=DATA['default_bin']
