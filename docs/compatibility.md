# Compatibility reports

A [compatibility report](https://github.com/noon-at-cgn/geforce-sriov/issues/new?template=compatibility-report.yml) records whether geforce-sriov works on your card and host.
Every report from a different machine helps show which cards and hosts work, and failed or partially working setups are just as useful as successes: choose *Does not work* or *Partially* in the form and say where it stopped.
Reports about other host or guest driver versions are welcome too.

[Supported cards](devices.md#status-of-every-row) shows the status of every card and driver version, and the [Ampere](ampere.md) and [Ada](ada.md) pages cover generation-specific details.
The recommended, tested versions are in the [supported software stack](installation.md#supported-software-stack). Other versions are not guaranteed to work.
[Community reports](community-reports.md) lists the setups people have reported so far.

Reports are voluntary. The form takes a few minutes and only a few fields are required.

## What to collect

Run these commands on the host.
Replace `0000:01:00.0` with your card's address from `lspci -nn | grep -i nvidia`.
In a source checkout without an install, use `./geforce-sriov` instead of `geforce-sriov`.

Two IDs identify your card:

- A PCI ID such as `10de:2684` is `vendor:device`: `10de` is NVIDIA, and the device ID identifies the card model.
- The subsystem ID identifies the board maker's variant of that card.

| What | Command | What to copy |
|---|---|---|
| PCI ID and subsystem ID | `lspci -nnv -s 0000:01:00.0` | The `[10de:....]` at the end of the first line, and the ID on the `Subsystem:` line |
| Device table row | `geforce-sriov devices` | Whether your ID is listed (every listed card is supported) |
| PCIe path to the GPU | `lspci -tv` | Whether the card is attached to a CPU root port, the chipset or a PCIe switch |
| Host kernel | `uname -r` | The whole line |
| NVIDIA vGPU host driver | `modinfo -F version nvidia` | The whole line |
| geforce-sriov version | `geforce-sriov --version`, and `git rev-parse --short HEAD` in a checkout | Both |
| `doctor` output | `sudo geforce-sriov doctor` | The whole output |
| `status` output | `sudo geforce-sriov status` | The whole output |
| Kernel log lines | `journalctl -k -b --no-hostname \| grep -E 'geforce_sriov\|NVRM\|AER'` | Only these lines, not the whole log |

For a problem from the previous boot, use `-b -1` instead of `-b`. That needs a persistent journal.
Without the systemd journal, `sudo dmesg | grep -E 'geforce_sriov|NVRM|AER'` shows the same lines for the current boot.
[Logs](recovery.md#logs) lists other log sources.
In those lines:

- `NVRM` lines are NVIDIA driver messages.
- `AER` lines are PCIe error reports.

For each guest, note:

- the guest OS,
- its vGPU type (NVIDIA also calls it a profile), which is set on its VF (for example `RTXA6000-4Q`, where `4Q` means 4 GB), and
- the version of the NVIDIA vGPU guest driver installed in it.

## Remove private data before posting

Issues are public, so remove or replace the following before you paste any output:

- IP and MAC addresses
- serial numbers of the GPU, the board or any other part, and GPU UUIDs (`GPU-…`)
- host names and user names, for example in shell prompts or log lines
- everything about your NVIDIA licence: licence server addresses, client configuration tokens, licence files
- passwords, keys and any other secrets

PCI addresses such as `0000:01:00.0`, PCI IDs and version numbers are **not** private. Keep them, because they are what makes the report useful.

## Opening a report

Open the [compatibility report form](https://github.com/noon-at-cgn/geforce-sriov/issues/new?template=compatibility-report.yml), or choose **Issues → New issue → Compatibility report** on GitHub.
File one report per card model and host.
If you need a fix for a broken setup, check [Symptoms](recovery.md#symptoms) as well.

Use a **Bug report** instead for problems in the geforce-sriov tools or documentation that do not depend on hardware, such as an incorrect message or a documentation error.

## After you submit

- Reports stay public as issues with the `compatibility` label, so people with the same card can find them.
- `geforce-sriov devices` lists every card in the device table, and every listed card is supported ([Supported cards](devices.md#generations)).
  Maintainers decide whether a report leads to changes in the device table or the documentation.
- Reports of failed setups are kept. Maintainers decide whether to document them.
- Support, fixes and response times are **not** guaranteed.
