import argparse
import json
import zipfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    library = args.library.resolve()
    output = args.output.resolve()

    if not library.is_file():
        raise FileNotFoundError(f"Library not found: {library}")

    manifest = {
        "type": "preload-native",
        "name": "Flying Pet Diagnostic F",
        "author": "Victor",
        "description": "Crash-isolation build: delayed guarded one-line Tessellator/RenderMesh pipeline with step-by-step logs.",
        "version": "0.2.7",
        "entry": "libFlyingPet.so",
        "overwrite_files": [],
        "overwrite_folders": []
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr("manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
        archive.write(library, "libFlyingPet.so")

    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
