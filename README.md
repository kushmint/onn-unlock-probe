# onn-unlock-probe

A tiny, non-destructive compatibility check for whether the AVB
bootloader-unlock method used by
[onn-4k-pro-2026-unlock-root](https://github.com/kushmint/onn-4k-pro-2026-unlock-root)
also exists on other onn Google TV boxes - specifically the **onn Plus** and
the **onn 4K Pro (2024)**, which that project's author doesn't own and can't
test on.

This is a separate, much smaller tool. It does **not** unlock or root your
device. It only checks whether the same trusted-application (TA) interface
is present, and - optionally - whether the lock-state bit on that hardware
can be flipped and flipped back without rebooting.

## What it actually does

1. **Read-only by default.** Opens `/dev/tee0`, opens a session with the same
   TA UUID the 2026 unlock tool uses, and reads the AVB lock-state value
   (command 2). That's it. Zero writes. This alone tells you whether the
   same TEE/TA interface exists on your box at all.
2. **Only with `--toggle-test`:** if the bit reads as locked (`1`), it writes
   `0` (unlock), reads back to confirm, then **immediately** writes `1`
   (re-lock) and reads back again to confirm the restore actually worked.
   Every step is reported. **It never reboots the device, at any point.**

Why this matters: Android reads properties like `ro.boot.flash.locked` at
boot. As long as the device is never rebooted, nothing about the running
system changes even if the underlying bit moved. The real risk window is
"after the next reboot" - which is exactly why this tool never triggers one,
and why the script warns you loudly if the restore step didn't work.

### Important: this is genuinely unproven on other hardware

The original tool treats writing this bit as a one-time, permanent action
and goes out of its way to guarantee it's never retried. That caution exists
because on real AVB implementations this kind of secure lock-state storage
is sometimes enforced as **one-directional** (TA refuses to move it back
from 0 to 1), as an anti-tamper measure - even if the underlying storage
isn't a physical fuse. This probe assumes nothing either way: it tries the
restore and tells you plainly whether it worked.

**If the final result after `--toggle-test` is anything starting with
`CRITICAL`, the restore did not verifiably succeed. Do not reboot the
device.** Re-run the read-only check to see the current state before
deciding anything further. Treat that device as unlocked until proven
otherwise.

## Quick start

Requires `adb` and a device with USB/network debugging enabled, connected
and authorized.

```sh
git clone https://github.com/kushmint/onn-unlock-probe.git
cd onn-unlock-probe
tools/run_probe.sh
# or, with multiple devices attached:
tools/run_probe.sh <adb-serial>
```

The script pushes the right prebuilt binary for the device's CPU ABI, runs
the read-only check first, prints the result, and only runs the toggle test
if you explicitly type `y` at the prompt. It cleans up the pushed binary
afterward either way.

### Manual run (no script)

```sh
adb push bin/arm64-v8a/probe /data/local/tmp/onn-unlock-probe   # or armeabi-v7a
adb shell chmod 755 /data/local/tmp/onn-unlock-probe
adb shell /data/local/tmp/onn-unlock-probe                      # read-only
adb shell /data/local/tmp/onn-unlock-probe --toggle-test         # reversible test
adb shell rm -f /data/local/tmp/onn-unlock-probe
```

## Reading the output

The binary prints newline-delimited JSON to stdout, ending with a line like:

```json
{"stage":"finished","outcome":"already_unlocked_no_write_attempted","exit_code":0,"rebooted":false}
```

`outcome` values you'll actually see:

| outcome | meaning | risk |
|---|---|---|
| `already_unlocked_no_write_attempted` | lock bit already reads 0 | none - no write issued |
| `locked_read_only_rerun_with_toggle_test_to_probe_further` | bit reads 1; read-only pass finished | none |
| `no_tee_device` | no `/dev/tee0` on this box | none - not applicable here |
| `tee_version_ioctl_failed` / `not_optee_gp` | TEE present but not OP-TEE/GP as expected | none |
| `ta_not_present_or_session_refused` | this TA/UUID isn't on this device | none - not compatible |
| `initial_read_failed` / `initial_value_unexpected` | TA present but read behaves unexpectedly | none - no write issued |
| `toggle_test_passed_state_fully_restored` | flipped to 0 and back to 1, verified both times | none - fully reversible here |
| `unlock_write_failed_state_presumed_unchanged` | write to unlock was rejected | low - nothing moved |
| `unlock_write_had_no_effect_state_still_locked` | write "succeeded" but bit didn't move | low - nothing moved |
| `CRITICAL_relock_ineffective_bit_appears_one_way_device_left_unlocked_do_not_reboot` | flipped to 0, could **not** flip back to 1 | **device is left unlocked - do not reboot** |
| `CRITICAL_relock_write_failed_device_left_unlocked_do_not_reboot` | restore write itself failed | **device is left unlocked - do not reboot** |
| `CRITICAL_midstate_read_failed_state_unknown_do_not_reboot` / `CRITICAL_final_read_failed_state_unknown_do_not_reboot` | couldn't confirm state after a write | **state unknown - do not reboot, re-check first** |

If you hit any `CRITICAL_*` outcome, please report it (model, build fingerprint,
the full JSON output) rather than rebooting to "see what happens."

## What a report should include

Please copy the full JSON output (it includes `ro.product.device`,
`ro.product.model`, `ro.build.fingerprint`, `ro.boot.bootloader`, etc.) along
with which model/region box you ran it on. That's enough to tell whether the
2026 unlock method's approach has any chance of porting to that hardware.

## Building from source

```sh
python3 tools/build_native.py --ndk /path/to/android-ndk-r28c
```

Built and verified with Android NDK r28c, API 28, for `armeabi-v7a` and
`arm64-v8a`, same flags as the parent project (`-std=c11 -D_GNU_SOURCE -O2
-Wall -Wextra -Werror -fPIE -pie`).

## Relationship to onn-4k-pro-2026-unlock-root

This is a separate, throwaway diagnostic companion to
[openensemble/onn-4k-pro-2026-unlock-root](https://github.com/openensemble/onn-4k-pro-2026-unlock-root)
and its [kushmint fork](https://github.com/kushmint/onn-4k-pro-2026-unlock-root).
Full credit to openensemble for discovering the AVB TA unlock method this
probe reuses the read/write command protocol from. This tool does not
perform the actual unlock-and-root walkthrough - use the parent project for
that once/if a device is confirmed compatible.

## Disclaimer

Use at your own risk. Even though this tool is designed to never leave the
device in a different state than it found it (and never reboots), it talks
directly to a trusted-execution environment via an undocumented, reverse
engineered command protocol on hardware the author has not tested. Follow
the warnings above, especially: **if the toggle test doesn't report a clean
restore, do not reboot the device.**
