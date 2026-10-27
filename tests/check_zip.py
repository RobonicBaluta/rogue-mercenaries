"""Independent ZIP check: Python's zipfile must read archives written by rm::WriteStoredZip.

Usage: python -I tests/check_zip.py <zip> [<zip> ...]
Exits non-zero if any archive fails zipfile.testzip() or contains non-stored entries.
"""

import sys
import zipfile


def check(path: str) -> None:
    with zipfile.ZipFile(path) as archive:
        bad = archive.testzip()
        if bad is not None:
            raise SystemExit(f"{path}: CRC/header error in {bad}")
        names = archive.namelist()
        if not names:
            raise SystemExit(f"{path}: archive is empty")
        for info in archive.infolist():
            if info.compress_type != zipfile.ZIP_STORED:
                raise SystemExit(f"{path}: {info.filename} is not stored")
            data = archive.read(info)
            if len(data) != info.file_size:
                raise SystemExit(f"{path}: {info.filename} size mismatch")
        print(f"{path}: OK ({len(names)} entries: {', '.join(names)})")


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        check(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
