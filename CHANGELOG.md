# Changelog

## [0.4.0] - 2026-11-13

### Upgrading from 0.3.0

An upgrade is recommended in two cases.
An Ada card benefits from the new VM video memory. A VM takes free video memory when it starts. A host program no longer keeps a stopped VM from starting.
A host whose boot display GPU is a selected card benefits from the display build. The GPU then draws the host desktop on X11.
To upgrade, run `sudo geforce-sriov uninstall` from 0.3.0, power the machine off and on, then install 0.4.0. See [Upgrading](docs/installation.md#upgrading) for the steps.
To go back, uninstall 0.4.0, power the machine off and on, and install 0.3.0.

0.4.0 also carries the fixes prepared as 0.3.1, which was not released.

### Breaking changes

- Every upgrade takes the same steps. Run the installed version's `sudo geforce-sriov uninstall`, power the machine off and on, then install the new version.
  `install`, `setup` and `uninstall` stop before they write anything when a geforce-sriov of another version is installed.
  They look for the installed program, the build record of a module that is still installed, the DKMS registration and an enabled boot unit.
  Installing the same version again, `rebuild`, `install --device` from the installed copy and the kernel hook work as before.
  `install` writes the version in `/var/lib/geforce-sriov/format`, and `uninstall` removes it as its last step.
- The Ada framebuffer slot reservation of 0.3.0 is gone. This removes the config key `fb_slots`, the owner built into `nvidia.ko`, `/proc/driver/nvidia/geforce-sriov-fb`, the `--fb-owner` and `--no-fb-owner` options of the driver step, and the `framebuffer slots` line of `status` and `check`.
  A `fb_slots=` line left in a config is ignored, and `install` says so once.
  The Ada VM video memory change (see below) replaces it.
- `doctor` is no longer a command.
  Its checks run in `status` after install (a FAIL line sets exit status 1) and in `install --dry-run` and `setup` before install.
- The migrations of 0.2.x names and files are gone, and so are the old option, command and key names of 0.2.x. See Removed.
  DKMS no longer replaces another registered version.

### Added

- Ada VM video memory: an Ada VM takes its video memory from free memory when it starts.
  Before this change an Ada card gave each VF a fixed slot at the bottom of the card. A host program in the slot of a stopped VM kept that VM from starting.
  When an Ada card is selected, `install` makes one more change to NVIDIA's host module. The card then reports `Homogeneous Placements: Not Supported`. The VFs have no `placement_id`. A VM takes one contiguous free piece of video memory of its size, as on Ampere. See [VM video memory](docs/ada.md#vm-video-memory).
  There is no setting for it.
  `status` and `check` print one `VM video memory:` line for each selected Ada card that has VFs enabled. The line tells whether the loaded module carries the change.
  A VM needs one contiguous free piece of video memory. A full or fragmented card refuses the VM, not the host program. The VM starts again once memory is freed.
- The 1 GiB vGPU type (941) works on Ada: the guest sees 1024 MiB and runs CUDA.
- `install --dry-run` checks this host for a selection and for feature and package options, and changes nothing.
  It takes no lock, starts no child process, and exits 3 when a line is a FAIL.
  `setup` runs it as its first step.
- The host desktop is drawn by the GPU.
  With the display build and NVIDIA's second driver package, `install` also installs NVIDIA's graphics libraries into geforce-sriov's own folders. These are the X driver, GLX, EGL, GBM and Vulkan. The driver build also accepts programs that use those libraries.
  The new key `host_desktop` (`auto`, `on`, `off`) and the options `--host-desktop` and `--no-host-desktop` control it.
  The X file `geforce-sriov-nvidia-drm-outputclass.conf` sets `ProbeAllGpus` to `false`. The X log then shows that X probes only the GPU it draws on. This covers X11 only.
  Xorg and the window manager still keep the device node of the other card open. A VM card is therefore **not** completely free of the desktop. See [Host desktop](docs/installation.md#host-desktop).
- A new kernel is built for automatically.
  On apt-based hosts `install` adds a hook to `/etc/kernel/postinst.d` and `/etc/kernel/header_postinst.d`. The hook runs `geforce-sriov rebuild --kernel RELEASE` for a new kernel.
  `install` also adds an apt snippet, `/etc/apt/apt.conf.d/99geforce-sriov`. The snippet builds after apt for every installed kernel that has headers and no build yet. When NVIDIA's own module is missing, it asks DKMS for that module first. The headers package of Proxmox does not run the headers hook, so the snippet covers that case.
  The first apt run after `install` builds for each such kernel, a few minutes per kernel. Headers and kernel image can be installed in either order.
  Hosts with systemd `kernel-install` get a plugin, `/etc/kernel/install.d/90-geforce-sriov.install`, which builds when the kernel is added. Install the kernel headers package with or before the kernel.
  Hosts with pacman get a hook, `/etc/pacman.d/hooks/90-geforce-sriov.hook`. It builds for every installed kernel that has headers and no build yet, in either order.
  On any other system, run `sudo geforce-sriov rebuild --kernel RELEASE` after a kernel update.
  The hooks never fail the kernel package, apt, `kernel-install` or pacman. When a build fails, `status` shows the kernel and the command that fixes it.
- The limit is now 32 cards, up from 8, in the config, in `--device`, in the module parameter and in the hook.
- The card at a selected address must be the card that was selected.
  `install` records the cards in `/var/lib/geforce-sriov/selection`. `activate`, `check`, `check --daemon-ready` and the early boot unit refuse to run when another card sits at an address. They write nothing to any GPU.
  The message prints the command that selects the cards that are there now.
  Two identical boards cannot be told apart.
- `install` run from the installed copy accepts these selection changes: `--device`, `--disable-d3cold`, `--display` and `--no-display`.
  It copies no file. It builds NVIDIA's modules again only when the change needs another build, and it says so.
- `activate` refuses to release the console when the boot display GPU is selected and the installed build has no display modules.
- `install` can find the code it patches in a host driver version it has no record of. It then builds that version on a best-effort basis. Such a version is **not guaranteed to work**.

### Changed

- `install` writes the start timeout of `geforce-sriov-activate.service` from the number of selected cards and VFs, with a minimum of 900 s. The timeout is an upper bound for activation.
  Activation also extends it with `EXTEND_TIMEOUT_USEC` before each step that can wait.
  Normal activation takes seconds per card, so the value matters only when activation hangs.
- Guest autostart is ordered after activation and never blocked by it.
  The drop-ins for `pve-guests.service` and `libvirt-guests.service` no longer require the activate unit. `check` runs first and cannot fail the unit.
  A guest can start with a VF that is not ready and then fail inside the guest.
- With an Ada card selected, `install` stops when it cannot find the code it changes for Ada VM video memory. The change is made for 580.159.01.
  It names the recommended driver version, installs nothing and does not fall back to fixed slots.
- `uninstall` puts NVIDIA's module back when the backup is current. It fails and keeps every backup when the installed module is a changed copy of the module `install` built.
  After NVIDIA's package replaced `nvidia.ko`, `install` keeps the new stock module as the backup.
- On a driver version with no recorded digests, `install` applies both Ada plugin rows and records the digest of the full copy.

### Removed

- The Ada framebuffer slot reservation and its settings, as above.
- The 0.2.x and 0.3.0 migration code, as above.
- `doctor` as a command, as above.
- The old option, command and key names are now plain unknown names:
  - `prepare`
  - `--clear-guard`
  - `--hold-nvidia`
  - `--hold-note-shown`
  - `--release-console`
  - `--boot-display-release`
  - `boot_display_release`

  Each prints the generic `unknown command`, `unknown option` or `unknown key` message and exits 2.

### Known limitations

- A full or fragmented Ada card refuses the VM, not the host program. A VM needs one contiguous free piece of video memory of its size.
  With 2112 MiB free in one piece, a 2 GiB VM starts. With two pieces of 1056 MiB, it does not.
  `nvidia-vgpu-mgr` then logs `Failed to alloc guest FB memory`, `NVOS status 0x51` and `vmiope_process_configuration failed with 0x1a`. The VM starts again once memory is freed.
- With an Ada card selected, use host driver 580.159.01. On another version `install` stops and installs nothing unless it finds the code it changes for VM video memory.
  To get fixed slots back, uninstall, power the machine off and on, and install 0.3.0. There is no setting for it.
- With the host desktop on, Xorg and the window manager still open the device node of every NVIDIA GPU, including the Ada card reserved for VMs. The X log still shows that only the display GPU is probed (`ProbeAllGpus` is `false`).
  The Ada card shows 1 MiB used with the desktop up; with `host_desktop=off` it shows 0 MiB.
- On the vGPU host driver, `nvidia-smi` on the host lists the vGPU processes of a GPU but not the host CUDA processes. Their memory is still counted in `memory.used`.
- Host driver versions other than 580.159.01 are not guaranteed to work.
- The host desktop feature is for X11. Only the X driver reads `ProbeAllGpus`.
- More than 4 VFs per card is not available.

## [0.3.0] - 2026-11-12

### Added

- The boot display GPU can now serve VFs while its console stays on.
  `activate` hands the firmware framebuffer to NVIDIA's display modules, so you need no console release and no manual step after boot.
  Display output goes only to the boot display GPU, even with `display=on`, and `display=on` builds the display modules only when a selected GPU is the boot display.
  The display modules (`nvidia-modeset`, `nvidia-drm`) are built from a second NVIDIA driver package (the 580.159.03 vGPU Linux guest `.run`, or the GeForce 580.159.03 package) and NVIDIA's public kernel module source at tag 580.159.03 ([Boot display card](docs/installation.md#boot-display-card)).

- Config keys `display` (`auto`, `on`, `off`) and `host_cuda` (`on`, `off`), with `install` and `setup` options `--display`, `--no-display`, `--host-cuda` and `--no-host-cuda`.
  `--companion-file PATH` or `--companion-source DIR` name the second driver package, and `--nvkms-source PATH` names the kernel module source.
  `doctor` takes the same options to judge the build.
  `setup` asks you for the second driver package and the kernel module source when a selected GPU is the boot display, and asks whether to enable host CUDA.

- Host CUDA: with `host_cuda=on`, `install` builds `nvidia-uvm`, installs NVIDIA's CUDA library (`libcuda`) and the libraries it loads from the second driver package, and writes `geforce-sriov-uvm.conf`, so CUDA programs can run on the host ([Host CUDA](docs/installation.md#host-cuda)).
  The libraries go into `<prefix>/lib/geforce-sriov/cuda/`, which `install` adds to the library search path; a CUDA library from another package is left as it is.
  Host CUDA with the display build on 580.159.01 is a confirmed combination.

- Ada: a stopped VM keeps its video memory.
  On an Ada card each VF uses a fixed range of the card's VRAM, its framebuffer slot. Until now a host program, for example a host CUDA job, could take the slot of a stopped VM, and that VM then failed to start.
  `install` now builds a small piece of code into your copy of `nvidia.ko` that holds the slot of every VF from activation on and hands it to NVIDIA's vGPU manager when the VM starts.
  `status`, `doctor` and `check` show a `framebuffer slots` line for each Ada card, and the new config key `fb_slots` (`auto`, `on`, `off`) turns it off.
  The slots stay reserved for the VFs while their VMs are stopped, so host programs on that card get only the memory outside them.

- An early livepatch unit, `geforce-sriov-livepatch.service`, loads the livepatch module `geforce_sriov` and then NVIDIA's driver (`nvidia`) before udev, using the new `livepatch --boot --load-driver` command.
  `geforce-sriov-autoload.conf` keeps udev from loading the driver by itself, and `install` writes it exactly when a GPU is selected ([Modprobe files](docs/installation.md#modprobe-files)).

- `activate --stop-display-manager` lets `activate` stop a running display manager and start it again afterwards.
  `setup` asks first, and the boot unit never stops it.

- `activate --clear-lock` clears the activation lock after a cold power cycle.

- `rebuild [--kernel RELEASE]` builds NVIDIA's patched module (`nvidia.ko`) and plugin again after a kernel update, for the running kernel or for one that is installed and not running yet.
  `install` keeps the second NVIDIA driver package and NVIDIA's public kernel module source it was given, so `rebuild` needs no options, and `uninstall` removes them and restores NVIDIA's `nvidia.ko` for every kernel it built for.
  `install` run from the installed copy stops and names `rebuild` ([After a kernel update](docs/installation.md#after-a-kernel-update)).
  `rebuild` also makes sure the livepatch module `geforce_sriov` is installed for that kernel and builds it with DKMS when it is not, so it also prepares a kernel that was installed before geforce-sriov.

- `check --daemon-ready`, run by the `nvidia-vgpud` drop-in, starts the daemon only when the selected VFs are enabled and bound.
  Drop-ins for `libvirt-guests.service` and `pve-guests.service` order guest autostart after activation and run `check` first.

- Other host driver versions can be tried on a best-effort basis, on the plain build as well as the display build.
  `install` names the version and carries on only if its patches still apply to it.
  `setup` asks first (the default is no), and `--allow-unsupported-driver` answers yes without asking.
  Host driver 580.159.01 is the version confirmed to work.

### Changed

- `activate` is now the one command that brings the GPUs up.
  It loads the livepatch when needed, hands off the boot display's console, removes and rescans the empty bus of a card that needs it, creates the VFs, restarts NVIDIA's daemons, re-probes each GPU and sets the types.
  The activation lock file keeps its name, `/var/lib/geforce-sriov/prepare-attempt`.

- With `geforce_sriov.skip` on the kernel command line, both boot units are skipped and no NVIDIA driver loads that boot.
  Run `modprobe nvidia` if you need it.

- `uninstall` also removes the three modprobe files and the guest-autostart drop-ins, and disables the 0.2.x prepare unit if one remains.
  A 0.2.x hold file is removed by `install` when unchanged.

- `status` exits 0 only when every selected VF is ready, the activation lock is clear and the installed build matches `display` and `host_cuda`.

- `activate` refuses a selected laptop GPU unless the config records D3cold consent (`d3cold=off`), before it writes anything ([Consent](docs/laptops.md#consent)).

- `activate` takes NVIDIA's `unbindLock` before it removes a card that is bound to `nvidia`, and refuses if a program still holds the GPU.
  `geforce-sriov-activate.service` is ordered before `nvidia-persistenced.service`.

- `setup` exits 0 after an install that finished but needs a power cycle, and it tells you so.

- `install` checks the SHA-256 of an NVIDIA `.run` package that is named as the recorded version before it runs the package, and if a commit of the driver build (the rebuilt `nvidia.ko` and the files that go with it) gets cut off, the next run can finish it.

### Removed

- `prepare`, replaced by `activate`; `prepare --clear-guard` is now `activate --clear-lock`.
- `--release-console`, `--boot-display-release`, `--hold-nvidia` and the `boot_display_release` key, which a config can no longer contain.
  `install` removes the line from a config it wrote.
- The prepare unit `geforce-sriov-prepare.service` and the deferral file `/run/geforce-sriov/deferred`.

### Breaking changes and how to migrate

0.3.0 is a beta release, so some 0.2.x names are gone.
Each old name either stops with a message that names its replacement, or is migrated by the next `install`.

- `activate --clear-guard` (in 0.2.x, `prepare --clear-guard`) is now `activate --clear-lock`.
  The old name exits 2 and prints the new one. The file itself is still `/var/lib/geforce-sriov/prepare-attempt`.
- `geforce-sriov prepare` is replaced by `activate`.
  `prepare` exits 2 with `prepare is no longer supported; use activate`.
  `install` replaces the 0.2.x boot units.
- `--boot-display-release`, `--release-console`, `--hold-nvidia` and `--hold-note-shown` are gone.
  Each exits 2 with `<option> is no longer supported; use activate and display=auto|on|off`.
  Drop the option and use `--display` or `--no-display` to choose the display build.
- The config key `boot_display_release=` is gone.
  The next `install` removes the line from a config that `install` wrote and says so.
  For a config you edited, `install` stops and names the line, and the other commands name it too.
  Run `install` once, or delete the line.
- `geforce-sriov-prepare.service`, `/etc/modprobe.d/nvidia-manual-load.conf` and `/run/geforce-sriov/deferred` are gone.
  The next `install` removes each one that you did not change and prints `removed obsolete <file>`.
  A changed file is kept, and a changed unit is disabled, with a line that says so. Run `install` once.
- After a kernel update, run `sudo geforce-sriov rebuild` instead of `install`.
  `install` run from the installed copy stops and names `rebuild`.
- Exit status 5 and the "recovery guard" are now called the activation lock.
  Only the wording changed, so nothing needs migrating.

### Known limitations

- Blackwell is not expected to work with the current implementation.
- Host driver versions other than 580.159.01 are not guaranteed to work.
- The display modules and host CUDA need a second NVIDIA driver package, and this project does not include or download it.
- On an Ada card, the framebuffer slots of the VFs stay reserved, so host programs on that card can use only the memory outside them: about 4 of 12 GiB on an RTX 4070 SUPER with four VFs of the default type ([Known limits](docs/ada.md#known-limits)).
- Holding the framebuffer slots is tested with host driver 580.159.01. With another host driver version it is best effort, and `install` builds NVIDIA's module without it when its checks do not pass.
- On an Ada card, vGPU type 941 (RTX6000-Ada-1Q) does not work: the guest driver does not initialise. Use type 942 or a larger one ([Choosing a profile](docs/ada.md#choosing-a-profile)).

## [0.2.2] - 2026-11-10

### Added

- The driver step refuses an installed NVIDIA open-flavour module and explains how to install the proprietary flavour.
  In 0.2.1 it warned and continued to a module `.text` size-mismatch refusal.

### Changed

- With `--driver-file`, NVIDIA's installer receives `--kernel-module-type=proprietary`; the printed and manual commands include it too (thanks to @Neko-0v0, PR #3).

## [0.2.1] - 2026-11-08

### Added

- `setup` now displays an IMPORTANT notice and asks for permission before creating the NVIDIA module hold file.
  The notice explains that releasing the boot display GPU disconnects its local console, recommends testing SSH or serial console access before the power cycle, and explains that this path requires manual setup after every boot.

- `install` and `setup` now accept `--hold-nvidia`, which creates and tracks the hold file.
  Uninstall removes the file if it is unchanged.

- `doctor` also accepts `--hold-nvidia`.
  With `--hold-nvidia` and a console-release grant, it reports a missing hold file as information rather than a warning, because `install` writes it.

- 60 Ampere and Ada laptop and embedded GPUs are in the device table and go through the same path as the desktop cards ([Laptop support](docs/laptops.md)).
  25 have public config-space dumps.
  The other 35 are listed because every laptop GPU with a config-space dump for their generation has the desktop layout.

- D3cold consent for laptop GPUs ([Laptop support](docs/laptops.md#consent)).
  `setup` shows a notice and asks for consent; `setup --disable-d3cold` and `install --disable-d3cold` record it without asking the D3cold question.
  `install` records `d3cold=off` in the config; `doctor --disable-d3cold` assesses the host as if that consent were recorded.
  Without consent, `prepare` refuses the entire run if a laptop GPU is selected (exit 3), before changing anything.
  `install --device` without `--disable-d3cold` removes consent; `uninstall` removes it with the config if the config is unchanged since install.

- Laptop power guards (require D3cold consent for laptop GPUs):
  - `prepare` powers on the GPU and the bridge above it and blocks D3cold (`d3cold_allowed=0`).
    If the GPU reports active, a config read must succeed first.
    If the GPU does not answer within 5 seconds, `prepare` refuses the entire run (exit 3) and restores the power settings it changed on every GPU.
    It blocks D3cold again after the rescan and leaves the bridge on.
  - `activate` keeps D3cold blocked around the re-probe.
  - `check` requires each VF of a laptop GPU to answer a config read.
  - `doctor` writes nothing and fails a laptop GPU that has no consent or does not answer.
    It warns if `options nvidia NVreg_DynamicPowerManagement=0` is missing, if `sleep.target` is not masked, or if MSI-X or the SR-IOV state was lost to a power cut.

  geforce-sriov does not write the modprobe option or the sleep setting.

### Changed

- The final install message no longer says an existing hold was "already" present, since it may have been created during the same installation.

- When `nvidia` is already loaded, the refusal message now includes the hold-file path and its two required lines.

- Running `install --device …` without `--boot-display-release` after a console-release grant removes the hold file if it was created by `setup` or `install --hold-nvidia` and remains unchanged.
  Modified and manually created hold files are preserved and reported.

- `prepare` also wakes a desktop GPU whose `runtime_status` is `suspended`; if it does not answer within 5 seconds, `prepare` refuses the entire run (exit 3).
  A desktop GPU that is not suspended sees no change.

- `install` removes other `geforce-sriov` versions from DKMS after a successful trial build.
  It then removes their source trees under `/usr/src`, leaving one version registered.

- The hints for `setup`'s console-release questions and the note printed by `install --boot-display-release` warn that the monitor may go blank or freeze until the next boot.
  The grant hint and the install note recommend setting up and testing SSH or a serial console before the power cycle.

- `install --hold-nvidia` preserves edits to a hold file it wrote if the file blocks `nvidia` at boot.
  If the edited file no longer holds `nvidia` back, installation refuses (exit 3).

- `uninstall` runs `geforce-sriov-driver --restore` only if the script is unchanged since install.
  If the script changed, `uninstall` does not run it, exits 1, and gives instructions for restoring NVIDIA's module by hand.

## [0.2.0] - 2026-11-07

### Added

#### Installation and driver patches

- Install NVIDIA's vGPU host package through `install` or `setup` with `--driver-file PATH` and `--accept-nvidia-license`.
  Build with `make`, then run:

  ```sh
  sudo ./geforce-sriov setup --driver-file NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run --accept-nvidia-license
  ```

  Add `--device BDF` for multiple cards, when more than one card is eligible, or for a boot display card.
  Install kernel headers for the running kernel, `dkms`, `pciutils`, a C compiler and `make` with your package manager first.
  A live `install` refuses if any are missing and lists them in one sentence.

- `--driver-file` extracts the `.run` package into `/var/lib/geforce-sriov/nvidia/<version>/` and uses its `kernel/` tree.
  It copies missing `nvidia-vgpud.service` and `nvidia-vgpu-mgr.service` files to `/etc/systemd/system/`, and missing `sriov-manage` to `/usr/lib/nvidia/sriov-manage`.
  Combining it with `--nvidia-source` or `--driver=skip`, or using it with `DESTDIR` staging, is a usage error.
  Use `--nvidia-source DIR` for a kernel tree outside `/usr/src/nvidia-<version>`, such as an extracted package's `kernel/` directory.

- With `--driver-file`, `--accept-nvidia-license` prints the licence notice and runs NVIDIA's userspace installer as `sh PATH --silent --dkms --skip-module-load`.
  The module stays unloaded until the boot unit runs.
  The installer is skipped if that version's `libnvidia-vgpu.so` is already installed, and refused while `nouveau` is loaded.
  Without licence acceptance, the package is extracted, but missing userspace causes refusal with NVIDIA's installer command.

- Before writing anything, `install --driver-file` refuses an existing non-vGPU NVIDIA driver (no `libnvidia-vgpu.so*`) or a driver owned by distribution packages.
  It names the removal command.
  The driver step warns if the installed module is NVIDIA's open flavour.
  `setup` lists selected cards already bound to `nvidia` on one line and explains that preparation must wait for the power cycle.

- The driver step, `scripts/geforce-sriov-driver.sh`, is installed as `geforce-sriov-driver`.
  `install` runs it unless `--driver=skip` is given.
  It applies three host-object revisions and two plugin revisions to copies of your files, builds and installs the module, backs up the stock module, installs the patched plugin copy, and writes `/var/lib/geforce-sriov/driver-build` and the modprobe file.

- Driver inputs and patch results are checked against whole-file SHA-256 digests recorded for 580.159.01.
  The host object, stock plugin, revised object and each plugin revision must match their recorded digests; other files and driver versions are refused.
  After the build, the installed stock module and rebuilt module must differ at exactly these eight `.text`-relative offsets: `0x14a7c4`-`0x14a7c7`, `0xa16708`, `0xa16709`, `0xd8966f`, `0xd89670`.
  Any other difference causes refusal and reports the first unexpected offset.

- Identical `.text` means the installed module is already this build.
  The step writes the build record for this kernel and exits 0 only if the modprobe file, patched plugin copy and stock backup are in place, with the backup differing at exactly the eight expected offsets.
  Otherwise it refuses and asks you to reinstall the stock module, or use `--driver=skip` to keep the installed module.

- `--dry-run` prints the driver plan without applying it.
  Refusals before the first write exit 3; a later failure restores replaced files.
  `--restore`, also used by `uninstall`, restores the stock module only while the installed one is still the step's build, points `libnvidia-vgpu.so` back at the stock plugin and removes the modprobe file.

- `install` writes the `nvidia-vgpud` drop-in and `/etc/geforce-sriov/config` only after the driver step succeeds, or when using `--driver=skip`.
  Once anything has been written, a failure or refusal exits 1.
  Exit 3 means nothing changed.

#### Guided setup and boot activation

- `geforce-sriov setup` runs the required steps as child processes of the same binary.
  Run it before the first cold power cycle, then repeat the same command afterwards.
  On a fresh host it runs `doctor`, then `install`.
  On an installed host after boot, it runs `doctor`, `check`, `prepare --boot` if this boot has no prepared record, `activate --boot`, then `check` again.
  It stops early if every VF is already ready, or at the first failed step, returning that step's exit code.

- `setup` automatically selects a single eligible card that is not the boot display.
  If the host's only NVIDIA card is the boot display, it asks the console-release question for that card. Without a terminal, or with `--no-prompt`, it refuses before installing and names `--device` and `--boot-display-release`.
  On later runs, it notes and ignores the first-install options: `--driver-file`, `--vfs`, `--boot-display-release`, `--nvidia-source` and `--driver=skip`.
  A `--device` selection that differs from the configured cards is refused.
  It explains when to power the host fully off and on, lists each card's guest drivers, and leaves VM creation and VF attachment to you.

- `install` installs and enables `systemd/geforce-sriov-activate.service`.
  It runs `activate --boot` once per boot, after preparation and NVIDIA's two daemons, and before VM autostart.
  It is ordered before `libvirt-guests.service` and `pve-guests.service`, but requires no particular hypervisor.
  Its start timeout is 600 seconds.
  It does not run on the boot-display path while the deferred marker exists.

- `doctor` checks kernel headers, `cc`, `make` and NVIDIA's kernel source tree at `/usr/src/nvidia-<version>`.
  Missing headers, `cc` or `make` are failures before the running kernel's module is installed, and warnings afterwards because only a later `install` needs them.
  Host driver versions other than 580.159.01, which are not supported, produce a warning.
  `doctor` and `status` report whether the installed `nvidia.ko` matches the driver step's recorded build.
  `doctor --driver-file` and `doctor --boot-display-release BDF` check the host as installation will leave it; `setup` passes both through.

#### Ada and multi-GPU support

- [Ada](docs/ada.md) now has the same support tier as [Ampere](docs/ampere.md): four VFs per card, three host-object revisions (scheduler, probe gate and VF count), two plugin revisions, and the `activate` and `check` commands.
  Other host and guest driver versions are not supported.
  Blackwell GPUs are not expected to work.

- The Ada VF-count revision makes NVIDIA's driver report 4 total VFs instead of 1.
  It changes site `0x126644` in `_nv039075rm` (`kbifCacheVFInfo`) from `0f b7 45 06` to `31 c0 b0 04`, taking the object SHA-256 from `f5b788cd…` to `946b27b8…`.
  See [the full digests](docs/driver-patch.md#revisions).
  The revision applies to every TU/GA/AD card bound to the module, not just selected cards.

- `activate` and `check` handle every VF of every selected Ada card.
  Mixed-generation selections produce one `[WARN]` from `doctor`, but are not refused.
  Both generations share a host module, VF count and plugin copy ([combining generations](docs/devices.md#combining-generations)).
  [Mixed-host documentation](docs/devices.md#combining-generations) covers the shared driver stack, four VFs per card, required driver versions and startup order.

- Added workstation device IDs, handled like GeForce cards of the same generation with four VFs by default:
  - Ada AD104: `27b1` RTX 4500 Ada Generation and `27b2` RTX 4000 Ada Generation.
  - Ada AD107: `28b0` RTX 2000 / 2000E Ada Generation.
  - Ampere GA104: `24b0` RTX A4000.
  - Ampere GA106: `2571` RTX A2000 12GB.

- Added GitHub issue forms for compatibility reports, including failed setups, and software bugs.
  [Compatibility reporting](docs/compatibility.md) explains what to collect and redact.
  [Supported cards](docs/devices.md) lists each generation's target, default type, VF count and required NVIDIA driver flavour.

#### Activation and readiness checks

- `geforce-sriov activate [--type N] [--dry-run] [--boot]` configures all Ada and Ampere cards selected by `bdfs`.
  It runs as root and takes no `--device` option.
  Activation:
  1. Creates missing VFs and loads `nvidia_vgpu_vfio`.
  2. Restarts `nvidia-vgpud`, then `nvidia-vgpu-mgr`.
  3. Re-probes each selected PF under the guard and NVIDIA's `unbindLock`, using driver unbind and bind, never device removal or a bus rescan.
  4. Sets each VF's type from `--type N`, then `type_ada` or `type_ampere`, then the default: 942 (RTX6000-Ada-2Q, 2048 MiB) on Ada or 525 on Ampere.
  5. Writes `power/control=on`, verifies `on`/`active`, and prints the `check` result.

- Activation refuses unless the driver has `NVreg_EnableGpuFirmware=0` and `libnvidia-vgpu.so` resolves to the patched plugin copy.
  These two checks only warn on host driver versions other than 580.159.01; the version is reported as not supported and activation continues.
  Because the daemon restart affects the whole host, activation also refuses if any NVIDIA VF has a vGPU type, or any GPU outside `bdfs` has VFs enabled.
  An installed `nvidia.ko` that differs from the recorded build, including a record for another kernel, is refused.
  A missing record produces only a warning.

- `/run/geforce-sriov/activating` exists from the first activation change until every VF reads back `on`/`active`.
  If a type is absent from `nvidia/creatable_vgpu_types`, no type is written.
  The error explains: `The driver keeps the vGPU types it registered first after a boot, so after a change of identity or target, power the machine off and on`.
  With `--boot`, activation first waits up to 30 s for the driver version file and each selected PF to bind to `nvidia`.

- `geforce-sriov check [--device BDF]... [--type N]` is a read-only check before each VM start.
  It exits 0 only when every VF is on `nvidia`, has `power/control=on` and `runtime_status=active`, carries the expected type, and neither the guard nor `/run/geforce-sriov/activating` exists.
  Type precedence is `N` from `--type`, then `type_ada` or `type_ampere`, then 942 on Ada or 525 on Ampere.
  It does not read the plugin.

- An unfinished activation produces a warning in `doctor`:
  `/run/geforce-sriov/activating exists: geforce-sriov activate is running or did not finish; check reports NOT READY until activate succeeds`.
  `status` prints `activate: /run/geforce-sriov/activating exists (activate is running or did not finish)` and `` next: stop every VM that uses a vGPU, then run `geforce-sriov activate` ``, and exits 1.

- For each selected Ada card, `doctor` lists its required host stack and explains that `install` prepares it unless `--driver=skip` is given.
  [Ada documentation](docs/ada.md) covers the proprietary 580.159.01 host stack with GSP off, host-object and plugin revisions, VF power fix, guest drivers and recovery.

#### Offline Ada patcher

- `geforce-sriov-patch ada list|inspect|apply|revert` applies the Ada revisions offline to your own 580.159.01 `nv-kernel.o_binary` or `libnvidia-vgpu.so.580.159.01`, writing only `NEWOUTPUT`.
  Like the install step, it checks each revision against recorded SHA-256 digests.

- Patch sites and accepted inputs:
  - Probe gate: `0xd654ef`, `75 7f` → `90 90`; accepts the stock host object or one with the scheduler revision.
  - VF count: `0x126644`, `0f b7 45 06` → `31 c0 b0 04`; accepts an object with both scheduler and probe-gate revisions.
  - Plugin placement: `0x8fc17`, 15 bytes; its output is the input for the 6-byte framebuffer change at `0xa5f28`.
    The resulting plugin serves both Ada and Ampere.

- Apply revisions in order: scheduler (`geforce-sriov-patch apply`), probe gate (`ada apply`), VF count (`ada apply` again), then both plugin changes.
  The scheduler commands refuse an object that already has the gate revision.
  See [Ada revisions](docs/driver-patch.md#revisions).
  `ada inspect` and `ada apply` print a `row N: …` line.
  The plugin starts from stock `7e52b76d…` and ends at SHA-256 `aa4fdeb5…`.

#### Console release and driver diagnostics

- Existing files in `/etc/modprobe.d/` that set `NVreg_EnableGpuFirmware=0` are kept.
  The driver step writes no modprobe file of its own, and `--restore` and `uninstall` leave the existing file alone.
  Any other value causes refusal: `<file> sets NVreg_EnableGpuFirmware=<value>; this stack needs 0`.
  `doctor` reports the same conflict.
  With `boot_display_release` set, it also warns if `/etc/modprobe.d/` lacks the boot hold: `blacklist nvidia` and `install nvidia /bin/false`.

- After boot, `setup` can release the console when a grant exists, `check` is not ready and NVIDIA's driver is unloaded.
  It asks `Release the console of <BDF> and bring the GPUs up now? [y/N]`.
  Yes runs these commands in order, stopping on the first failure and returning its exit status:
  1. `geforce-sriov prepare --release-console=<BDF>`
  2. `modprobe --ignore-install nvidia`
  3. `geforce-sriov activate`
  4. `geforce-sriov check`

  No prints the commands and exits 0.
  `--no-prompt`, no terminal or end of input causes refusal with exit 3 and no changes:
  `setup: releasing the console needs a yes. Run setup on a terminal, or run these commands:`.

  `doctor`, `status`, the boot unit's deferral message and `install` print the same four commands under `After each boot, as root, in this order:`, followed by `or run geforce-sriov setup`.
  With a release grant, `install` prints the hold file and commands near the end of its output.

  Before first installation, `setup` asks for permission for any boot-display card selected at its prompt.
  The default is no, which excludes the card; the prompt also says `SSH keeps working`.

- Driver-step output is saved to `/var/lib/geforce-sriov/driver-step.log`, mode 0600, replaced on each run and removed by `uninstall`.
  Before starting, `install` prints `This may build NVIDIA's module for <kernel release>, which can take a few minutes; progress is in /var/lib/geforce-sriov/driver-step.log`.
  It then shows only `geforce-sriov-driver: …` lines, plus the last 40 log lines and log path on failure.
  The last displayed log line is the step's own refusal; only source-tree and vendor-object refusals mention `--nvidia-source`.
  Success ends with `geforce-sriov-driver: NVIDIA's module and plugin are prepared`.
  The installed plugin copy has mode 0755, including on reruns that find a prepared host but a copy missing that mode.

- Before its closing line, the driver step reports `geforce-sriov-driver: patched plugin copy installed: <path> (sha256 <first 16 hex digits>)`.
  `install` shows this with the step's other lines.

- `doctor`, `status` and `check` compare the loaded `nvidia` build-id from `/sys/module/nvidia/notes/.note.gnu.build-id` with the installed `nvidia.ko`, but only when the installed module is the driver step's build.
  A mismatch makes `doctor` warn `the running nvidia module (build-id <first 12 hex digits>) is not the installed one (<first 12 hex digits>); power the machine off and on to load it`.
  `status` adds the sentence to the card's `nvidia.ko` note and exits 1.
  `check` adds it to its `nvidia.ko` line without changing its exit status.
  If the installed module is not the driver step's build, none reports on the loaded module.

- `doctor` checks systemd's unit drop-in directories for `LD_PRELOAD` overrides in `nvidia-vgpud` and `nvidia-vgpu-mgr`.
  The package's own `geforce-sriov.conf` is excluded.
  Other files produce `[WARN] <file> sets LD_PRELOAD for <unit> and overrides the geforce-sriov identity hook. Remove it or point it at <hook path>.`
  In `nvidia-vgpud.service.d`, files read before `geforce-sriov.conf` are not reported because the last `LD_PRELOAD` wins.

- With neither a driver-step record nor an install manifest, including after uninstall, `doctor` prints `[INFO] the driver step has not run on this host; install runs it`.
  With a manifest but no record (`install --driver=skip`), or a record for another module or kernel, it instead says to rerun the driver step.

- `uninstall` restores the stock module, then removes the running kernel's backup in `/var/lib/geforce-sriov/stock/`, empty `build/` and `stock/` directories, and `/var/lib/geforce-sriov/` if empty.
  It prints `removed <path>` for each.
  Other kernels' backups remain, and a later install takes a fresh backup.
  If restoration fails, uninstall exits 1 and removes other installed files, but keeps `geforce-sriov-driver` and every backup.
  It prints `NVIDIA's module was not restored: <reason>` with two ways to complete restoration by hand.

#### Profiles and guest setup

- `type_ada=N` and `type_ampere=N` select the type that `activate` sets and `check` expects on each VF.
  Without them, the defaults are 942 on Ada and 525 on Ampere.
  Invalid values are refused with `line N: type_ada must be a vGPU type ID from 1 to 999999, as nvidia/creatable_vgpu_types lists it`.
  `config.example` and the installed config show both keys commented out unless set.

- After setting an Ada card's types, `activate` names VFs whose framebuffer slots end above 75% of the card's memory.
  For example: `0000:03:00.0: with type 946 the VFs 0000:03:00.5, 0000:03:00.6 and 0000:03:00.7 may not start: their framebuffer slots end near the top of the card's 16376 MiB. Put VMs on the lower VFs, or choose a smaller type.`
  VF k occupies `[k × (P − 32), (k + 1) × (P − 32))` MiB, where P is the type's `profileSize` in `/usr/share/nvidia/vgpu/vgpuConfig.xml`.
  Memory comes from `nvidia-smi`, which is killed after 5 s.
  This is advisory: the exit status is unchanged, missing either source produces no message, and Ampere cards get no message.

- Q profiles are listed in [Ada](docs/ada.md#choosing-a-profile) and [Ampere](docs/ampere.md#choosing-a-profile).
  [Choosing a vGPU type](docs/configuration.md#choosing-a-vgpu-type) explains how to change size.

- [Guests](docs/installation.md#guests) documents each card's guest drivers, where to set `RMSetVgpuGspPluginOffloadMode=0` on Linux and Windows, and unsupported pairs: Ada with a 580-series guest driver, or Ampere with 16.9 drivers.
  [Ada](docs/ada.md#guests-and-vms) and [Ampere](docs/ampere.md#guests-and-vms) link to those instructions.

### Changed

#### GPU support and profiles

- `msix=on` now exposes MSI-X on Ada as well as Ampere.
  Every Ada device-table row uses `0x41C` bit 29 as the hide bit; the module clears it at load, then `prepare` removes and rescans the card.
  A card reset hides MSI-X again until the next cold boot.
  Use `msix=off` to leave it hidden.

- The Ada plugin framebuffer change lets guests use more of their profile.
  Previously, each VF's host framebuffer was capped at the card's framebuffer divided by the type's `maxVgpus`, less reserved memory.
  For RTX6000-Ada-2Q, `maxVgpus` is 24.
  The 6-byte change at `0xa5f28` (`8b bb d0 29 00 00` → `bf 04 00 00 00 90`) uses 4, the VF count, as the divisor.
  Ampere never reaches this code.

  Applied after the placement change (`7ddfad45…`) to stock plugin `7e52b76d…`, it produces SHA-256 `aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c`.
  `geforce-sriov-patch ada` adds a fifth row, `plugin-div4`; the driver step applies both plugin rows in order and checks each digest.
  Rerunning `install` replaces placement-only copies, including on prepared hosts and when repairing the build record.
  `activate` accepts only the new digest and tells users with older copies to run `sudo geforce-sriov install` again.
  `doctor` checks the plugin link by the same rule: other files produce warnings after the driver step has run, and information beforehand.
  Messages and documentation call the result the patched plugin copy.

- Each generation now has one vGPU target, rather than a target per device-table row.
  Ampere uses RTX A6000 (`10de:2230`); Ada uses RTX 6000 Ada (`10de:26b1`).
  Both keep the board's own subsystem ID.
  Version 0.1.0 mapped some dies to other targets.
  Ada defaults to type 942 (RTX6000-Ada-2Q, 2048 MiB).

- `geforce-sriov devices` prints each generation's target and default type once, then the `DEVICE`, `GEN`, `DIE` and `NAME` columns for each row.
  It states once that every listed card is supported.
  `doctor` prints `-> vGPU target 10de:<id> <name>` per card, and the hook logs `presented as <name> (10de:<id>), subsystem kept`.
  Unlisted cards are refused by `doctor`, `install --device`, `setup`, `activate` and `check` with `<BDF> (10de:<id>) is not in the device table`.

- `activate`, including its boot-unit and dry-run paths, and `check` now share type precedence: `--type N`, then `type_ada` or `type_ampere`, then the generation default.
  Custom types survive boot-unit and `setup` runs.
  Mixed hosts can set a type per generation without `--type`.
  A refused mixed-generation `--type` selection lists the types a run without it would use and adds `Set type_ada= and type_ampere= in /etc/geforce-sriov/config to choose one per generation`.

- `geforce-sriov-patch` usage no longer mentions Blackwell.
  [Ada](docs/ada.md) documents the hardware VF count: 1 for every Ada row except `28b0`, which reports 16.
  [Ampere](docs/ampere.md) documents GeForce and workstation boards.

#### Console release and cold power cycles

- First-install console permission now asks `Allow releasing the console of <BDF> after the next boot? [y/N]`.
  Yes records the grant without releasing anything; `setup` asks again after the power cycle.
  `install --boot-display-release` prints `Console release: granted; nothing is released now.` and explains the later release, rather than describing one happening immediately.
  The post-boot question is unchanged.

- After a live install that enables boot units, the next step is: run `doctor`, power the host fully off and on, then run `sudo geforce-sriov setup`.
  With console release granted, use the listed commands or `setup`.
  The message no longer suggests a reboot or says that `status` names the next step.
  `setup` adds only that the same command works after boot.

- Before a prepared boot, `status` now says `power the machine off and on`, not reboot.
  `uninstall --unload` refusals end with `disable the VFs, or power the machine off and on instead` or `power the machine off and on instead`.
  `prepare` refuses a module already loaded with other parameters with a message that ends `power the machine off and on to change them`.

- An obsolete NVIDIA boot hold now produces a warning when the installed config grants no console release:
  `nvidia is held back at boot by <file>, and no console release is granted. activate will refuse. Remove <file>`.
  For files other than `nvidia-manual-load.conf`, `doctor` names the two lines to remove instead.
  `uninstall` leaves the hold in place and prints `<file> still holds nvidia back at boot; remove it if nothing else needs it`.
  [Uninstall](docs/installation.md#uninstall) explains its removal.

- If a release grant exists but NVIDIA's driver is already loaded and a card is unprepared, `setup` stops before `prepare` with exit 3:
  `setup: NVIDIA's driver is loaded, so the console GPU cannot be released in this boot. Create the hold (docs/installation.md#boot-display-card), power the machine off and on, then run setup again.`

- `install` prints `geforce_sriov.skip` only when no console release is granted.
  Console-release information appears once: at `setup`'s prompt if it asked, otherwise in `install`.
  `setup` uses `--release-note-shown` to suppress the duplicate.

- If a file in `/etc/modprobe.d/` already holds NVIDIA's driver back, `install` and the closing `setup` block print `nvidia is already held back at boot by <file>.` instead of showing a file to create.
  They still list the commands required after each boot.
  This also applies below `DESTDIR` when staging with a console-release grant.

#### Build mismatches and activation refusals

- On a loaded/installed module mismatch, `activate` warns and continues:
  `geforce-sriov: warning: the running nvidia module (build-id …) is not the installed one (…); power the machine off and on to load it`.
  It no longer adds "continuing", because an existing vGPU can still cause refusal.
  `status` prioritizes `next: power the machine off and on (the running nvidia module is not the installed one)` and omits steps leading to activation.
  While `/run/geforce-sriov/activating` exists, it retains `activate: … exists` but omits `next: stop every VM …`.
  A prepared card without VFs gets no separate next step.

- A module mismatch now makes `status` exit 1 even when cards are otherwise ready; previously it exited 0.
  It still prints `next: power the machine off and on (the running nvidia module is not the installed one)`.
  `check` retains its exit status, and `doctor` retains its warning.

- On an installed host with unready VFs and a loaded module different from the installed driver-step build, `setup` stops after `check`, before `prepare` and `activate`.
  It exits 3 without changes and prints `setup: the running nvidia module is not the installed one. Power the machine off and on, then run setup again.`

- Refusal because a vGPU already exists now lists affected VFs and their types, one per line, up to the first 8 followed by `and N more`.
  The list follows `each VF below has a vGPU, and restarting NVIDIA's vGPU daemons would end it:`.
  It ends with `Stop the VMs that use them, then write 0 to each nvidia/current_vgpu_type, for example:` and one command for all listed VFs:
  `for f in <BDFs>; do echo 0 > /sys/bus/pci/devices/$f/nvidia/current_vgpu_type; done`.
  The exit code remains 3.

#### Driver reporting and recovery

- `uninstall` and the driver step's `--restore` report plugin changes:
  - `pointed <link> back at the stock plugin libnvidia-vgpu.so.580.159.01` when the link pointed at the package's copy.
  - `removed <path>` for the removed copy.
  - `left <link>: it points to <target>, which this package did not install` for an unrelated link, which stays unchanged.

- `doctor` says the driver step writes no modprobe file only when `/etc/modprobe.d/geforce-sriov-nvidia.conf` is absent.
  Otherwise it says `its own file stays until uninstall`.
  If the driver step fails, `install` prints the cause once, at the end of the displayed log lines.

- After restoration, `uninstall` prints `NVIDIA's module backups stay for these kernels; to put one back, as root:` and a command for each retained kernel backup:
  `restore <kernel release>: cp /var/lib/geforce-sriov/stock/<kernel release>/nvidia.ko <module path> && depmod -a <kernel release>`.
  A failed restore prints `cp … && depmod -a <kernel release>` and points to docs/driver-patch.md#reverting for the plugin link and modprobe file.

- VF-count guidance in usage, `devices`, the config warning and the module parameter now explains that above 4 VFs, the module refuses to load if the card's VF layout check fails.
  Without a config file, `doctor --device BDF` prints `[INFO] config: /etc/geforce-sriov/config absent; the selected devices come from --device` instead of saying no card is selected.

- The README and supported stack state the driver-version rule: `install` accepts only host driver 580.159.01, and other host and guest driver versions are not supported.
  The closing `Guest drivers:` block in `setup` ends with:
  - `Every guest driver needs RMSetVgpuGspPluginOffloadMode=0.`
  - `Use these versions; other versions are not supported.`
  - `Setting and steps: docs/installation.md#guests.`

### Removed

- Removed `enable`.
  `activate` creates missing VFs, so it is the only command needed after boot.

- Removed Blackwell device IDs `2b85`, `2c02`, `2c05`, `2d04`, `2d05` and `2f04`.
  Blackwell cards are now treated as unlisted devices.

- Removed `experimental_vfs` from the module parameters, config, `install` and `prepare`, along with the module's replacement of `pci_read_config_dword()`.
  Configs containing `experimental_vfs=` or `twin_id=` are refused with `line N: unknown key '<key>'; delete this line`.

### Known limitations

- Once `prepare --release-console` releases a card, its framebuffer console stays detached until the next boot.

- Loading `nvidia` binds every NVIDIA card on the host; card selection cannot limit this.
  Select every card a mixed host will use.
  The rebuilt `nvidia.ko` applies its VF-count and scheduler revisions to every card bound to it.
  `bdfs` limits only the kernel module's SR-IOV presentation, identity hook, `prepare` and `activate`.

- Blackwell is not expected to work.
  Its cards are absent from the device table, and NVIDIA's proprietary 580.159.01 host driver does not support them on a vGPU host.

## [0.1.0]

Initial release.

### Added

- `geforce-sriov` CLI: read-only `devices`, `doctor` and `status`, plus `install`, `prepare`, `enable` and `uninstall`.
  Supports up to 8 selected GPUs and checks all of them before changing anything.
- `geforce_sriov` livepatch kernel module, packaged as DKMS `geforce-sriov/0.1.0`.
  For listed cards only, it exposes hidden SR-IOV, enables ARI in software and reveals MSI-X on Ampere.
  Parameters: `bdfs`, `vfs`, `ari`, `msix` and `experimental_vfs`.
- `geforce-sriov-prepare.service` prepares selected cards once per boot, before NVIDIA's driver binds them.
- Identity hook `libgeforce_sriov.so`, loaded only into `nvidia-vgpud` through a systemd drop-in.
  It presents selected cards as professional counterparts so the daemon offers vGPU types.
- `geforce-sriov-patch` with `inspect`, `apply` and `revert`.
  Its two-byte patch to your own `nv-kernel.o_binary` prevents guest GPU work from stalling.
- A device table containing 38 GeForce IDs across Ampere, Ada and Blackwell.
- Explicit boot-display permission through `install --boot-display-release BDF`, followed by `prepare --release-console=BDF` after each boot.
- VF-count configuration through `install --vfs N` or `vfs=N` in `/etc/geforce-sriov/config`, default 4.
- An offline test suite, run with `make check`.

### Known limitations

- VF count defaults to 4.
  ARI is enabled in software, but GPU hardware ARI decoding does not work.
- vGPU types are not filtered by physical VRAM; choose profiles that fit together.
- NVIDIA's vGPU host driver and guest licences are not included.
  You must supply both.
