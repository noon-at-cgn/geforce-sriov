# Compatibility reports

Submit a [compatibility report](https://github.com/noon-at-cgn/geforce-sriov/issues/new?template=compatibility-report.yml) to record whether geforce-sriov works on your card and host.
Reports from other machines help establish which cards and hosts work.
Failed and partially working setups are just as useful: choose *Does not work* or *Partially* in the form and say where it stopped.

See [Supported cards](devices.md#status-of-every-row) for the status of every card and driver version.
The [Ampere](ampere.md) and [Ada](ada.md) pages cover generation-specific details.

Reports are voluntary.
The form takes a few minutes to complete, and only a few fields are required.

## What to collect

Run these commands on the host.
Replace `0000:01:00.0` with your card's address from `lspci -nn | grep -i nvidia`.
In a source checkout without an install, use `./geforce-sriov` instead of `geforce-sriov`.

| What | Command | What to copy |
|---|---|---|
| PCI ID and subsystem ID | `lspci -nnv -s 0000:01:00.0` | The `[10de:....]` at the end of the first line, and the ID on the `Subsystem:` line |
| Device table row | `geforce-sriov devices` | Whether your ID is listed (every listed card is supported) |
| PCIe path to the GPU | `lspci -tv` | Whether the card is attached to a CPU root port, the chipset or a PCIe switch |
| Host kernel | `uname -r` | The whole line |
| NVIDIA vGPU host driver | `modinfo -F version nvidia` | The whole line |
| geforce-sriov version | `geforce-sriov --version`, and `git rev-parse --short HEAD` in a checkout | Both |
| `install --dry-run` output, before installing | `sudo geforce-sriov install --dry-run --device 0000:01:00.0` | The whole output |
| `status` output | `sudo geforce-sriov status` | The whole output |
| Kernel log lines | `journalctl -k -b --no-hostname \| grep -E 'geforce_sriov\|NVRM\|AER'` | Only these lines, not the whole log |

For a problem from the previous boot, use `-b -1` instead of `-b`; this requires a persistent journal.
Without systemd's journal, `sudo dmesg | grep -E 'geforce_sriov|NVRM|AER'` shows the same lines for the current boot.
See [Logs](recovery.md#logs) for other log sources.

For each guest, note:

- the guest OS
- its vGPU type, the profile set on its VF (for example `RTXA6000-4Q`)
- the version of the NVIDIA vGPU guest driver installed in it

## What to remove before posting

Issues are public. Before you paste output, remove or replace:

- IP and MAC addresses
- serial numbers of the GPU, the board or any other part, and GPU UUIDs (`GPU-…`)
- host names and user names, for example in shell prompts or log lines
- everything about your NVIDIA licence: licence server addresses, client configuration tokens,
  licence files
- passwords, keys and any other secrets

PCI addresses such as `0000:01:00.0`, PCI IDs and version numbers are not private; keep them.

## Opening a report

Open the [compatibility report form](https://github.com/noon-at-cgn/geforce-sriov/issues/new?template=compatibility-report.yml), or choose **Issues → New issue → Compatibility report** on GitHub.
File one report per card model and host.
If you need a fix for a broken setup, also check [Symptoms](recovery.md#symptoms).

Use a **Bug report** for problems in geforce-sriov's tools or documentation that do not depend on hardware, such as an incorrect message or documentation error.

## What happens to a report

- Reports remain public issues with the `compatibility` label so readers with the same card can find them.
- `geforce-sriov devices` lists every card in the device table, and every listed card is supported ([Devices](devices.md#generations)).
  Maintainers decide whether a report warrants changes to the device table or documentation.
- Reports of failed setups are retained.
  Maintainers decide whether to document them.
- Support, fixes and response times are not guaranteed.
