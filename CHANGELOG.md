# Changelog

## [0.4.0] - 2026-11-13

This release gives Ada VMs a better way to get video memory and lets the GPU draw the host desktop. It also includes the fixes prepared as 0.3.1, which was not released.

### Upgrading from 0.3.0

Upgrade if you use an Ada card or if the boot display GPU is one of your selected cards. To upgrade, run `sudo geforce-sriov uninstall` from 0.3.0, power the machine off and on, then install 0.4.0. See [Upgrading](docs/installation.md#upgrading). To go back, do the same steps in the other direction.

### Breaking changes

- Every upgrade uses the same steps: uninstall the installed version, power the machine off and on, then install the new version.
- Commands stop before they change anything when another version is installed, and DKMS no longer replaces another registered version.
- The Ada framebuffer slot reservation of 0.3.0 and its setting are gone, and a leftover `fb_slots=` line is ignored.
- Ada VM video memory replaces the slot reservation.
- `doctor` is no longer a command, and its checks now run in `status` and in `install --dry-run`.
- The old 0.2.x names and the migrations that handled them are gone.

### Added

- An Ada VM takes its video memory from free memory when it starts, so a host program can no longer keep a stopped VM from starting. See [VM video memory](docs/ada.md#vm-video-memory).
- The 1 GiB vGPU type works on Ada.
- `install --dry-run` checks your host and changes nothing.
- The GPU can draw the host desktop on X11. See [Host desktop](docs/installation.md#host-desktop).
- A new kernel is built for automatically on most systems. See [After a kernel update](docs/installation.md#after-a-kernel-update).
- Up to 32 cards can be selected, up from 8.
- The commands refuse to run when a different card sits at a selected address.
- `install` run from the installed copy can change the card selection and the display setting.
- Other host driver versions are built on a best-effort basis, but they are not guaranteed to work.

### Changed

- The activation timeout now follows the number of cards and VFs.
- Guest autostart waits for activation but is never blocked by it.
- With an Ada card, `install` stops when it cannot make the video memory change, and it installs nothing.
- `uninstall` is safer about restoring NVIDIA's module and keeps every backup when something looks wrong.

### Known limitations

- A VM needs one free piece of video memory of its size. A full or fragmented Ada card refuses the VM, not the host program.
- For an Ada card, use host driver 580.159.01. See [Ada](docs/ada.md#known-limits).
- With the host desktop on, a VM card is not completely free of the desktop. See [Host desktop](docs/installation.md#host-desktop).
- The host desktop feature is for X11 only.
- More than 4 VFs per card is not available.

## [0.3.0] - 2026-11-12

This beta release lets the boot display GPU serve VFs, adds host CUDA, and replaces `prepare` with `activate`.

### Breaking changes and how to migrate

- `prepare` is replaced by `activate`, and `--clear-guard` is now `--clear-lock`, so use the new names.
- `--release-console`, `--boot-display-release`, `--hold-nvidia` and `--hold-note-shown` are gone, so drop them and use `--display` or `--no-display` to choose the display build.
- The `boot_display_release=` config line is gone, so run `install` once to remove it, or delete the line yourself if you edited the config.
- Obsolete units and files are removed by `install`, and a file or unit you changed is kept or disabled with a message.
- After a kernel update, run `sudo geforce-sriov rebuild` instead of `install`. See [After a kernel update](docs/installation.md#after-a-kernel-update).

### Added

- The boot display GPU can serve VFs while its console stays on. See [Boot display GPU](docs/installation.md#boot-display-gpu).
- Host CUDA lets CUDA programs run on the host. See [Host CUDA](docs/installation.md#host-cuda).
- On Ada, a stopped VM keeps its video memory.
- An early boot unit loads the livepatch and NVIDIA's driver in a fixed order.
- `rebuild` builds the modules again after a kernel update.
- `check --daemon-ready` and guest autostart drop-ins start guests only when their VFs are ready.
- Other host driver versions can be tried on a best-effort basis.

### Changed

- `activate` is now the one command that brings the GPUs up.
- `status` exits 0 only when everything is ready.
- `activate` refuses a laptop GPU without D3cold consent. See [Consent](docs/laptops.md#consent).
- `setup` exits 0 after an install that needs a power cycle.

### Removed

- `prepare`, its unit and the old console release options.

### Known limitations

- Blackwell is not expected to work.
- Host driver versions other than 580.159.01 are not guaranteed to work.
- On an Ada card, the reserved slots limit host programs. See [Ada](docs/ada.md#known-limits).

## [0.2.2] - 2026-11-10

This release makes the driver step stricter about which NVIDIA module flavour it accepts.

### Added

- The driver step refuses an installed open-flavour NVIDIA module and explains how to install the proprietary one.

### Changed

- NVIDIA's installer now receives the proprietary module type (thanks to @Neko-0v0, PR #3).

## [0.2.1] - 2026-11-08

This release adds laptop GPU support and makes several messages and removals safer.

### Added

- 60 Ampere and Ada laptop GPUs are supported. See [Laptop support](docs/laptops.md).
- D3cold consent and power guards for laptop GPUs.
- `setup` shows a notice and asks permission before it creates the NVIDIA module hold file.

### Changed

- Several messages are clearer, and `uninstall` is more careful with files you changed.
- `install` removes other geforce-sriov versions from DKMS.

## [0.2.0] - 2026-11-07

This release adds Ada support, guided setup and automatic activation at boot.

### Added

- `install` and `setup` can install NVIDIA's vGPU host package and patch its driver. See [What install does](docs/driver-patch.md).
- `setup` runs the required steps and guides you through the power cycle.
- `activate` and `check` bring up and verify all selected cards, and a boot unit runs `activate` once per boot.
- Ada has the same support level as Ampere. See [Ada](docs/ada.md).
- `geforce-sriov-patch ada` applies the Ada patches offline.
- More workstation cards and GitHub issue forms for reports. See [Compatibility reports](docs/compatibility.md).
- `type_ada` and `type_ampere` choose the vGPU type. See [Choosing a vGPU type](docs/configuration.md#choosing-a-vgpu-type).
- Guest driver steps are documented in [Guests](docs/installation.md#guests).

### Changed

- Each generation has one vGPU target.
- MSI-X is exposed on Ada as well as Ampere.
- Ada guests can use more of their profile.
- Messages tell you to power the machine off and on instead of rebooting.

### Removed

- `enable`, because `activate` creates the VFs.
- Blackwell device IDs and the `experimental_vfs` option.

### Known limitations

- Loading `nvidia` binds every NVIDIA card on the host, so select every card a mixed host uses.
- Blackwell is not expected to work.

## [0.1.0]

Initial release with the `geforce-sriov` command, the livepatch module, the identity hook and the offline patcher.

### Added

- Commands to check, install, prepare and remove the setup.
- Support for 38 Ampere, Ada and Blackwell GeForce IDs.
- Optional console release for the boot display GPU.
- A VF count setting, 4 by default.

### Known limitations

- Hardware ARI decoding does not work, so the VF count defaults to 4.
- NVIDIA's vGPU host driver and guest licences are not included, so you must supply both.
