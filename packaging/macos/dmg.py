# SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
# SPDX-License-Identifier: GPL-3.0-or-later

import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ARCH = "arm64"
SYSTEM_PATHS = ("/usr/lib/", "/System/Library/")
LOAD_DYLIB = {
    "LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
    "LC_LAZY_LOAD_DYLIB", "LC_LOAD_UPWARD_DYLIB",
}
HERE = Path(__file__).resolve().parent


def run(*args):
    subprocess.run(args, check=True)


def read(*args):
    return subprocess.check_output(args, text=True).strip()


def assemble_app(work, binary, info, svg, catalogs):
    renderer = shutil.which("rsvg-convert")
    if renderer is None:
        raise ValueError("Install the icon renderer with: brew install librsvg")

    app = work / "payload/gchat.app"
    resources = app / "Contents/Resources"
    macos = app / "Contents/MacOS"
    macos.mkdir(parents=True)
    resources.mkdir()
    shutil.copyfile(binary, macos / "gchat")
    shutil.copyfile(info, app / "Contents/Info.plist")

    for catalog in catalogs:
        language = catalog.parent.parent.name
        destination = resources / "locale" / language / "LC_MESSAGES"
        destination.mkdir(parents=True)
        shutil.copyfile(catalog, destination / "gchat.mo")

    iconset = work / "gchat.iconset"
    iconset.mkdir()
    for base in (16, 32, 128, 256, 512):
        for scale, suffix in ((1, ""), (2, "@2x")):
            size = base * scale
            edge = size * 13 // 16
            inset = (size - edge) / 2
            png = iconset / f"icon_{base}x{base}{suffix}.png"
            run(renderer, f"--page-width={size}", f"--page-height={size}",
                f"--width={edge}", f"--height={edge}", f"--left={inset:g}",
                f"--top={inset:g}", "--output", png, svg)
    run("iconutil", "-c", "icns", "-o", resources / "gchat.icns", iconset)
    return app


def load_commands(path):
    output = read("otool", "-arch", ARCH, "-l", path)
    for block in re.split(r"^Load command \d+\n", output, flags=re.M)[1:]:
        cmd = re.search(r"^\s*cmd (\S+)$", block, flags=re.M).group(1)
        value = re.search(
            r"^\s*(?:name|path) (.+) \(offset \d+\)$", block, flags=re.M
        )
        if cmd in {"LC_BUILD_VERSION", "LC_VERSION_MIN_MACOSX"}:
            value = re.search(r"^\s*(?:minos|version) ([\d.]+)$", block, re.M)
        yield cmd, value.group(1) if value else None


def expanded(path, origin, executable):
    if path.startswith("@loader_path/"):
        return origin.parent / path.removeprefix("@loader_path/")
    if path.startswith("@executable_path/"):
        return executable.parent / path.removeprefix("@executable_path/")
    return Path(path)


class Runtime:
    def __init__(self, app, binary):
        self.app = app
        self.binary = binary.resolve()
        self.frameworks = app / "Contents/Frameworks"
        self.resources = app / "Contents/Resources"
        self.frameworks.mkdir()
        self.prefix = Path(read("brew", "--prefix"))
        self.cellar = Path(read("brew", "--cellar")).resolve()
        self.packages = set()
        self.objects = {}
        self.pending = []
        self.minimum = ()
        self.main_paths = self.search_paths(self.binary)
        self.add(self.binary, app / "Contents/MacOS/gchat")

    def formula(self, name):
        keg = (self.prefix / "opt" / name).resolve(strict=True)
        self.track_formula(keg)
        return keg

    def track_formula(self, source):
        if not source.is_relative_to(self.cellar):
            raise ValueError(f"Not in the Homebrew Cellar: {source}")
        name, version, *_ = source.relative_to(self.cellar).parts
        self.packages.add((name, version))

    def add(self, source, destination=None):
        source = source.resolve(strict=True)
        if source in self.objects:
            return self.objects[source]
        if source != self.binary:
            self.track_formula(source)
        if destination is None:
            destination = self.frameworks / source.name
        if destination in self.objects.values():
            raise ValueError(f"Duplicate destination: {destination.name}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        archs = read("lipo", "-archs", source).split()
        if ARCH not in archs:
            raise ValueError(f"Missing {ARCH} slice: {source}")
        if len(archs) > 1:
            run("lipo", source, "-thin", ARCH, "-output", destination)
        elif source != self.binary:
            shutil.copyfile(source, destination)
        destination.chmod(0o755)
        self.objects[source] = destination
        self.pending.append(source)
        return destination

    def search_paths(self, source):
        return [expanded(path, source, self.binary)
                for cmd, path in load_commands(source) if cmd == "LC_RPATH"]

    def resolve(self, path, source, paths):
        if path.startswith("@rpath/"):
            tail = path.removeprefix("@rpath/")
            candidates = (directory / tail for directory in paths)
        else:
            candidates = (expanded(path, source, self.binary),)
        for candidate in candidates:
            if candidate.is_absolute() and candidate.is_file():
                return candidate
        raise ValueError(f"Cannot resolve {path} from {source}")

    def install(self):
        while self.pending:
            source = self.pending.pop()
            destination = self.objects[source]
            commands = list(load_commands(source))
            paths = [expanded(path, source, self.binary)
                     for cmd, path in commands if cmd == "LC_RPATH"]
            changes = []
            minimum = ()
            if any(cmd == "LC_CODE_SIGNATURE" for cmd, _ in commands):
                run("codesign", "--remove-signature", destination)
            for cmd, value in commands:
                if cmd == "LC_RPATH":
                    changes.extend(("-delete_rpath", value))
                elif cmd == "LC_ID_DYLIB":
                    changes.extend(("-id", "@rpath/" + destination.name))
                elif cmd in LOAD_DYLIB and not value.startswith(SYSTEM_PATHS):
                    target = self.add(self.resolve(value, source,
                                                   paths + self.main_paths))
                    relative = os.path.relpath(target, destination.parent)
                    changes.extend(("-change", value, "@loader_path/" + relative))
                elif cmd in {"LC_BUILD_VERSION", "LC_VERSION_MIN_MACOSX"}:
                    minimum = tuple(int(part) for part in value.split("."))
            if not minimum:
                raise ValueError(f"Missing macOS deployment target: {source}")
            self.minimum = max(self.minimum, minimum)
            if changes:
                run("install_name_tool", *changes, destination)

        self.verify()
        info_path = self.app / "Contents/Info.plist"
        info = plistlib.loads(info_path.read_bytes())
        info["LSMinimumSystemVersion"] = ".".join(map(str, self.minimum))
        info_path.write_bytes(plistlib.dumps(info))
        return info

    def verify(self):
        paths = {path.resolve() for path in self.objects.values()}
        for path in paths:
            for cmd, value in load_commands(path):
                if cmd == "LC_RPATH":
                    raise ValueError(f"Unexpected runpath: {path}: {value}")
                if cmd not in LOAD_DYLIB or value.startswith(SYSTEM_PATHS):
                    continue
                if (not value.startswith("@loader_path/") or
                        expanded(value, path, self.binary).resolve() not in paths):
                    raise ValueError(f"Unbundled dependency: {path}: {value}")

    def install_data(self, catalogs):
        share = self.resources / "share"
        data = [
            ("adwaita-icon-theme", "icons/Adwaita"),
            ("hicolor-icon-theme", "icons/hicolor"),
            ("gtksourceview5", "gtksourceview-5"),
        ]
        for name, path in data:
            shutil.copytree(self.formula(name) / "share" / path, share / path)

        languages = {catalog.parent.parent.name for catalog in catalogs}
        for name in ("glib", "gtk4", "libadwaita", "gtksourceview5"):
            locale = self.formula(name) / "share/locale"
            for language in sorted(languages):
                relative = Path(language) / "LC_MESSAGES"
                destination = share / "locale" / relative
                for catalog in (locale / relative).glob("*.mo"):
                    destination.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(catalog, destination / catalog.name)

        schemas = share / "glib-2.0/schemas"
        schema_source = self.formula("gtk4") / "share/glib-2.0/schemas"
        schemas.mkdir(parents=True)
        for path in schema_source.glob("org.gtk.gtk4.*.gschema.xml"):
            shutil.copyfile(path, schemas / path.name)
        compile_schemas = self.formula("glib") / "bin/glib-compile-schemas"
        run(compile_schemas, "--strict", schemas)

        certs = self.formula("ca-certificates") / "share/ca-certificates/cacert.pem"
        shutil.copyfile(certs, self.resources / "cacert.pem")
        shutil.copyfile(HERE / "fonts.conf", self.resources / "fonts.conf")

        gio = self.formula("glib-networking") / "lib/gio/modules/libgiognutls.so"
        self.add(gio, self.frameworks / "gio/modules" / gio.name)

        pixbuf = self.formula("gdk-pixbuf")
        svg = self.formula("librsvg")
        modules = "lib/gdk-pixbuf-2.0/2.10.0/loaders"
        loaders = sorted((pixbuf / modules).glob("*.so"))
        loaders.extend(sorted((svg / modules).glob("*.so")))
        cache = read(pixbuf / "bin/gdk-pixbuf-query-loaders", *loaders)
        cache = "\n".join(line for line in cache.splitlines()
                          if not line.startswith("#"))
        for loader in loaders:
            self.add(loader, self.frameworks / "gdk-pixbuf" / loader.name)
            quoted = f'"{loader}"'
            if quoted not in cache:
                raise ValueError(f"Missing loader metadata: {loader}")
            # GModule calls dlopen from the Frameworks directory.
            cache = cache.replace(quoted, f'"@loader_path/gdk-pixbuf/{loader.name}"')
        (self.resources / "loaders.cache").write_text(cache + "\n\n", encoding="utf-8")

    def install_licenses(self, license_file):
        licenses = self.resources / "Licenses"
        licenses.mkdir()
        shutil.copyfile(license_file, licenses / "gchat.txt")
        inventory = []
        for name, version in sorted(self.packages):
            keg = self.cellar / name / version
            dest = licenses / name
            dest = dest / version
            dest.mkdir(parents=True)
            inventory.append(f"{name}\t{version}")
            for file in keg.iterdir():
                if file.is_file() and file.name.upper().startswith(
                        ("COPYING", "COPYRIGHT", "LICENSE", "NOTICE", "SBOM.")):
                    shutil.copyfile(file, dest / file.name)
            if (keg / "share/licenses").is_dir():
                shutil.copytree(keg / "share/licenses", dest / "licenses")
            for file in (keg / ".brew").glob("*.rb"):
                shutil.copyfile(file, dest / file.name)
        (licenses / "Homebrew-packages.tsv").write_text(
            "Formula\tVersion\n" + "\n".join(inventory) + "\n", encoding="utf-8"
        )

    def sign(self, identity):
        timestamp = ["--timestamp"] if identity != "-" else []
        for path in self.objects.values():
            if path.is_relative_to(self.frameworks):
                run("codesign", "--force", "--sign", identity, *timestamp, path)
        options = ["--options", "runtime"] if identity != "-" else []
        run("codesign", "--force", "--sign", identity, *timestamp, *options, self.app)
        run("codesign", "--verify", "--deep", "--strict", self.app)


def build(binary, info, icon, license_file, output, *catalogs):
    identity = os.environ.get("GCHAT_CODESIGN_IDENTITY") or "-"
    notary_profile = os.environ.get("GCHAT_NOTARY_PROFILE")
    if notary_profile and identity == "-":
        raise ValueError("Notarization requires a Developer ID Application identity")

    with tempfile.TemporaryDirectory(prefix="gchat-dmg-", dir=output) as tmp:
        work = Path(tmp)
        app = assemble_app(work, binary, info, icon, catalogs)
        payload = app.parent
        runtime = Runtime(app, binary)
        runtime.install_data(catalogs)
        info = runtime.install()
        runtime.install_licenses(license_file)
        runtime.sign(identity)
        (payload / "Applications").symlink_to("/Applications")

        version = info["CFBundleShortVersionString"]
        dmg = work / f"gchat-{version}-{ARCH}.dmg"
        create = ("diskutil", "image", "create", "from")
        supported = subprocess.run((*create, "--help"), capture_output=True)
        if supported.returncode == 0:
            run(*create, "--volumeName", "gchat", "--format", "UDZO", payload, dmg)
        else:
            run("hdiutil", "create", "-srcfolder", payload, "-volname", "gchat",
                "-fs", "HFS+", "-format", "UDZO", dmg)
        if identity != "-":
            run("codesign", "--sign", identity, "--timestamp", dmg)
        if notary_profile:
            result = json.loads(read(
                "xcrun", "notarytool", "submit", dmg, "--keychain-profile",
                notary_profile, "--wait", "--output-format", "json",
            ))
            if result.get("status") != "Accepted":
                raise ValueError(f"Notarization failed: {json.dumps(result)}")
            run("xcrun", "stapler", "staple", dmg)
            run("xcrun", "stapler", "validate", dmg)
        run("hdiutil", "verify", dmg)
        final = dmg.replace(output / dmg.name)
        print(f"Created {final} (macOS {info['LSMinimumSystemVersion']} or later)")


if __name__ == "__main__":
    try:
        build(*(Path(arg).absolute() for arg in sys.argv[1:]))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"DMG packaging failed: {error}")
