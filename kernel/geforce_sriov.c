// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * geforce_sriov - livepatch that lets the kernel use the SR-IOV capability of
 * the NVIDIA physical functions (PFs) named in its bdfs parameter, with at
 * most vfs (default GS_DEFAULT_VFS, 4) virtual functions, never more than the
 * card's hardware TotalVFs. Cards are described only by the reviewed table in
 * include/gs_devices.h.
 *
 * Three exported vmlinux functions are replaced, in the way
 * samples/livepatch/livepatch-sample.c shows. All replacements are written
 * for this module on top of exported PCI accessors; for every device that is
 * not a listed, validated PF they return exactly what the kernel's own
 * functions return, and pci_write_config_word() writes exactly what it is
 * given for every device.
 *
 * pci_find_ext_capability():
 *   Ampere/Ada keep SR-IOV (and ARI) in config space but out of the extended
 *   capability chain. For a listed PF whose chain has no SR-IOV and whose
 *   table offset holds an SR-IOV version-1 header, an SR-IOV lookup returns
 *   that offset. With ari=1 and a usable hidden ARI header (see
 *   gs_hidden_ari()), the SR-IOV lookup made by pci_iov_init() turns on ARI
 *   Forwarding in the upstream port when the port supports it. That side
 *   effect is the mechanism that brings ARI to the port: on a kernel whose
 *   pci_configure_ari() inlines its ARI lookup, it never calls this function
 *   (see gs_enable_ari_forwarding()). An ARI lookup returns the hidden ARI
 *   offset for the same PFs; on such a kernel its caller is pci_scan_slot()'s
 *   inlined next_ari_fn(), which uses it only to find the next function once
 *   forwarding is on. This ARI scheme (forwarding set from the SR-IOV
 *   lookup) is on by default; if VF enable does not complete, load with
 *   ari=0 (ari=off).
 *
 * pci_read_config_word():
 *   sriov_init() and sriov_enable() read TotalVFs and InitialVFs through this
 *   exported function. For a listed PF whose hidden SR-IOV header validates
 *   (ID and version 1 checked before every override) those reads return
 *   gs_vf_limit(): the vfs parameter, never more than the hardware value,
 *   except that a hardware value of exactly 1 counts as 4 on Ada (0 stays
 *   0). This also sizes the VF BAR reservation. User-space config reads
 *   (lspci, setpci, sysfs config) go through pci_user_read_config_word() and
 *   still show the hardware values. NVIDIA's driver does not read these
 *   words: on Ada it reads a BAR0 mirror, which no accessor sees, and gets 4
 *   from the VF-count revision of the vendor object instead.
 *
 *   The same function guards where the kernel puts the VFs while it sets
 *   up SR-IOV, and only then. sriov_init() (drivers/pci/iov.c) runs
 *   compute_max_vf_buses(), which writes every NumVFs from TotalVFs (at
 *   most vfs here) down to 1, with the ARI bit sriov_init() has just written,
 *   and reads First VF Offset and VF Stride through pci_iov_set_numvfs().
 *   It refuses offset 0, or stride 0 with more than one VF, with -EIO;
 *   sriov_init() then drops all SR-IOV state of the PF, so no VF can ever be
 *   enabled on it. For a listed PF the offset and stride reads return 0 so
 *   the kernel takes that path when NumVFs does not read back the value the
 *   kernel wrote (a card may clamp or ignore it) or when that many VFs would
 *   not all land on free functions 1-7 of the PF's slot (gs_vf_setup_check(),
 *   gs_guard_vf_geometry()). sriov_init() runs only from pci_device_add(),
 *   before device_add() registers the device; only reads made then are
 *   changed.
 *
 * pci_write_config_word():
 *   Unchanged for every device and register. It only records, per listed
 *   PF that is not yet registered, the NumVFs value the kernel writes, so
 *   the guard checks the count the kernel asked for rather than what the
 *   register reads back.
 *
 * Not guarded: sriov_enable() and sriov_restore_state() run on a registered
 *   device and do not check the offset and stride they read; a 0 there
 *   would place VF0 on the PF, so those reads keep the hardware values and a
 *   bad layout is only logged. They rely on the card giving the same offset
 *   and stride for the same NumVFs and ARI bit as during sriov_init() (the
 *   kernel writes only the ARI bit sriov_init() chose and restores it); a
 *   card that changes its answer after setup, or after a reset, is not
 *   supported. A NumVFs read-back that matches does not show that a card
 *   whose hardware TotalVFs is lower (the Ada path) really provides that
 *   many VFs.
 *
 * MSI-X (msix=1): NVIDIA's CYA bit 0x41C[29] unlinks MSI-X from the
 *   standard chain. At load, for each listed PF whose MSI-X is not linked, the
 *   module validates the MSI-X capability at its table offset, clears that bit,
 *   reads it back and checks that the chain now links MSI-X there. It does not
 *   touch struct pci_dev: the CLI removes and rescans the PF afterwards, and
 *   the PCI core then finds MSI-X on its own. A natively linked MSI-X is left
 *   alone. In two cases the load succeeds with MSI-X still hidden, after a
 *   warning: there is no MSI-X capability at the table's MSI-X offset, or bit
 *   29 is already clear while MSI-X is not linked. Any other failed check
 *   restores the bit and refuses the load. A PF reset (FLR, secondary-bus
 *   reset, AER recovery) hides MSI-X again and silently stops the PF's
 *   interrupts until the next cold boot (msix=0 avoids it).
 *
 * This module never removes, rescans, resets or binds a device. PCI
 * enumeration runs before modules load, so the CLI must remove and rescan each
 * listed PF after the patch is enabled; then the kernel sees the capability.
 *
 * Disable: echo 0 > /sys/kernel/livepatch/geforce_sriov/enabled, wait for
 * transition=0, then rmmod. Never with VFs enabled. The MSI-X hide bit is
 * volatile (FLR, secondary-bus reset or a cold boot set it again) and is not
 * set again at unload.
 *
 * No MODULE_DEVICE_TABLE on purpose: udev must never autoload a livepatch.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/version.h>

#include "gs_devices.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
#error "geforce_sriov supports Linux 6.8 and later"
#endif
/*
 * pci_dev_is_disconnected() is public in <linux/pci.h> from 6.9 and in stable
 * backports that LINUX_VERSION_CODE cannot see; Kbuild probes for it.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0) && \
	!defined(GS_HAVE_PUBLIC_DISCONNECTED)
#error "geforce_sriov needs pci_dev_is_disconnected() in <linux/pci.h> (Linux 6.9, or a stable backport)"
#endif
#if !IS_ENABLED(CONFIG_LIVEPATCH)
#error "geforce_sriov needs CONFIG_LIVEPATCH"
#endif
#if !IS_ENABLED(CONFIG_PCI_IOV)
#error "geforce_sriov needs CONFIG_PCI_IOV"
#endif
/*
 * The patch replaces three exported PCI functions (drivers/pci/access.c and
 * pci.c) and relies on drivers/pci/iov.c calling them, not inlined copies.
 * Linux's LIVEPATCH does not exclude Clang LTO
 * (CONFIG_LTO_CLANG, full or thin), which can inline across files, so such a
 * kernel is refused.
 */
#if IS_ENABLED(CONFIG_LTO_CLANG)
#error "geforce_sriov does not support kernels built with Clang LTO (CONFIG_LTO_CLANG): drivers/pci/iov.c may use inlined copies of the PCI config accessors the patch replaces"
#endif

static_assert(GS_VENDOR_NVIDIA == PCI_VENDOR_ID_NVIDIA);
static_assert(GS_EXT_CAP_ID_SRIOV == PCI_EXT_CAP_ID_SRIOV);
static_assert(GS_EXT_CAP_ID_ARI == PCI_EXT_CAP_ID_ARI);
static_assert(GS_CAP_ID_MSIX == PCI_CAP_ID_MSIX);

/* ------------------------------------------------------------- parameters */

static bool ari = true;
module_param(ari, bool, 0444);
MODULE_PARM_DESC(ari, "Present the hidden ARI capability of listed function-0 PFs and enable ARI Forwarding on their upstream port (default Y). pci=noari is not visible to modules: pass ari=0 with it");

static bool msix = true;
module_param(msix, bool, 0444);
MODULE_PARM_DESC(msix, "At load, relink the hidden MSI-X capability of listed PFs (default Y)");

static unsigned int vfs = GS_DEFAULT_VFS;
module_param(vfs, uint, 0444);
MODULE_PARM_DESC(vfs, "VFs per listed PF, 1-16, at most the card's hardware TotalVFs (default 4: ARI decoding in the GPU does not work, so with more than 4 VFs the module refuses to load if the card's VF layout check fails)");
static_assert(GS_DEFAULT_VFS == 4 && GS_VFS_MAX == 16); /* the text above */

static char *bdfs = "";
module_param(bdfs, charp, 0444);
MODULE_PARM_DESC(bdfs, "Required: 1-8 comma-separated PFs dddd:bb:ss.0 as named in /sys/bus/pci/devices (lowercase, function 0). Only these are changed; empty refuses to load");

/*
 * Parsed bdfs and, per PF, the functions of its slot in use at load (bit n =
 * function n, the PF itself included). Written once in init before the patch
 * is enabled.
 */
static struct gs_bdf gs_sel[GS_MAX_PFS];
static unsigned int gs_occupied[GS_MAX_PFS];
static unsigned int gs_nsel;

/*
 * The NumVFs value the kernel last wrote to each listed PF while it was not
 * yet registered, i.e. during sriov_init() (gs_pci_write_config_word()):
 * the value (bits 15:0) with GS_REQ_VALID, stored before the hardware write
 * whatever it returns (pci_iov_set_numvfs() ignores the result), and
 * GS_REQ_WRITE_OK added once the write returned 0. One word, so READ_ONCE()/
 * WRITE_ONCE() see it whole. A write to a registered PF clears it. Every
 * sriov_init() writes NumVFs before it reads offset or stride, so the
 * record a read sees in that window is from the same enumeration.
 */
#define GS_REQ_VALID	BIT(16)
#define GS_REQ_WRITE_OK	BIT(17)
static u32 gs_req[GS_MAX_PFS];

/*
 * TotalVFs, InitialVFs, First VF Offset and VF Stride offsets of the
 * distinct table SR-IOV positions for the read hook, and their NumVFs
 * offsets for the write hook (a separate list, so NumVFs reads are never
 * capped by gs_vf_limit()). Filled once in init before the patch is enabled
 * (the table has one position).
 */
static const int gs_watch_regs[] = {
	PCI_SRIOV_TOTAL_VF, PCI_SRIOV_INITIAL_VF,
	PCI_SRIOV_VF_OFFSET, PCI_SRIOV_VF_STRIDE,
};
static int gs_watch_where[ARRAY_SIZE(gs_watch_regs) * GS_DEVICE_COUNT];
static unsigned int gs_nwatch_where;
static int gs_numvf_where[GS_DEVICE_COUNT];
static unsigned int gs_nnumvf_where;

static void gs_init_watch_where(void)
{
	unsigned int i, j, k;
	int pos;

	for (i = 0; i < GS_DEVICE_COUNT; i++) {
		pos = gs_devices[i].sriov_pos;
		for (j = 0; j < gs_nwatch_where; j++)
			if (gs_watch_where[j] == pos + gs_watch_regs[0])
				break;
		if (j < gs_nwatch_where)
			continue;
		for (k = 0; k < ARRAY_SIZE(gs_watch_regs); k++)
			gs_watch_where[gs_nwatch_where++] = pos + gs_watch_regs[k];
		gs_numvf_where[gs_nnumvf_where++] = pos + PCI_SRIOV_NUM_VF;
	}
}

/* ------------------------------------------------------------- predicates */

/* The index of @dev in gs_sel, or -1 if it is not listed. */
static int gs_sel_index(const struct pci_dev *dev)
{
	unsigned int i;

	for (i = 0; i < gs_nsel; i++)
		if (gs_sel[i].domain == (unsigned int)pci_domain_nr(dev->bus) &&
		    gs_sel[i].bus == dev->bus->number &&
		    gs_sel[i].devfn == dev->devfn)
			return (int)i;
	return -1;
}

/* The table entry of a listed PF, or NULL for every other device. */
static const struct gs_device *gs_entry(const struct pci_dev *dev)
{
	const struct gs_device *e;

	if (dev->vendor != PCI_VENDOR_ID_NVIDIA || dev->is_virtfn ||
	    PCI_FUNC(dev->devfn) != 0)
		return NULL;
	e = gs_device_lookup(dev->device);
	if (!e || gs_sel_index(dev) < 0)
		return NULL;
	return e;
}

/* Does @pos hold an extended capability header @id, version 1? Reads only. */
static bool gs_ext_hdr_at(struct pci_dev *dev, u16 pos, u16 id)
{
	u32 hdr;

	if (pos < PCI_CFG_SPACE_SIZE || pos + 4 > dev->cfg_size)
		return false;
	if (pci_read_config_dword(dev, pos, &hdr))
		return false;
	return gs_ext_cap_hdr_is(hdr, id, 1);
}

/*
 * The validated SR-IOV offset of @dev under @e, or 0: the chain has no
 * SR-IOV and the table offset holds a v1 header.
 */
static u16 gs_sriov_pos(struct pci_dev *dev, const struct gs_device *e)
{
	if (pci_find_next_ext_capability(dev, 0, PCI_EXT_CAP_ID_SRIOV))
		return 0;
	return gs_ext_hdr_at(dev, e->sriov_pos, PCI_EXT_CAP_ID_SRIOV) ?
	       e->sriov_pos : 0;
}

/*
 * Should @dev's hidden ARI capability be presented? Besides the header, its
 * Next Function Number must be non-zero: with ARI Forwarding on,
 * pci_scan_slot() follows it and would stop after function 0, dropping the
 * card's audio function. The public config-space dumps show 1 (0xC18 =
 * 00000100); the value is checked on every card. Read as a dword so the
 * patched word accessor is not involved.
 *
 * Never with PCI_SCAN_ALL_PCIE_DEVS (pci=pcie_scan_all or a platform quirk):
 * the rescan then probes slots 1-31 below the port, and with ARI Forwarding
 * on those devfns (8, 16, ...) reach the card, whose 3-bit function decode
 * answers them as aliases of the PF and its audio function. The kernel would
 * add phantom devices and size their BARs through the real PF.
 */
static bool gs_hidden_ari(struct pci_dev *dev, const struct gs_device *e)
{
	u32 cap;

	if (!ari || dev->devfn != 0 ||
	    pci_has_flag(PCI_SCAN_ALL_PCIE_DEVS) ||
	    pci_find_next_ext_capability(dev, 0, PCI_EXT_CAP_ID_ARI) ||
	    !gs_ext_hdr_at(dev, e->ari_pos, PCI_EXT_CAP_ID_ARI) ||
	    e->ari_pos + PCI_ARI_CAP + 4 > dev->cfg_size ||
	    pci_read_config_dword(dev, e->ari_pos + PCI_ARI_CAP, &cap))
		return false;
	return PCI_ARI_CAP_NFN(cap & 0xffff) != 0;
}

/*
 * Enable ARI Forwarding on the port above @dev, as the PCI core does for a
 * function 0 whose ARI capability it can see.
 *
 * pci_configure_ari() runs just before pci_iov_init() during enumeration. On
 * a kernel where its ARI lookup is inlined, it never reaches the replaced
 * pci_find_ext_capability(), misses the hidden header and leaves the port
 * with ari_enabled = 0. The SR-IOV lookup in pci_iov_init() is the first
 * replaced call after it, and sriov_init() reads pci_ari_enabled() right
 * after that lookup, so that is where the port is set. On a kernel whose
 * pci_configure_ari() does call the replaced lookup, it enables the port
 * itself and this finds ari_enabled already set. A later lookup changes
 * nothing either.
 */
static void gs_enable_ari_forwarding(struct pci_dev *dev)
{
	struct pci_dev *port = dev->bus->self;
	u32 devcap2 = 0;
	int ret;

	if (!port || port->ari_enabled || !pci_is_pcie(dev))
		return;
	pcie_capability_read_dword(port, PCI_EXP_DEVCAP2, &devcap2);
	if (!(devcap2 & PCI_EXP_DEVCAP2_ARI)) {
		pci_info_ratelimited(dev, KBUILD_MODNAME ": port %s does not support ARI Forwarding; ARI stays off\n",
				     pci_name(port));
		return;
	}
	ret = pcie_capability_set_word(port, PCI_EXP_DEVCTL2,
				       PCI_EXP_DEVCTL2_ARI);
	if (ret) {
		pci_warn(dev, KBUILD_MODNAME ": cannot enable ARI Forwarding on %s (%d); ARI stays off\n",
			 pci_name(port), ret);
		return;
	}
	port->ari_enabled = 1;
	pci_info(dev, KBUILD_MODNAME ": ARI capability is not linked; enabled ARI Forwarding on %s\n",
		 pci_name(port));
}

/* ------------------------------------------------- replaced functions */

/* pci_find_ext_capability(), plus the hidden SR-IOV and ARI fallbacks */
static u16 gs_pci_find_ext_capability(struct pci_dev *dev, int cap)
{
	const struct gs_device *e;
	u16 pos = pci_find_next_ext_capability(dev, 0, cap);

	if (pos || (cap != PCI_EXT_CAP_ID_SRIOV && cap != PCI_EXT_CAP_ID_ARI))
		return pos;
	e = gs_entry(dev);
	if (!e)
		return 0;

	if (cap == PCI_EXT_CAP_ID_ARI)
		return gs_hidden_ari(dev, e) ? e->ari_pos : 0;

	if (!gs_ext_hdr_at(dev, e->sriov_pos, PCI_EXT_CAP_ID_SRIOV))
		return 0;
	if (gs_hidden_ari(dev, e))
		gs_enable_ari_forwarding(dev);
	pci_info_ratelimited(dev, KBUILD_MODNAME ": SR-IOV capability at %#x is not linked; using it with at most %u VFs\n",
			     e->sriov_pos, vfs);
	return e->sriov_pos;
}

/* Is @where a watched SR-IOV register offset of a table SR-IOV position? */
static bool gs_watched_offset(int where)
{
	unsigned int i;

	for (i = 0; i < gs_nwatch_where; i++)
		if (where == gs_watch_where[i])
			return true;
	return false;
}

/*
 * The First VF Offset or VF Stride read (@where, hardware value in *@val) of
 * a listed PF whose SR-IOV header at @e->sriov_pos validated. NumVFs, offset
 * and stride are read again from the hardware with the unpatched bus
 * accessor.
 *
 * Before device_add() registers the PF (device_is_registered() is false)
 * these reads come from sriov_init(): pci_iov_init() is called only from
 * pci_init_capabilities() in pci_device_add(), and nothing else in that
 * window calls pci_iov_set_numvfs(). The NumVFs value checked is the one the
 * kernel wrote just before (gs_req), not the register: a card may clamp or
 * ignore the write, and the kernel would still create that many VFs later.
 * gs_vf_setup_check() refuses a read-back that differs and a bad layout;
 * a missing record or a failed non-zero write is refused as well. A refusal
 * reads as 0, and compute_max_vf_buses() then refuses SR-IOV for the PF with
 * -EIO. A requested 0 (its final reset) places no VF and keeps the hardware
 * value, whatever the write returned.
 *
 * On a registered PF the reads come from sriov_enable() or
 * sriov_restore_state(), which use offset and stride without checking them:
 * a 0 would put VF0 on the PF's own routing ID. There the hardware value is
 * kept and a bad layout at the register's NumVFs is only logged.
 *
 * Logs only on the offset read: the stride read follows for the same NumVFs.
 */
static void gs_guard_vf_geometry(struct pci_dev *dev, const struct gs_device *e,
				 int where, u16 *val)
{
	u16 numvfs = 0, offset = 0, stride = 0;
	int pos = e->sriov_pos, i = gs_sel_index(dev);
	bool log = where == pos + PCI_SRIOV_VF_OFFSET, readable;
	unsigned int requested = 0;
	enum gs_vf_geometry g;
	const char *why;
	u32 req;

	if (i < 0)
		return;
	readable = !pci_bus_read_config_word(dev->bus, dev->devfn,
					     pos + PCI_SRIOV_NUM_VF, &numvfs) &&
		   !pci_bus_read_config_word(dev->bus, dev->devfn,
					     pos + PCI_SRIOV_VF_OFFSET, &offset) &&
		   !pci_bus_read_config_word(dev->bus, dev->devfn,
					     pos + PCI_SRIOV_VF_STRIDE, &stride);

	if (device_is_registered(&dev->dev)) {
		if (readable) {
			g = gs_vf_geometry_check(numvfs, offset, stride,
						 gs_occupied[i]);
			if (g == GS_GEOM_OK)
				return;
			why = gs_vf_geometry_text(g);
		} else {
			why = "NumVFs, offset or stride could not be read";
		}
		if (log)
			dev_err_ratelimited(&dev->dev, KBUILD_MODNAME ": unexpected VF layout after SR-IOV setup at NumVFs %u (first VF offset %u, stride %u, functions in use %#04x): %s; the kernel uses it anyway: disable the VFs, then remove and rescan the PF\n",
					    numvfs, offset, stride,
					    gs_occupied[i], why);
		return;
	}

	req = READ_ONCE(gs_req[i]);
	if (!(req & GS_REQ_VALID)) {
		why = "no NumVFs write by the kernel was seen before this read";
	} else {
		requested = req & 0xffff;
		if (!requested)
			return;
		if (!(req & GS_REQ_WRITE_OK)) {
			why = "the kernel's NumVFs write failed";
		} else if (!readable) {
			why = "NumVFs, offset or stride could not be read";
		} else {
			g = gs_vf_setup_check(requested, numvfs, offset, stride,
					      gs_occupied[i]);
			if (g == GS_GEOM_OK)
				return;
			why = gs_vf_geometry_text(g);
		}
	}
	*val = 0;
	if (log)
		pci_err(dev, KBUILD_MODNAME ": VF layout for %u requested VFs (NumVFs reads %u, first VF offset %u, stride %u, functions in use %#04x): %s; SR-IOV is not set up on this PF\n",
			requested, numvfs, offset, stride, gs_occupied[i], why);
}

/*
 * pci_read_config_word(), plus the TotalVFs/InitialVFs override and the VF
 * layout guard
 */
static int gs_pci_read_config_word(const struct pci_dev *dev, int where,
				   u16 *val)
{
	const struct gs_device *e;
	struct pci_dev *pdev;
	int ret, pos;

	if (pci_dev_is_disconnected(dev)) {
		PCI_SET_ERROR_RESPONSE(val);
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	ret = pci_bus_read_config_word(dev->bus, dev->devfn, where, val);
	/* Cheap tests first: this runs for every in-kernel config word read. */
	if (ret || !gs_watched_offset(where))
		return ret;

	e = gs_entry(dev);
	if (!e)
		return ret;
	pos = e->sriov_pos;
	if (where != pos + PCI_SRIOV_TOTAL_VF &&
	    where != pos + PCI_SRIOV_INITIAL_VF &&
	    where != pos + PCI_SRIOV_VF_OFFSET &&
	    where != pos + PCI_SRIOV_VF_STRIDE)
		return ret;
	pdev = (struct pci_dev *)dev; /* the code below only reads */
	if (gs_sriov_pos(pdev, e) != pos)
		return ret;
	if (where == pos + PCI_SRIOV_VF_OFFSET ||
	    where == pos + PCI_SRIOV_VF_STRIDE)
		gs_guard_vf_geometry(pdev, e, where, val);
	else
		*val = gs_vf_limit(e, *val, vfs);
	return ret;
}

/* Is @where a NumVFs offset of a table SR-IOV position? */
static bool gs_numvf_offset(int where)
{
	unsigned int i;

	for (i = 0; i < gs_nnumvf_where; i++)
		if (where == gs_numvf_where[i])
			return true;
	return false;
}

/*
 * pci_write_config_word(), unchanged, plus a record of the NumVFs value the
 * kernel writes to a listed PF that is not yet registered (gs_req, read by
 * gs_guard_vf_geometry()). The disconnect check, the hardware write, the
 * value and the return code are those of the kernel's own function; nothing
 * else is read or written, and nothing is logged (this runs for every
 * in-kernel config word write). On a registered PF the record is cleared.
 */
static int gs_pci_write_config_word(const struct pci_dev *dev, int where,
				    u16 val)
{
	const struct gs_device *e;
	bool unregistered = false;
	int ret, i = -1;

	if (pci_dev_is_disconnected(dev))
		return PCIBIOS_DEVICE_NOT_FOUND;
	/* Cheap test first. */
	if (gs_numvf_offset(where)) {
		e = gs_entry(dev);
		if (e && where == e->sriov_pos + PCI_SRIOV_NUM_VF)
			i = gs_sel_index(dev);
	}
	if (i >= 0) {
		unregistered =
			!device_is_registered(&((struct pci_dev *)dev)->dev);
		WRITE_ONCE(gs_req[i], unregistered ?
				      (u32)val | GS_REQ_VALID : 0);
	}
	ret = pci_bus_write_config_word(dev->bus, dev->devfn, where, val);
	if (i >= 0 && unregistered && !ret)
		WRITE_ONCE(gs_req[i], (u32)val | GS_REQ_VALID | GS_REQ_WRITE_OK);
	return ret;
}

/* ----------------------------------------------------------- MSI-X unhide */

static int gs_set_msix_hidden(struct pci_dev *pf, const struct gs_device *e,
			      bool hidden)
{
	u32 cya;
	int ret;

	ret = pci_read_config_dword(pf, e->msix_hide_pos, &cya);
	if (ret)
		return pcibios_err_to_errno(ret);
	if (hidden)
		cya |= BIT(GS_MSIX_HIDE_BIT);
	else
		cya &= ~BIT(GS_MSIX_HIDE_BIT);
	return pcibios_err_to_errno(pci_write_config_dword(pf, e->msix_hide_pos,
							    cya));
}

/*
 * Relink a hidden MSI-X capability by clearing its hide bit; the MSI-X
 * capability itself is not written. Sets *@unhid
 * when it cleared the hide bit. 0 also covers "nothing to do".
 * The caller holds @pf's device lock.
 */
static int gs_unhide_msix_locked(struct pci_dev *pf, const struct gs_device *e,
				 bool *unhid)
{
	u32 cya = 0;
	u16 flags = 0;
	u8 linked, id = 0;
	int ret;

	*unhid = false;
	linked = pci_find_capability(pf, PCI_CAP_ID_MSIX);
	if (linked) {
		pci_info(pf, KBUILD_MODNAME ": MSI-X is linked at %#x; nothing to do\n",
			 linked);
		return 0;
	}
	if (pf->driver) {
		pci_err(pf, KBUILD_MODNAME ": driver %s is bound; MSI-X can only be relinked on an unbound PF\n",
			pf->driver->name);
		return -EBUSY;
	}
	if (pci_num_vf(pf)) {
		pci_err(pf, KBUILD_MODNAME ": %d VFs are enabled; refusing the MSI-X write\n",
			pci_num_vf(pf));
		return -EBUSY;
	}
	/* After an unbind current_state may be PCI_UNKNOWN with config access fine. */
	if (pf->current_state != PCI_D0 && pf->current_state != PCI_UNKNOWN) {
		pci_err(pf, KBUILD_MODNAME ": power state D%d; MSI-X needs D0\n",
			pf->current_state);
		return -EBUSY;
	}
	if (pf->cfg_size < e->msix_hide_pos + 4 ||
	    e->msix_pos + PCI_MSIX_FLAGS + 2 > PCI_CFG_SPACE_SIZE) {
		pci_err(pf, KBUILD_MODNAME ": config space too small (%d bytes) for MSI-X at %#x / hide register %#x\n",
			pf->cfg_size, e->msix_pos, e->msix_hide_pos);
		return -EINVAL;
	}

	/* The capability itself must be there, merely unlinked. */
	ret = pci_read_config_byte(pf, e->msix_pos + PCI_CAP_LIST_ID, &id);
	if (!ret)
		ret = pci_read_config_word(pf, e->msix_pos + PCI_MSIX_FLAGS,
					   &flags);
	if (!ret)
		ret = pci_read_config_dword(pf, e->msix_hide_pos, &cya);
	if (ret) {
		pci_err(pf, KBUILD_MODNAME ": config read failed (%d)\n", ret);
		return pcibios_err_to_errno(ret);
	}
	if (id != PCI_CAP_ID_MSIX) {
		pci_warn(pf, KBUILD_MODNAME ": no MSI-X capability at %#x (ID %#04x); MSI-X stays hidden\n",
			 e->msix_pos, id);
		return 0;
	}
	if (flags & PCI_MSIX_FLAGS_ENABLE) {
		pci_err(pf, KBUILD_MODNAME ": hidden MSI-X is already enabled (control %#06x); refusing\n",
			flags);
		return -EBUSY;
	}
	if (!(cya & BIT(GS_MSIX_HIDE_BIT))) {
		pci_warn(pf, KBUILD_MODNAME ": %#x bit %u is already clear but MSI-X is not linked; unknown state, MSI-X stays hidden\n",
			 e->msix_hide_pos, GS_MSIX_HIDE_BIT);
		return 0;
	}

	ret = gs_set_msix_hidden(pf, e, false);
	if (ret) {
		pci_err(pf, KBUILD_MODNAME ": clearing %#x bit %u failed (%d)\n",
			e->msix_hide_pos, GS_MSIX_HIDE_BIT, ret);
		return ret;
	}
	*unhid = true;

	ret = pci_read_config_dword(pf, e->msix_hide_pos, &cya);
	if (ret || (cya & BIT(GS_MSIX_HIDE_BIT))) {
		pci_err(pf, KBUILD_MODNAME ": %#x bit %u did not clear (read %#010x, %d)\n",
			e->msix_hide_pos, GS_MSIX_HIDE_BIT, cya, ret);
		return -EIO;
	}
	linked = pci_find_capability(pf, PCI_CAP_ID_MSIX);
	if (linked != e->msix_pos) {
		pci_err(pf, KBUILD_MODNAME ": the chain links MSI-X at %#x, expected %#x\n",
			linked, e->msix_pos);
		return -EIO;
	}
	pci_info(pf, KBUILD_MODNAME ": MSI-X linked at %#x, %u vectors (%#x bit %u cleared); remove and rescan the PF before a driver binds\n",
		 linked, (flags & PCI_MSIX_FLAGS_QSIZE) + 1, e->msix_hide_pos,
		 GS_MSIX_HIDE_BIT);
	return 0;
}

/*
 * gs_unhide_msix_locked() under @pf's device lock, which the driver core
 * holds while a driver binds or unbinds and sriov_numvfs_store() holds while
 * VFs are enabled or disabled: from the checks to the read-back, no driver
 * binds to @pf and no VF is enabled.
 */
static int gs_unhide_msix(struct pci_dev *pf, const struct gs_device *e,
			  bool *unhid)
{
	int ret;

	device_lock(&pf->dev);
	ret = gs_unhide_msix_locked(pf, e, unhid);
	device_unlock(&pf->dev);
	return ret;
}

/* ------------------------------------------------------- load-time checks */

/*
 * The functions of @pf's slot that hold a device, as a mask (bit n = function
 * n), the PF itself included. @pf's own VFs are left out: they are the VFs
 * the layout check places. Every reference pci_get_slot() returns is put.
 */
static unsigned int gs_slot_occupied(struct pci_dev *pf)
{
	unsigned int fn, mask = 1u << PCI_FUNC(pf->devfn);
	struct pci_dev *d;

	for (fn = 0; fn < GS_SLOT_FNS; fn++) {
		if (fn == PCI_FUNC(pf->devfn))
			continue;
		d = pci_get_slot(pf->bus, PCI_DEVFN(PCI_SLOT(pf->devfn), fn));
		if (!d)
			continue;
		if (!d->is_virtfn || pci_physfn(d) != pf)
			mask |= 1u << fn;
		pci_dev_put(d);
	}
	return mask;
}

/*
 * Validate one listed PF before anything is patched or written: identity,
 * table entry, the SR-IOV header the patch will rely on and where its VFs
 * would land. *@occupied gets the functions of its slot in use.
 */
static int gs_check_pf(struct pci_dev *pf, const struct gs_bdf *b,
		       const struct gs_device **entry, unsigned int *occupied)
{
	const struct gs_device *e;
	u16 linked, total = 0, numvfs = 0, offset = 0, stride = 0;
	enum gs_vf_geometry g;
	unsigned int limit;
	u32 hdr = 0;

	if (!pf) {
		pr_err("%04x:%02x:%02x.%x: no such PCI device\n", b->domain,
		       b->bus, PCI_SLOT(b->devfn), PCI_FUNC(b->devfn));
		return -ENODEV;
	}
	if (pf->vendor != PCI_VENDOR_ID_NVIDIA || pf->is_virtfn) {
		pci_err(pf, KBUILD_MODNAME ": %04x:%04x%s is not an NVIDIA physical function\n",
			pf->vendor, pf->device, pf->is_virtfn ? " (VF)" : "");
		return -EINVAL;
	}
	e = gs_device_lookup(pf->device);
	if (!e) {
		pci_err(pf, KBUILD_MODNAME ": 10de:%04x is not in the device table\n",
			pf->device);
		return -EINVAL;
	}

	pci_info(pf, KBUILD_MODNAME ": %s %s (%s) subsystem %04x:%04x, vGPU target 10de:%04x %s\n",
		 e->die, e->name, gs_generation_name(e->generation),
		 pf->subsystem_vendor, pf->subsystem_device,
		 gs_target_of(e)->device, gs_target_of(e)->name);

	linked = pci_find_next_ext_capability(pf, 0, PCI_EXT_CAP_ID_SRIOV);
	if (pf->cfg_size < e->sriov_pos + PCI_SRIOV_VF_STRIDE + 2 ||
	    pci_read_config_dword(pf, e->sriov_pos, &hdr) ||
	    pci_user_read_config_word(pf, e->sriov_pos + PCI_SRIOV_TOTAL_VF, &total) ||
	    pci_user_read_config_word(pf, e->sriov_pos + PCI_SRIOV_NUM_VF, &numvfs) ||
	    pci_user_read_config_word(pf, e->sriov_pos + PCI_SRIOV_VF_OFFSET, &offset) ||
	    pci_user_read_config_word(pf, e->sriov_pos + PCI_SRIOV_VF_STRIDE, &stride)) {
		pci_err(pf, KBUILD_MODNAME ": cannot read config space at %#x (size %d)\n",
			e->sriov_pos, pf->cfg_size);
		return -EIO;
	}
	if (!gs_ext_cap_hdr_is(hdr, PCI_EXT_CAP_ID_SRIOV, 1)) {
		pci_err(pf, KBUILD_MODNAME ": %#x reads %#010x, not an SR-IOV version 1 header; refusing\n",
			e->sriov_pos, hdr);
		return -EINVAL;
	}
	if (linked) {
		pci_err(pf, KBUILD_MODNAME ": SR-IOV is already linked at %#x; this card needs no livepatch; refusing\n",
			linked);
		return -EINVAL;
	}
	/*
	 * pci_user_read_config_word() never goes through the patched
	 * pci_read_config_word(), so these are the hardware values even when an
	 * earlier instance of the patch is active.
	 *
	 * TotalVFs 0: sriov_init() stops without SR-IOV state, and 0 is never
	 * raised (it may mean no usable VF resources).
	 */
	if (!total) {
		pci_err(pf, KBUILD_MODNAME ": TotalVFs at %#x reads 0; this card offers no VFs; refusing\n",
			e->sriov_pos);
		return -EINVAL;
	}
	limit = gs_vf_limit(e, total, vfs);
	/*
	 * Every VF must land on a free function 1-7 of the PF's own slot: the
	 * device decodes only function bits 2:0, so function 8 and up alias the
	 * PF and its audio function, and a VF on a function already in use would
	 * give the kernel a second device at that address. Offset and stride are
	 * read with NumVFs as found (0 at load; the public config-space dumps
	 * show 4/1 there). The values the kernel reads for each NumVFs it
	 * writes, 1 to the limit, with its own ARI setting, are checked again
	 * when it sets up SR-IOV after the rescan (gs_guard_vf_geometry()).
	 */
	*occupied = gs_slot_occupied(pf);
	g = gs_vf_geometry_check(limit, offset, stride, *occupied);
	if (g != GS_GEOM_OK) {
		pci_err(pf, KBUILD_MODNAME ": first VF offset %u / stride %u (read at NumVFs %u) for %u VFs, functions in use %#04x: %s; refusing\n",
			offset, stride, numvfs, limit, *occupied,
			gs_vf_geometry_text(g));
		return -EINVAL;
	}
	pci_info(pf, KBUILD_MODNAME ": SR-IOV hidden at %#x: hardware TotalVFs %u, vfs=%u, VF offset %u, stride %u; %u VFs visible after a rescan\n",
		 e->sriov_pos, total, vfs, offset, stride, limit);

	if (ari && pf->devfn == 0 &&
	    pci_has_flag(PCI_SCAN_ALL_PCIE_DEVS))
		pci_warn(pf, KBUILD_MODNAME ": ARI stays off for this card: pci=pcie_scan_all (or a platform quirk) scans every slot below the port, and hidden ARI would expose function aliases\n");
	else if (ari && pf->devfn == 0 &&
		 !pci_find_next_ext_capability(pf, 0, PCI_EXT_CAP_ID_ARI) &&
		 !gs_hidden_ari(pf, e))
		pci_warn(pf, KBUILD_MODNAME ": ARI stays off for this card: no usable hidden ARI capability at %#x (needs version 1 and a non-zero next function number)\n",
			 e->ari_pos);

	*entry = e;
	return 0;
}

/* --------------------------------------------------------------- livepatch */

static void gs_post_patch(struct klp_object *obj)
{
	pr_info("patch active for %u PF(s): ari=%d msix=%d vfs=%u; remove and rescan each PF now\n",
		gs_nsel, ari, msix, vfs);
}

static void gs_post_unpatch(struct klp_object *obj)
{
	pr_info("patch removed; a hidden capability stays in use until the PF is removed and rescanned\n");
}

static struct klp_func gs_funcs[] = {
	{
		.old_name = "pci_find_ext_capability",
		.new_func = gs_pci_find_ext_capability,
	}, {
		.old_name = "pci_read_config_word",
		.new_func = gs_pci_read_config_word,
	}, {
		.old_name = "pci_write_config_word",
		.new_func = gs_pci_write_config_word,
	}, { }
};

static struct klp_object gs_objs[] = {
	{
		/* name NULL means vmlinux */
		.funcs = gs_funcs,
		.callbacks = {
			.post_patch = gs_post_patch,
			.post_unpatch = gs_post_unpatch,
		},
	}, { }
};

static struct klp_patch gs_patch = {
	.mod = THIS_MODULE,
	.objs = gs_objs,
};

static int __init geforce_sriov_init(void)
{
	struct pci_dev *pf[GS_MAX_PFS] = { };
	const struct gs_device *e[GS_MAX_PFS] = { };
	bool unhid[GS_MAX_PFS] = { };
	unsigned int i;
	int n, ret = 0, ret2;

	gs_init_watch_where();
	if (vfs < 1 || vfs > GS_VFS_MAX) {
		pr_err("vfs must be 1-%u, got %u\n", GS_VFS_MAX, vfs);
		return -EINVAL;
	}
	n = gs_parse_pf_bdf_list(bdfs, gs_sel, GS_MAX_PFS);
	if (n < 0) {
		pr_err("bdfs must be 1-%u comma-separated PFs dddd:bb:ss.0 as named in /sys/bus/pci/devices (lowercase, function 0, domain up to %x, no repeats), got \"%s\"\n",
		       GS_MAX_PFS, GS_PCI_DOMAIN_MAX, bdfs);
		return -EINVAL;
	}
	if (n == 0) {
		pr_err("bdfs is empty; list the PFs to activate (no card is changed without being listed)\n");
		return -EINVAL;
	}

	for (i = 0; i < (unsigned int)n; i++) {
		/* The parser keeps the domain <= GS_PCI_DOMAIN_MAX (INT_MAX). */
		pf[i] = pci_get_domain_bus_and_slot((int)gs_sel[i].domain,
						    gs_sel[i].bus,
						    gs_sel[i].devfn);
		ret = gs_check_pf(pf[i], &gs_sel[i], &e[i], &gs_occupied[i]);
		if (ret)
			goto out_put;
	}

	if (msix) {
		for (i = 0; i < (unsigned int)n; i++) {
			ret = gs_unhide_msix(pf[i], e[i], &unhid[i]);
			if (ret)
				goto out_restore;
		}
	}

	gs_nsel = n;
	ret = klp_enable_patch(&gs_patch);
	if (ret) {
		gs_nsel = 0;
		pr_err("klp_enable_patch failed (%d); nothing is patched\n", ret);
		goto out_restore;
	}
	goto out_put;

out_restore:
	for (i = 0; i < (unsigned int)n; i++) {
		if (!unhid[i])
			continue;
		device_lock(&pf[i]->dev);
		ret2 = gs_set_msix_hidden(pf[i], e[i], true);
		device_unlock(&pf[i]->dev);
		if (ret2)
			pci_err(pf[i], KBUILD_MODNAME ": could not set %#x bit %u again; a cold boot restores it\n",
				e[i]->msix_hide_pos, GS_MSIX_HIDE_BIT);
	}
out_put:
	for (i = 0; i < (unsigned int)n; i++)
		pci_dev_put(pf[i]);
	return ret;
}

static void __exit geforce_sriov_exit(void)
{
}

module_init(geforce_sriov_init);
module_exit(geforce_sriov_exit);
MODULE_DESCRIPTION("Livepatch: SR-IOV, ARI and MSI-X presentation and a VF count (default 4) for listed NVIDIA PFs (geforce-sriov " GS_VERSION ")");
MODULE_VERSION(GS_VERSION);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_INFO(livepatch, "Y");
