# Glossary

Terms used in the guides and reference pages.

| Term | Meaning |
|---|---|
| **PF** (physical function) | The card itself as the PCI bus sees it, for example `0000:01:00.0` |
| **BDF** | A PCI address in the form `domain:bus:device.function`, as in `/sys/bus/pci/devices` |
| **SR-IOV** | A PCIe feature that lets one device appear as several lightweight devices |
| **VF** (virtual function) | One of those lightweight devices. Each can back one vGPU. By default 4 are presented per card (`vfs`); 4 is also the most NVIDIA's patched driver uses |
| **ARI** | A PCIe addressing extension that allows more functions per device. The module reveals the card's hidden ARI and turns on ARI forwarding in the bridge above it (`ari=on`). More than 4 VFs per card is not available, so 4 VFs is the default |
| **MSI-X** | A way for a device to send interrupts. The module reveals the card's hidden MSI-X (`msix=on`) |
| **vGPU** | NVIDIA's software that gives each VM a slice of one GPU. Each vGPU has a type, also called a profile (for example `RTXA6000-4Q`), which fixes its memory size |
| **mdev** (mediated device) | How legacy vGPU creates each vGPU on GPUs without SR-IOV. Here each VM gets a VF instead, attached as a plain VFIO PCI device with the vGPU type already set on the VF ([Create a VM](installation.md#create-a-vm)) |
| **IOMMU** | The hardware that keeps each VM's device access separate. It must be enabled |
| **Livepatch** | A kernel feature that changes a running kernel's functions without a reboot. Used here only for the listed cards |
| **DKMS** | A framework that rebuilds out-of-tree kernel modules automatically for each new kernel |
| **Boot display** | The GPU that displays BIOS/UEFI startup messages before Linux loads and the local text console (login prompt) on its connected monitor. Its `boot_vga` is `1` in sysfs. See [Boot display GPU](installation.md#boot-display-gpu) |
| **GSP** | The GPU System Processor, which runs part of NVIDIA's driver as firmware. NVIDIA's stock vGPU host driver uses it on Ada, not on Ampere. The Ada host stack runs the driver without it (`NVreg_EnableGpuFirmware=0`, *CPU-RM*) ([Ada host stack](ada.md#host-stack)) |
| **Driver flavour** | NVIDIA's Linux kernel module comes in a proprietary and an open flavour ([NVIDIA driver flavour](devices.md#nvidia-driver-flavour)) |
