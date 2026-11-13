# Community reports

This page collects the [compatibility reports](compatibility.md) that people have filed for their own cards and hosts.
Each row links to the full report, which has the complete output that the reporter attached.
Reports from 0.2.x include output of `doctor`, which no longer exists. `status` and `install --dry-run` now run its checks.

Each report describes the version of geforce-sriov that the reporter ran. A report against an older version may not match the current version.
For the cards and driver versions that geforce-sriov lists, see [Supported cards](devices.md#status-of-every-row).

## Summary

| Report | Card | Host | Host kernel | geforce-sriov | Result |
|---|---|---|---|---|---|
| [#1](https://github.com/noon-at-cgn/geforce-sriov/issues/1) | RTX 4070 Ti SUPER 16 GB (Ada, `10de:2705`) | Unraid 7.1.4 | 6.12.24-Unraid | 0.2.0 | Works |
| [#2](https://github.com/noon-at-cgn/geforce-sriov/issues/2) | RTX 3080 20 GB (Ampere, `10de:2206`) | Proxmox VE 9.2 | 7.0.14-28-pve | 0.2.1 | Works after a clean driver reinstall |
| [#4](https://github.com/noon-at-cgn/geforce-sriov/issues/4) | RTX 3090 24 GB (Ampere, `10de:2204`) | Proxmox VE 9.0 | 7.0.14-28-pve | 0.2.1 | Works |

All three reports use NVIDIA's vGPU host driver 580.159.01 with the card attached directly to a CPU root port.

## Report details

### #1: RTX 4070 Ti SUPER on Unraid

- Setup: one NVIDIA GPU, which is also the boot display card, and 2 VFs (`vfs=2`).
- Guests: Windows 11 with the `RTX6000-Ada-6Q` vGPU type and guest driver 582.53. The type ID is 945.
- Result: everything in the checklist worked. This included a CUDA or graphics program in a guest and restarting one VM while the others kept running.
- Notes: Unraid has no DKMS, compiler or kernel headers on the running host. The reporter built the kernel module in a container and replaced the boot unit with Unraid's own boot script.
  The 8 warnings in its `doctor` output come from that custom setup. They are not a fault in the stack.

### #2: RTX 3080 on Proxmox VE

- Setup: one NVIDIA GPU, not the boot display card.
- First result: `install` stopped at the driver step on both 7.0 kernels the reporter tried, 7.0.14-28-pve and 7.0.2-6-pve. NVIDIA's installer had built the **open** flavour of its kernel module. Reinstalling with the proprietary flavour on top of it still failed the check.
- Fix: the reporter removed everything with `nvidia-uninstall` and `geforce-sriov uninstall`, rebooted, and reinstalled NVIDIA's package with `--kernel-module-type=proprietary`. After a cold power cycle, `setup` completed and 4 VFs came up on 7.0.14-28-pve.
- Since 0.2.2, `install` runs NVIDIA's installer with `--kernel-module-type=proprietary` itself ([#3](https://github.com/noon-at-cgn/geforce-sriov/pull/3)).

### #4: RTX 3090 on Proxmox VE

- Setup: one NVIDIA GPU, not the boot display card, and 1 VF (`vfs=1`) with vGPU type 529.
- Guests: Windows 11 with the `RTXA6000-8Q` vGPU type and guest driver 582.53.
- Result: `doctor` reported no failures and no warnings. The VM started and ran a CUDA or graphics program.
- Notes: NVIDIA's installer first chose the open flavour, which failed the digest check. Forcing the proprietary flavour fixed it, and the reporter contributed that fix ([#3](https://github.com/noon-at-cgn/geforce-sriov/pull/3)).

## What the reports show

- NVIDIA's installer can pick the open flavour of its kernel module on these cards. geforce-sriov needs the proprietary flavour, and from 0.2.2 `install` asks for it.
- A host that once had the open flavour installed may need a clean reinstall of NVIDIA's driver before `install` succeeds.
- A boot display card on Unraid works with a custom boot script in place of the boot units.

## Keeping this page current

Maintainers add one row and a short entry for each new [compatibility report](https://github.com/noon-at-cgn/geforce-sriov/issues?q=label%3Acompatibility), including reports of setups that do not work.
Personal details stay in the issue: this page names each report only by its number.
To add your own setup, [file a compatibility report](compatibility.md).
