# Getting started

This page is the short path from a plain GeForce card to a VM with a vGPU.
Every step links to the matching section of the [installation guide](installation.md), which has the options, the reasons and the edge cases.
Run the host commands as root: on Proxmox VE you are already `root`, and on other distributions put `sudo` in front.

> [!IMPORTANT]
> - A power cycle means switching the machine fully off and on, **not** rebooting.
> - geforce-sriov does **not** supply NVIDIA software or vGPU licensing. You obtain the drivers and the licences from NVIDIA, and you license the guests yourself.
> - Host driver 580.159.01 is the tested version. Other versions are **not** guaranteed to work.
> - If the GPU you share also drives the host's monitor, the screen can go blank for a while during setup, and a desktop session on it is ended. Run `setup` over SSH.

## Before you start

You need:

- A Linux KVM host such as Proxmox VE or Debian, with a card from the [device table](devices.md#generations).
- The right to download NVIDIA's vGPU software, which NVIDIA calls an entitlement. Getting it is between you and NVIDIA.
- From the vGPU software 19.5 download for Linux KVM: the host driver, `NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run`.
- The guest drivers for your card ([step 6](#steps)). For an Ampere card they are in the same 19.5 download. For an Ada card they are in the vGPU software 16.9 download.
- If the card is the host's boot display (the card that shows the BIOS/UEFI screen, which it is when it is the only GPU), the second package and the kernel module source, both version 580.159.03:
  the Linux guest driver from the 19.5 download or NVIDIA's GeForce driver, and `https://github.com/NVIDIA/open-gpu-kernel-modules/archive/refs/tags/580.159.03.tar.gz`.
  `cat /sys/bus/pci/devices/<address>/boot_vga` prints `1` for the boot display ([Boot display card](installation.md#boot-display-card)).
- A C compiler, `make`, `dkms`, `pciutils` and the kernel headers for the running kernel, for example `apt install build-essential dkms pciutils proxmox-headers-$(uname -r)` (`linux-headers-$(uname -r)` on Debian).
- The `nouveau` driver not loaded: `lsmod | grep nouveau` prints nothing. Otherwise put `blacklist nouveau` and `options nouveau modeset=0` in `/etc/modprobe.d/blacklist-nouveau.conf`, run `update-initramfs -u` and power-cycle.
- The IOMMU turned on: `ls /sys/kernel/iommu_groups` lists numbered folders. Otherwise turn on VT-d or AMD-Vi and *Above 4G decoding* in the BIOS/UEFI setup. `setup` only warns about it, and the VM start fails later.
- Secure Boot off (`mokutil --sb-state`). With it on, geforce-sriov's modules would need signing, which this guide does not cover.

Have a way back ready ([Prepare a way back](installation.md#prepare-a-way-back)):

- a backup of your VM configs and of any data you cannot recreate;
- a bootable recovery medium, such as a USB installer for the host;
- SSH access to the host from another machine, tested, which keeps the host reachable while the screen is blank;
- a way to power-cycle the host (the power button, a smart plug or a remote management port).

## Steps

1. Download and build.
   Get `geforce-sriov-<version>.tar.gz` and `SHA256SUMS` from the [latest release](https://github.com/noon-at-cgn/geforce-sriov/releases/latest), then run this in the folder that holds both files, with `<version>` replaced by the release number and no angle brackets:

   ```sh
   sha256sum -c SHA256SUMS
   tar xzf geforce-sriov-<version>.tar.gz
   cd geforce-sriov-<version>
   make
   ```

   The first command prints `geforce-sriov-<version>.tar.gz: OK` when the download is intact.
   `make` builds the tools in that folder and installs nothing.
   More: [Automated setup](installation.md#automated-setup).

2. Run setup.

   ```sh
   ./geforce-sriov setup
   ```

   `setup` asks only for what it cannot find out itself, in this order:
   - The path of NVIDIA's host driver `.run` file, if no NVIDIA driver is installed yet, then whether you accept NVIDIA's licence (no default).
   - Which GPUs to use (`all`, or the numbers shown), when the host has more than one supported card. With one, `setup` selects it and says so.
   - For a laptop GPU, your consent to keep it powered. The default is no, and without consent `setup` stops.
   - Whether to build NVIDIA's CUDA module for the host. The default is no.
   - For a boot display card, the path of the second package, then of the kernel module source. Answering `none` builds no display modules, so the card cannot keep the host's screen working while it serves VFs: plan on SSH only.

   It then prints `Setup runs with:`, a summary and one `geforce-sriov setup --no-prompt …` command that repeats the run without questions. Copy it: if `setup` stops, you can run it again without answering every question.
   Two more questions can follow: whether to continue if the host driver is not 580.159.01 (default no), and whether to stop the display manager (the service behind the graphical login) if it uses a selected GPU.
   Stopping it **ends the running desktop session**, so save your work first.
   Then `== doctor`, `== install` (the NVIDIA module build takes a few minutes), `== activate` and `== check` run in turn. If `doctor` prints `[FAIL]`, `setup` stops.
   If `install` says `nouveau` is loaded, do what the nouveau item above says, then run `setup` again.
   More: [What setup asks](installation.md#what-setup-asks).

3. Power-cycle when it asks.
   If `setup` prints `The install is complete.` and `Next: power the machine off and on.`, switch the machine fully off and on, then run `geforce-sriov setup` again. The command is installed now, so it works from any folder without `./`.
   This happens when NVIDIA's driver was already loaded, because the livepatch must load before it.
   From then on, the boot units bring the VFs back at every boot.
   More: [When NVIDIA's driver is already loaded](installation.md#when-nvidias-driver-is-already-loaded).

4. Check the result.
   `setup` prints one line per VF under `== check`:

   ```text
   OK 0000:01:00.4 (VF of 0000:01:00.0): driver=nvidia type=942 expected=942 power/control=on runtime_status=active
   ```

   `OK` means the VF is bound to NVIDIA's driver, powered on and carries its vGPU type (942 on an Ada card, 525 on an Ampere card).
   A first run prints `activation finished` above these lines. The run after a power cycle prints `Every selected VF is ready. Nothing to bring up.` below them, then `Guest drivers:` with the versions for your card.
   A line that begins with `NOT READY` means start no VM yet ([If something goes wrong](recovery.md#if-something-goes-wrong)).
   Later, `geforce-sriov status` shows `next: every selected VF is ready. Nothing to bring up.`
   More: [Cold boot](installation.md#cold-boot).

5. Create a VM with a VF.
   Each VF is one vGPU, and each VM gets its own VF. `geforce-sriov status` lists the addresses, such as `0000:01:00.4` to `0000:01:00.7`.
   Create the VM as usual, then add one line to `/etc/pve/qemu-server/<vmid>.conf` with that VM's VF (the web interface has no field for it):

   ```text
   args: -device vfio-pci,host=0000:01:00.4,enable-migration=off
   ```

   `vfio-pci` here is QEMU's name for a passed-through PCI device, not a host driver. Do **not** bind the VF to a `vfio-pci` driver and do **not** blacklist `nvidia`: the VF stays on NVIDIA's host driver.
   With libvirt, see [Create a VM](installation.md#create-a-vm).
   Right before you start the VM, run `geforce-sriov check`: every line should begin with `OK`. Then start it, for example `qm start <vmid>`.
   Each vGPU gets 2 GB of VRAM by default ([Choosing a vGPU type](installation.md#choosing-a-vgpu-type)).
   A vGPU has no monitor connector: you see the guest through the Proxmox VE console or a remote desktop program inside the guest.

6. Install the guest driver.
   Each VM needs NVIDIA's vGPU **guest** driver, not a regular GeForce driver. The tested ones are:

   | Card | Linux guest | Windows guest | In NVIDIA's download |
   |---|---|---|---|
   | Ada (RTX 40) | 535.230.02 | 539.19 | vGPU software 16.9 |
   | Ampere (RTX 30) | 580.159.03 | 582.53 | vGPU software 19.5, with the host driver |

   For an Ada card the recommended guest drivers are older than the host driver on purpose. In an Ada guest the 19.5 Linux driver (580.159.03) does **not** run CUDA; the 19.5 Windows driver (582.53) was also tested and works ([Guests and VMs](ada.md#guests-and-vms)).
   Every guest also needs one setting, `RMSetVgpuGspPluginOffloadMode=0`. Without it, a Linux guest logs `RmInitAdapter failed`, and Windows shows the device with Code 43.

   In a **Linux** guest (tested with Debian 12):

   ```sh
   sudo apt install build-essential dkms linux-headers-$(uname -r)
   sudo sh NVIDIA-Linux-x86_64-<version>-grid.run --dkms
   echo 'options nvidia NVreg_RegistryDwords="RMSetVgpuGspPluginOffloadMode=0"' | sudo tee /etc/modprobe.d/nvidia-vgpu-guest.conf
   sudo update-initramfs -u
   sudo reboot
   ```

   If the installer says the `nouveau` driver (the open-source NVIDIA driver Linux loads by default) is in use, let it disable `nouveau`, reboot the VM and run the installer again.
   After the reboot, `nvidia-smi` lists the vGPU: `NVIDIA RTX6000-Ada-2Q` on an Ada card, `NVIDIA RTXA6000-2Q` on an Ampere card.

   In a **Windows** guest, run NVIDIA's installer, then open PowerShell as Administrator and run:

   ```powershell
   Get-PnpDevice -PresentOnly -Class Display -FriendlyName '*NVIDIA*' | ForEach-Object {
       $key = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Driver).Data
       reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\$key" /v RMSetVgpuGspPluginOffloadMode /t REG_DWORD /d 0 /f
       pnputil /restart-device "$($_.InstanceId)"
   }
   Get-PnpDevice -PresentOnly -Class Display -FriendlyName '*NVIDIA*' | Format-Table FriendlyName, Status
   ```

   The last command shows the vGPU with the status `OK`: `NVIDIA RTX 6000 Ada-2Q` on an Ada card, `NVIDIA RTXA6000-2Q` on an Ampere card.
   More: [Guests](installation.md#guests).

## After setup

- A kernel update needs `geforce-sriov rebuild --kernel <new release>` before you boot the new kernel, then a power cycle ([After a kernel update](installation.md#after-a-kernel-update)). Until the VFs are up, the host's guest autostart does not run.
- To boot once without geforce-sriov, add `geforce_sriov.skip` to the kernel command line ([Turn features off](recovery.md#turn-features-off)). To remove everything, run `geforce-sriov uninstall` ([Uninstall](installation.md#uninstall)).
- If the host stops responding or a card disappears from `lspci`, power-cycle and do **not** rescan the PCI bus ([the power-cycle rule](recovery.md#the-one-rule)).

If something does not work, start with [If something goes wrong](recovery.md#if-something-goes-wrong).
