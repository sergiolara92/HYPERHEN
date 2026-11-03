#!/usr/bin/env python3
"""Build the pinned json-c release with its upstream destruction fixes."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SHA256 = "37ad0249902e301bd9052bf712e511fcc6acff4ecaad4b5900aad9ce564e26de"
URL = "https://github.com/json-c/json-c/releases/download/json-c-0.19-20260627/json-c-0.19.tar.gz"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path)
    parser.add_argument("--sdk", type=Path, default=Path(os.environ.get("PS5_PAYLOAD_SDK", "/opt/ps5-payload-sdk")))
    parser.add_argument("--source-archive", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--host", action="store_true", help="Build a native host library for release regressions")
    parser.add_argument("--sanitize", action="store_true", help="Enable host ASan/UBSan")
    args = parser.parse_args()
    if args.jobs < 1 or (args.sanitize and not args.host):
        parser.error("jobs must be positive; sanitizers require --host")
    default_prefix = "json-c-host" if args.host else "json-c"
    if args.sanitize:
        default_prefix += "-sanitize"
    prefix = (args.prefix or ROOT / ".deps" / default_prefix).resolve()
    patch = ROOT / "patches/json-c-0.19-release-free.patch"
    # The locale APIs in the payload SDK are stubs: newlocale ignores base,
    # and uselocale(NULL) changes global state. JSON uses the C numeric locale
    # throughout this payload; avoid both the leaked duplicate and that race.
    identity = repr(("locale-off-threading-v1", args.host, args.sanitize, str(args.sdk.resolve()))).encode()
    revision = hashlib.sha256(patch.read_bytes() + Path(__file__).read_bytes() + identity).hexdigest()
    stamp = prefix / ".shadowmount-revision"
    installed = ("lib/libjson-c.a", "include/json-c/json.h",
                 "include/json-c/json_config.h", "include/json-c/json_object.h")
    if (stamp.exists() and stamp.read_text() == revision and
            all((prefix / name).is_file() for name in installed)):
        return
    prefix.mkdir(parents=True, exist_ok=True)
    archive = prefix / "json-c-0.19.tar.gz"
    if args.source_archive:
        if args.source_archive.resolve() != archive:
            shutil.copyfile(args.source_archive, archive)
    elif not archive.exists():
        temporary = archive.with_suffix(".download")
        with urllib.request.urlopen(URL, timeout=60) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("json-c source checksum mismatch: " + str(archive))
    work = prefix / (".build-" + revision[:16])
    source = work / "json-c-0.19"
    prepared = work / ".prepared"
    if not prepared.exists():
        work.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as tar:
            for member in tar.getmembers():
                target = (work / member.name).resolve()
                if member.issym():
                    target = (target.parent / member.linkname).resolve()
                elif member.islnk():
                    target = (work / member.linkname).resolve()
                if work not in target.parents:
                    raise RuntimeError("Unexpected source archive entry: " + member.name)
            tar.extractall(work)
        subprocess.run(["patch", "-p1", "--batch", "-i", str(patch)], cwd=source, check=True)
        prepared.touch()
    cmake = "cmake" if args.host else str(args.sdk.resolve() / "bin/prospero-cmake")
    toolchain = [] if args.host else ["-DCMAKE_TOOLCHAIN_FILE=" + str(args.sdk.resolve() / "toolchain/prospero.cmake")]
    flags = [] if args.host else ["-Wno-unreachable-code-generic-assoc"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    build = work / "build"
    subprocess.run([cmake, *toolchain, "-S", str(source), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF",
        "-DBUILD_APPS=OFF",
        "-DENABLE_THREADING=ON", "-DHAVE_USELOCALE=OFF", "-DHAVE_SETLOCALE=OFF",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DCMAKE_INSTALL_LIBDIR=lib",
        "-DCMAKE_INSTALL_PREFIX=" + str(prefix), "-DCMAKE_C_FLAGS=" + " ".join(flags)], check=True)
    subprocess.run([cmake, "--build", str(build), "--parallel", str(args.jobs)], check=True)
    subprocess.run([cmake, "--install", str(build)], check=True)
    stamp.write_text(revision)


if __name__ == "__main__":
    main()
