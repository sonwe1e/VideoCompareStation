"""Bulk-rename active product branding from VCStation to CompareStation."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SKIP_DIR_PARTS = {
    ".git",
    ".zcode",
    "out",
    "node_modules",
    "docs/archive",
    "docs/plans",
    "docs/releases",
}

SKIP_NAME_PREFIXES = (
    "VideoCompareStation_",
    "generated-",
)

SKIP_SUFFIXES = {
    ".png",
    ".jpg",
    ".jpeg",
    ".webp",
    ".gif",
    ".ico",
    ".pdf",
    ".zip",
    ".msi",
    ".exe",
    ".dll",
    ".lib",
    ".pdb",
    ".obj",
    ".o",
    ".a",
    ".so",
}

TEXT_SUFFIXES = {
    "",
    ".md",
    ".txt",
    ".cmake",
    ".in",
    ".ps1",
    ".yml",
    ".yaml",
    ".json",
    ".qrc",
    ".qml",
    ".cpp",
    ".h",
    ".hpp",
    ".js",
    ".rc",
    ".def",
    ".html",
    ".css",
    ".xml",
    ".wxs",
}

# Order matters: longest / most specific first.
REPLACEMENTS = [
    ("VCStation (VideoCompareStation)", "CompareStation"),
    ("VCStation - VideoCompareStation", "CompareStation"),
    ("VideoCompareStation", "CompareStation"),
    ("VCStationCli", "CompareStationCli"),
    ("VCStationShell", "CompareStationShell"),
    ("VCStation", "CompareStation"),
    ("vcstation-issue-log", "comparestation-issue-log"),
    ("vcstation", "comparestation"),
    ("VCSTATION", "COMPARESTATION"),
]


def should_skip(path: Path) -> bool:
    rel = path.relative_to(ROOT).as_posix()
    parts = rel.split("/")
    for i in range(len(parts)):
        prefix = "/".join(parts[: i + 1])
        if prefix in SKIP_DIR_PARTS:
            return True
        if parts[i] in SKIP_DIR_PARTS:
            return True
    name = path.name
    if name.startswith(SKIP_NAME_PREFIXES):
        return True
    if path.suffix.lower() in SKIP_SUFFIXES:
        return True
    if path.suffix.lower() not in TEXT_SUFFIXES and path.suffix != "":
        return True
    return False


def protect_urls(text: str) -> tuple[str, dict[str, str]]:
    """Keep GitHub repo URLs intact (repo slug may stay VideoCompareStation)."""
    placeholders: dict[str, str] = {}
    protected = text
    urls = [
        "https://github.com/sonwe1e/VideoCompareStation",
    ]
    for i, url in enumerate(urls):
        key = f"__URL_KEEP_{i}__"
        placeholders[key] = url
        protected = protected.replace(url, key)
    return protected, placeholders


def restore_urls(text: str, placeholders: dict[str, str]) -> str:
    restored = text
    for key, url in placeholders.items():
        restored = restored.replace(key, url)
    return restored


def transform(text: str) -> str:
    protected, placeholders = protect_urls(text)
    for old, new in REPLACEMENTS:
        protected = protected.replace(old, new)
    return restore_urls(protected, placeholders)


def main() -> None:
    changed = []
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file() or should_skip(path):
            continue
        try:
            original = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        updated = transform(original)
        if updated != original:
            path.write_text(updated, encoding="utf-8", newline="\n")
            changed.append(path.relative_to(ROOT).as_posix())
    print(f"updated {len(changed)} files")
    for item in changed:
        print(item)


if __name__ == "__main__":
    main()
