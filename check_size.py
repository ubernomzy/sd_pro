# PlatformIO post-build check: fail the build if firmware.bin is too large.
#
# Over-the-air updates are written into the free space beside the running
# firmware, inside a ~1MB area. If a version is bigger than about half of
# that, the NEXT update won't fit and the clock can only be updated by
# opening it and connecting a USB-serial adapter. 500KB keeps a margin.

import os

Import("env")  # noqa: F821 (provided by PlatformIO)

LIMIT_BYTES = 500_000


def check_size(source, target, env):
    path = target[0].get_abspath()
    size = os.path.getsize(path)
    print(f"Firmware size: {size:,} bytes (limit {LIMIT_BYTES:,})")
    if size > LIMIT_BYTES:
        print(
            "ERROR: firmware is too big to update safely over WiFi.\n"
            "Remove something (fonts, features) before uploading it to the clock."
        )
        env.Exit(1)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_size)  # noqa: F821
