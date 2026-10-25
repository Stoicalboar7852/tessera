#!/usr/bin/env python3
"""Repairs an MSYS2 installation on a file system without hard links (exFAT, FAT32).

pacman's libarchive cannot create hard links there and skips those files ("Can't create ...").
This script asks pacman which package files are missing, then re-extracts exactly those files from
the cached, already signature-checked packages with Python's tarfile, which falls back to copying
the link target when a hard link cannot be created.

Usage (Windows Python 3.14+, for zstd support):
    python -I tools/setup/msys2-fix-hardlinks.py T:\\toolchain\\msys64
"""
import pathlib
import subprocess
import sys
import tarfile


def pacman(root: pathlib.Path, *args: str) -> str:
    bash = root / "usr" / "bin" / "bash.exe"
    cmd = "pacman " + " ".join(args)
    res = subprocess.run([str(bash), "-lc", cmd], capture_output=True, text=True, env={"MSYSTEM": "UCRT64", "PATH": ""})
    return res.stdout


def missing_files(root: pathlib.Path) -> dict[str, set[str]]:
    """package -> set of archive member names (relative, forward slashes) that are missing on disk."""
    out: dict[str, set[str]] = {}
    for line in pacman(root, "-Qkq").splitlines():
        parts = line.strip().split(" ", 1)
        if len(parts) != 2:
            continue
        pkg, path = parts
        rel = path.lstrip("/")
        if rel.endswith("/"):
            continue  # directories are recreated by extraction of their files
        out.setdefault(pkg, set()).add(rel)
    return out


def cached_package(root: pathlib.Path, pkg: str) -> pathlib.Path | None:
    q = pacman(root, "-Q", pkg).split()
    if len(q) != 2:
        return None
    version = q[1]
    cache = root / "var" / "cache" / "pacman" / "pkg"
    for cand in cache.glob(f"{pkg}-{version}-*.pkg.tar.*"):
        if not cand.name.endswith(".sig"):
            return cand
    return None


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = pathlib.Path(sys.argv[1])
    todo = missing_files(root)
    if not todo:
        print("no missing files")
        return 0
    failed = 0
    for pkg, members in sorted(todo.items()):
        archive = cached_package(root, pkg)
        if archive is None:
            print(f"{pkg}: cached package not found; reinstall it with pacman -S {pkg}")
            failed += 1
            continue
        mode = "r:zst" if archive.name.endswith(".zst") else "r:*"
        with tarfile.open(archive, mode) as tar:
            wanted = [m for m in tar.getmembers() if m.name in members]
            for m in wanted:
                try:
                    tar.extract(m, path=root, filter="tar")
                except Exception as exc:  # report and continue with the rest
                    print(f"{pkg}: {m.name}: {exc}")
                    failed += 1
        print(f"{pkg}: restored {len(wanted)} of {len(members)} missing files")
    remaining = missing_files(root)
    print(f"remaining missing files: {sum(len(v) for v in remaining.values())}")
    return 1 if failed or remaining else 0


if __name__ == "__main__":
    sys.exit(main())
