# Glossary

Short definitions of the terms the guides and reference pages use, with a link to the page that explains each one in depth.

| Term | Meaning |
|---|---|
| PF (physical function) | The card itself as the PCI bus sees it, for example `0000:01:00.0` |
| BDF | A PCI address in the form `domain:bus:device.function`, as in `/sys/bus/pci/devices`. This package selects a card by the BDF of its PF |
| SR-IOV | A PCIe feature that lets one device appear as several lightweight devices. GeForce cards have it but hide it, and this package reveals it |
| VF (virtual function) | One of those lightweight devices. Each VF backs one vGPU and is attached to one VM. By default 4 are presented per card (`vfs`) |
| vGPU | NVIDIA's software that gives each VM a slice of one GPU. Each vGPU has a vGPU type (see the next row) that fixes its memory size |
| vGPU type | The fixed size of a vGPU. NVIDIA also calls it a profile. A name such as `RTXA6000-4Q` ends in the memory size: `4Q` means a 4 GB framebuffer, the VRAM given to the vGPU. All VFs of a card carry the same type ([Choosing a vGPU type](installation.md#choosing-a-vgpu-type)) |
| vGPU plugin | NVIDIA's `libnvidia-vgpu.so`, which `nvidia-vgpu-mgr` loads to run vGPUs. `install` makes a patched copy of your own plugin file and points the `libnvidia-vgpu.so` link at it |
| ARI | A PCIe addressing extension that allows more functions per device. The livepatch module (`geforce_sriov`) reveals the card's hidden ARI and turns on ARI forwarding in the bridge above it (`ari=on`). ARI decoding in the GPU hardware does **not** work, which is why 4 VFs is the default |
| MSI-X | A way for a device to send interrupts. The livepatch module (`geforce_sriov`) reveals the card's hidden MSI-X (`msix=on`) |
| mdev (mediated device) | How legacy vGPU creates each vGPU on GPUs without SR-IOV. Here each VM gets a VF instead, attached as a plain VFIO PCI device with the vGPU type already set on the VF ([Create a VM](installation.md#create-a-vm)) |
| IOMMU | The hardware that keeps each VM's device access separate. It must be enabled |
| Livepatch | A kernel feature that changes a running kernel's functions without a reboot. The package's `geforce_sriov` kernel module is a livepatch that lets the kernel see the SR-IOV feature a card hides, and only for the cards you select |
| DKMS | A framework that rebuilds out-of-tree kernel modules automatically for each new kernel |
| Boot unit | One of two systemd services that `install` enables: `geforce-sriov-livepatch.service` loads the livepatch and NVIDIA's driver, and `geforce-sriov-activate.service` runs `activate` to bring the GPUs up at each boot ([Systemd units](configuration.md#systemd-units)) |
| Identity hook | A small library loaded into NVIDIA's `nvidia-vgpud` daemon. It presents each selected card under its professional counterpart's device ID, so that NVIDIA's vGPU software accepts the card. The daemon loads it through `LD_PRELOAD`, a setting that makes a process load a given library first ([The identity hook](technical-reference.md#the-identity-hook)) |
| Boot display | The GPU that shows BIOS/UEFI startup messages before Linux loads, and then the host's display on its connected monitor, which is the local text console (login prompt) or a graphical desktop. Its `boot_vga` is `1` in sysfs ([Boot display card](installation.md#boot-display-card)) |
| Display modules | The kernel modules `nvidia-modeset` and `nvidia-drm`, which let the boot display GPU keep its display working while it serves VFs. `install` builds them from a second NVIDIA driver package and NVIDIA's public kernel module source ([Display and host CUDA builds](driver-patch.md#display-and-host-cuda-builds)) |
| KMS (kernel mode setting) | The part of a display driver that sets the screen mode and drives the display, console or desktop. Here it goes only to the boot display GPU |
| D3cold | A PCIe power state in which a device is powered off. A laptop GPU can enter it when idle, which erases its SR-IOV setup ([D3cold](laptops.md#d3cold)) |
| Activation lock | The file `/var/lib/geforce-sriov/prepare-attempt`. `activate` writes it when an activation is incomplete or its state is uncertain, and removes it when the run finishes cleanly. While it is set, activation refuses to run ([The activation lock](recovery.md#the-activation-lock)) |
| GSP | The GPU System Processor, which runs part of NVIDIA's driver as firmware. NVIDIA's stock vGPU host driver uses it on Ada, not on Ampere. On Ada, the NVIDIA host driver setup runs the driver without it (`NVreg_EnableGpuFirmware=0`, *CPU-RM*) ([NVIDIA host driver setup on Ada](ada.md#nvidia-host-driver-setup)) |
| Driver flavour | NVIDIA's Linux kernel module comes in a proprietary and an open flavour ([NVIDIA driver flavour](devices.md#nvidia-driver-flavour)) |
| Cold power cycle | Switching the machine fully off and on, **not** rebooting. A card that stopped answering, or an activation that did not finish, gets back to a known state only after the card loses power ([The power-cycle rule](recovery.md#the-one-rule)) |
| unbindLock | A file in `/proc/driver/nvidia/gpus/<BDF>/` through which NVIDIA's driver says whether a GPU is free. `activate` removes or re-probes a card only after the lock is granted, which means no program holds the GPU |
| PCI vendor ID, device ID, subsystem ID | The numbers a PCI device reports to identify itself. The vendor ID names the maker (`10de` is NVIDIA), the device ID names the chip or card model (for example `2206`), and the subsystem ID names the board maker's variant. `lspci -nn` shows them. This package matches cards by device ID and presents each one under its professional counterpart's ID |
| Config space | The small block of registers every PCI device exposes so the system can discover and configure it, including its capabilities such as SR-IOV, ARI and MSI-X. The addresses in the docs (`0xBCC`, `0x41C`) are offsets into it |
| BAR, memory window | A BAR (base address register) is a range of the machine's address space that a device uses to expose its memory and registers. The bridges above the card must have a window large enough to hold the BARs of the card and of every VF, otherwise a VF cannot be assigned its memory |
| Framebuffer | Video memory that holds an image. For the boot display GPU it is the memory the screen is drawn into: the firmware sets one up at boot, and `activate` hands it over to NVIDIA's display modules. For a vGPU, the framebuffer is the VRAM it gets, which the number in its type name states (`4Q` is 4 GB) |
| VFIO | The Linux framework that lets a VM use a PCI device directly. QEMU attaches a VF to a guest as a `vfio-pci` device |
| Kernel module | Code that the Linux kernel loads on demand, such as NVIDIA's `nvidia.ko`. A kernel module is built against one specific kernel version |
| udev | The part of systemd that loads drivers automatically when it sees hardware. This package keeps udev from loading `nvidia` on its own, because the livepatch has to be loaded first |
| modprobe option | A setting for a kernel module, written as `options <module> <name>=<value>` in a file under `/etc/modprobe.d/`. `install` writes a few of these ([Modprobe files](installation.md#modprobe-files)) |
| Display manager | The service that starts the graphical login screen, such as `gdm`, `sddm` or `lightdm`. `activate` stops it only with your consent while it hands the display over |
| `vgpuConfig.xml` | NVIDIA's list of the professional cards and vGPU types the vGPU software knows. GeForce cards are not in it, which is why the identity hook is needed |
| TotalVFs | The number of VFs a card says it can have. The package presents up to this many, 4 by default. Most Ada cards report 1, which the package treats as 4 ([Supported cards](devices.md#capability-positions-per-generation)) |
| Byte patch | A small change to a few bytes of NVIDIA's own driver object or plugin, which `install` makes in your own copy. Not to be confused with the two source patches of the display build ([Byte patches](driver-patch.md#byte-patches)) |
| Runtime power management | NVIDIA's driver powering an idle GPU down on its own (`NVreg_DynamicPowerManagement`). On a laptop it must be off, because a GPU that powers off loses its SR-IOV setup ([D3cold](laptops.md#d3cold)) |
