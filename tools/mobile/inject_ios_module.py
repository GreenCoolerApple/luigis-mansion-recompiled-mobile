#!/usr/bin/env python3
import argparse
import os
import shutil
import sys
import tempfile
import zipfile


def find_app_dir(names):
    for n in names:
        parts = n.split("/")
        if len(parts) >= 2 and parts[0] == "Payload" and parts[1].endswith(".app"):
            return "Payload/" + parts[1]
    return None


def main():
    p = argparse.ArgumentParser(
        description="Put game modules exported by Luigi's Mansion Recompiled into the iOS app's IPA.")
    p.add_argument("ipa", help="LuigisMansionRecompiled-<version>-ios-arm64-unsigned.ipa")
    p.add_argument("modules", nargs="+", help="lm_game_GLME01.dylib and/or lm_game_GLMP01.dylib")
    p.add_argument("-o", "--output", help="output IPA (default: <ipa>-with-game.ipa)")
    a = p.parse_args()

    for m in a.modules:
        name = os.path.basename(m)
        if not (name.startswith("lm_game_") and name.endswith(".dylib")):
            sys.exit(f"{m}: expected a file named lm_game_<GAMEID>.dylib (Export for iPhone / iPad on the PC)")
        with open(m, "rb") as f:
            data = f.read()
        if data[:4] != b"\xcf\xfa\xed\xfe" or b"LMMODKEY:" not in data:
            sys.exit(f"{m}: not a game module exported for iPhone / iPad")

    out = a.output or os.path.splitext(a.ipa)[0] + "-with-game.ipa"
    with zipfile.ZipFile(a.ipa) as zin:
        app = find_app_dir(zin.namelist())
        if not app:
            sys.exit(f"{a.ipa}: no Payload/<name>.app inside")
        replaced = {f"{app}/Frameworks/{os.path.basename(m)}" for m in a.modules}
        tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".ipa", dir=os.path.dirname(os.path.abspath(out)))
        tmp.close()
        with zipfile.ZipFile(tmp.name, "w", zipfile.ZIP_DEFLATED) as zout:
            for info in zin.infolist():
                if info.filename in replaced:
                    continue
                zout.writestr(info, zin.read(info.filename))
            for m in a.modules:
                target = f"{app}/Frameworks/{os.path.basename(m)}"
                zi = zipfile.ZipInfo(target)
                zi.external_attr = (0o100755 << 16)
                zi.compress_type = zipfile.ZIP_DEFLATED
                with open(m, "rb") as f:
                    zout.writestr(zi, f.read())
    shutil.move(tmp.name, out)
    print(f"Wrote {out}. Sign and install it with your sideloading tool (AltStore, SideStore, Sideloadly, ...).")


if __name__ == "__main__":
    main()
