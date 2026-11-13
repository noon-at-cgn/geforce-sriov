# Identity hook (`libgeforce_sriov.so`)

The identity hook is an `LD_PRELOAD` library for NVIDIA's `nvidia-vgpud`. The stock `vgpuConfig.xml`
lists no GeForce device ID, so the daemon refuses a GeForce card ("GPU not supported by vGPU"). The
hook presents each selected card to the daemon under the vGPU target of its generation from the shared
device table (`include/gs_devices.h`): an Ampere card as the RTX A6000 (`10de:2230`), an Ada card as
the RTX 6000 Ada (`10de:26b1`). The daemon then offers that target's vGPU types (also called profiles,
each with a fixed memory size). NVIDIA does not support this configuration.

## When it matters

The hook matters when `nvidia-vgpud` has to see a GeForce card as its professional counterpart, so
that vGPU types appear for it. The CLI installs it for `nvidia-vgpud` only, and it does nothing in any
other process.

After a PCI rescan, a hot-plug or a bus renumbering, stop the vGPU VMs, clear their VF types to `0` and
run `sudo geforce-sriov activate`, which restarts `nvidia-vgpud`. The card table is built from sysfs
once, when the daemon starts.

## What it changes

The hook looks only at `NV_ESC_RM_CONTROL` (`ioctl` request `0xc020462a`), and only after the driver has
answered successfully. Two replies are changed:

| Command | Action |
|---|---|
| `NV2080_CTRL_CMD_BUS_GET_PCI_INFO` `0x20801801` | For a selected card whose device word is its own `10de:<device>`, the hook writes `10de:<target>`. The board's own subsystem is kept (the stock `<pgpu>` entry of both targets matches any subsystem). Revision and `pciExtDeviceId` are unchanged |
| `NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE` `0x00800289` | With `identity_force_host=on`, and only for a selected card, `virtualizationMode` is set to HOST (3). `isGridBuild` is unchanged |

Everything else reaches the driver and comes back unchanged: other devices, failed calls, unexpected
sizes and every other `ioctl`. The caller always gets the driver's return value and `errno`.

The hook does not touch `SET_TYPES_INFO`, SR-IOV caps, type info, licensing or `nvidia-vgpu-mgr`, and
it does not edit or redirect `vgpuConfig.xml`. vGPU types and their memory are the target's own
entries from the stock file, so a type can be larger than the card's memory.

## When it stays inactive

The decision is made once per process. If any check fails, the hook is inactive and every `ioctl` passes
through.

1. **Process.** `/proc/self/comm` must be `nvidia-vgpud`.
2. **Config.** The file is `/etc/geforce-sriov/config`, or the path in `GEFORCE_SRIOV_CONFIG`. It must
   exist, be a regular file, be owned by root or the process's user, be writable by no one else, be
   at most 65536 bytes and valid, and contain `identity=on`.
3. **Driver build.** The version on the `NVRM version:` line of `/proc/driver/nvidia/version` must match
   one of these builds exactly:

   | Build | Status |
   |---|---|
   | `580.159.01` | Confirmed |
   | `580.178.05` | **Not guaranteed to work**; the hook acts on it. Open headers of tag 580.178.04 (no 580.178.05 tag exists) are byte-identical to 580.159.03 for the headers the hook uses |
   | `595.91.04` | **Not guaranteed to work**; the hook acts on it. No 595.91.04 source tag exists; the headers used are 595.91.07 |

   Any other build leaves the hook inactive.
4. **Cards.** `bdfs` must be present. Each listed PF (function 0) is mapped only when:
   - the vendor is `10de` and it is not a VF;
   - it is a display-class device (base class `0x03`);
   - its device ID is in the table;
   - no other NVIDIA display-class physical function shares its domain, bus and slot (VFs and the audio
     function do not count). The driver names a GPU by domain, bus and slot only, so a second display
     function makes the slot ambiguous and the card is skipped. An unreadable function in the slot also
     skips the card.

   At least one card must remain. Identical cards may be installed and selected in any combination.

### Attribution

`GET_PCI_INFO` and `GET_VIRTUALIZATION_MODE` replies carry no bus address. For each reply it may change,
the hook therefore asks the driver which GPU the daemon's handle belongs to, using standard read-only RM
controls on the daemon's own fd and client. The answer is compared exactly with the mapped cards
(domain, bus and slot). Domains above `ffff` are supported, as behind Intel VMD, and the config accepts
them as the kernel prints them, for example `10000:81:00.0`.

- **Attributed to a mapped card:** the `GET_PCI_INFO` reply is rewritten if its device word is that
  card's own `10de:<device>` (a different word is left unchanged and logged once), and
  `GET_VIRTUALIZATION_MODE` is forced to HOST.
- **Attributed to any other GPU:** left unchanged. This covers an unselected identical card, the boot
  display, a skipped card, and a GPU in another domain or behind another switch port.
- **Not attributable** (a query fails or answers out of range): `GET_VIRTUALIZATION_MODE` is left
  unchanged, with no fallback. `GET_PCI_INFO` falls back to the device ID only if two scans of sysfs
  (one at startup, one at the time of the reply) both find that every NVIDIA physical function with that
  device ID is a mapped card. Otherwise the reply is left unchanged and the reason is logged once.

Nothing is cached, because handles can be freed and reused for another GPU. That costs 2 extra controls
per matching `GET_PCI_INFO` reply and 4 per non-HOST `GET_VIRTUALIZATION_MODE` reply.

If no subdevice exists when `nvidia-vgpud` asks for the virtualization mode, HOST is not forced.

## Config

The hook reads the same file and format as the CLI, which owns it:

- Lines are separated by `\n`. Empty lines and lines starting with `#` are ignored.
- Every other line is `key=value`, where the key matches `[a-z0-9_]+` and the value is non-empty.
- No whitespace or `\r` may appear on a line, and a key may not repeat.
- Keys the hook does not use are ignored.

Any violation makes the whole file invalid, and the hook stays inactive.

| Key | Default | Meaning to the hook |
|---|---|---|
| `identity` | `off` | `on` enables the rewrite |
| `identity_force_host` | `off` | `on` forces the virtualization-mode reply to HOST for a mapped card ([Attribution](#attribution)). `install --device` turns it on |
| `bdfs` | none | 1–32 distinct canonical PFs `dddd:bb:ss.0` (domain of 4–8 hex digits, as the kernel prints it), comma-separated |

## Logs

Log lines go to syslog (`LOG_DAEMON`) with the prefix `geforce-sriov-hook:`. Read them with
`journalctl -t nvidia-vgpud`. When the hook becomes active, it logs:

- each card mapping (`card <bdf>: <name> (10de:<device>, <die>) presented as <target> (10de:<target id>), subsystem kept`);
- why a card was skipped (`card <bdf>: ... skipped`, including `ambiguous slot`);
- a missing device-ID fallback (`card <bdf>: no device-ID fallback: ...`);
- the first rewrite and the first HOST force for each card.

Once per process, it also logs size mismatches, FINN-serialized calls, device words of no card, replies
from a GPU that is not a mapped card, and replies it cannot attribute to a GPU, with the outcome
(`unchanged`, `by device ID`, or `device-ID fallback refused: <reason>`).

## Install

The CLI installs the library as `$(PREFIX)/lib/geforce-sriov/libgeforce_sriov.so` and adds a drop-in
for `nvidia-vgpud.service`:

```ini
[Service]
ExecStartPre=/usr/local/bin/geforce-sriov check --daemon-ready
Environment=LD_PRELOAD=/usr/local/lib/geforce-sriov/libgeforce_sriov.so
```

The `ExecStartPre` line keeps `nvidia-vgpud` from starting until the selected VFs are ready.

Do not preload it system-wide or into `nvidia-vgpu-mgr`. It stays inactive in any process other than
`nvidia-vgpud` regardless.

## Status

Ampere and Ada are supported. The host and guest drivers should
match the [supported software stack](../docs/installation.md#supported-software-stack). 580.159.01 is the
only confirmed host version. `install` and `setup` carry on with another host version as best effort
(`setup` asks, default no; `--allow-unsupported-driver` answers yes), and other versions are **not
guaranteed to work**; other guest driver versions are **not guaranteed to work** either. On the manual
path (`--driver=skip`), `status`, `check` and `activate` print `NVIDIA's driver X is not guaranteed to
work; confirmed versions: 580.159.01.` and continue. The hook acts on the builds it lists
([When it stays inactive](#when-it-stays-inactive)) and passes any other build through unchanged.
How the device table marks each row is in
[Status of every row](../docs/devices.md#status-of-every-row), and each generation's details are in
[Ampere](../docs/ampere.md) and [Ada](../docs/ada.md).

For an Ada card, the device word becomes the RTX 6000 Ada's (for example `10de:2783` → `10de:26b1`)
and the board's own subsystem is kept. The hook does not check whether the driver runs with or
without GSP. Several selected Ada cards are told apart by PCI address as Ampere cards are.

## Build and tests

```sh
make -C hook          # hook/libgeforce_sriov.so  (needs a C11 compiler, libc, libdl)
make -C hook check    # behavioural tests, no GPU/driver/root needed
```

`check` runs a stand-in `nvidia-vgpud` under the hook against a fake driver, using temporary configs,
version files and sysfs trees. A test-only build of `gs_hook.c` (`-DGS_HOOK_TEST_ENV`) is the only one
that honours the test environment variables, and it logs to stderr. The fake driver validates every
query the hook sends, and the runner compares each reply, return value, `errno` and status with the
expected one.

The scenarios (including a no-hook baseline) cover:

- topologies with unselected identical cards, mixed device IDs, several PCI domains, PCIe switch ports,
  adjacent slots, audio functions and VFs in a card's slot, and ambiguous slots;
- failures of each attribution query, and the device-ID fallback in each of its on and off states;
- handles reassigned mid-run, client changes, and 8 threads × 500 calls on selected and unselected
  cards at once;
- replies that must stay unchanged and send no queries;
- inactive configs, inactive drivers, and the process gate.

## Provenance and credits

The structures and command values in `include/gs_hook.h` come from NVIDIA's MIT-licensed
[open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules) headers, compared at tags
580.159.03, 580.178.04 and 595.91.07 (`nvos.h`, `ctrl2080bus.h`, `ctrl0080gpu.h`, `ctrl2080gpu.h`,
`ctrl0000gpu.h`, `nv_escape.h` and `nv-ioctl-numbers.h`). Sizes and offsets are pinned with
`_Static_assert`.

The design is adapted from earlier unpublished code by the same copyright holder, which was itself
trimmed from a fork of [vgpu_unlock-rs](https://github.com/mbilker/vgpu_unlock-rs) 2.5.0 (MIT,
© 2021 Jonathan Johansson, © 2021 Matt Bilker), which builds on DualCoder's
[vgpu_unlock](https://github.com/DualCoder/vgpu_unlock). Their notices are kept in `LICENSE`.

Differences from the earlier hook:

- One build serves all listed drivers, and the version is read from `/proc/driver/nvidia/version`.
- The rewrite is scoped by the CLI's `bdfs` allowlist, and each reply is attributed to a GPU by bus
  address through the driver, instead of matching the device ID alone.
- The device table comes from `gs_devices.h` instead of explicit config pairs.
- No result file is written, and `SET_INFO` is not recorded.
