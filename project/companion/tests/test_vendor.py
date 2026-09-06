import hashlib
import re
from pathlib import Path

from recorder_companion import _espressif


def test_vendored_files_match_pinned_sources_except_documented_import_relocation():
    root = Path(_espressif.__file__).parent
    manifest = (root / "NOTICE.md").read_text(encoding="utf-8")
    entries = re.findall(r"^([A-F0-9]{64}) (.+)$", manifest, re.MULTILINE)
    assert len(entries) == 13
    for expected, relative in entries:
        content = root.joinpath(*relative.split("\\")).read_bytes()
        if relative.endswith(".py"):
            text = content.decode("utf-8")
            text = text.replace("from .. import proto", "import proto")
            text = text.replace("from ..utils import ", "from utils import ")
            text = re.sub(r"from \. import (\w+_pb2) as ", r"import \1 as ", text)
            content = text.encode("utf-8")
        assert hashlib.sha256(content).hexdigest().upper() == expected, relative
