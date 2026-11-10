/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * geforce-sriov device table: the only place that names a GeForce card.
 *
 * Shared by the kernel livepatch (kernel/geforce_sriov.c), the CLI and the
 * identity hook. It uses only plain C types and needs no header, so it builds
 * unchanged in the kernel and in user space.
 *
 * Every entry is one exact, reviewed PCI device ID. There are no ranges: an ID
 * that is not listed (Turing, Blackwell, an unreleased part, or any other ID)
 * gets nothing from any component.
 *
 * What an entry claims, and what it does not
 * ------------------------------------------
 * - Hidden capability positions come from public raw config-space dumps of
 *   cards with that ID. They show the *shape* of the capability on cards of
 *   that ID; they do not show that SR-IOV works there.
 *   The kernel module re-reads and validates every header on the running card
 *   before it uses a position, and does nothing when the header does not match.
 * - An entry names a device ID the components act on; it does not certify
 *   every board with that ID. Every listed card is supported, the laptop rows
 *   included (see the note above gs_devices[]); the evidence is in
 *   docs/ampere.md and docs/ada.md (Status and testing).
 * - An Ada card whose hardware TotalVFs is 1 is presented with
 *   GS_DEFAULT_VFS VFs (gs_vf_ceiling()).
 * - The vGPU target presented to nvidia-vgpud depends on the generation only
 *   (gs_targets[], gs_target_of()). Exposing a capability is not the same as
 *   a working vGPU.
 *
 * Fields
 * ------
 * device            PCI device ID (vendor is always GS_VENDOR_NVIDIA)
 * sriov_pos         hidden (unlinked) SR-IOV extended capability offset
 * ari_pos           hidden ARI extended capability offset
 * msix_pos          MSI-X capability offset in the standard chain
 * msix_hide_pos     config dword whose bit GS_MSIX_HIDE_BIT unlinks MSI-X
 * totalvfs          hardware TotalVFs in the public dumps of this ID (the
 *                   card's own default; the module reads the real register)
 * generation        enum gs_generation; it decides the vGPU target
 * laptop            1 on a laptop row (GS_AMPERE_LAPTOP_ENTRY, GS_ADA_LAPTOP_ENTRY):
 *                   the CLI adds its laptop power guards (runtime D3cold) for it
 * die, name         display strings
 */
#ifndef GS_DEVICES_H
#define GS_DEVICES_H

#define GS_VERSION		"0.2.2"
#define GS_VENDOR_NVIDIA	0x10deu

/*
 * VFs per card when no count is configured (module parameter vfs, config key
 * vfs=). ARI is enabled in software, but ARI decoding in the GPU does not
 * work, so with larger counts the module refuses to load if the card's VF
 * layout check fails (gs_vf_geometry_check() refuses VFs beyond function 7
 * of the PF's slot).
 */
#define GS_DEFAULT_VFS		4u

/* Largest VF count that can be configured: the largest totalvfs in the table. */
#define GS_VFS_MAX		16u

/* Most physical functions one module load / one config may list. */
#define GS_MAX_PFS		8u

/*
 * PCI addresses are written as Linux names them ("%04x:%02x:%02x.%d", see
 * pci_name()): the domain takes 4 to 8 lowercase hex digits (more than 4
 * only when the value needs them, e.g. 10000 behind Intel VMD). Linux keeps
 * the domain in an int (pci_domain_nr()), so no domain exceeds 0x7fffffff.
 * Sizes include the terminating NUL:
 *   GS_BDF_STR_SIZE       "dddddddd:bb:ss.f"
 *   GS_BUS_STR_SIZE       "dddddddd:bb" (as in /sys/class/pci_bus)
 *   GS_BDF_LIST_STR_SIZE  GS_MAX_PFS BDFs separated by commas
 */
#define GS_PCI_DOMAIN_MAX	0x7fffffffu
#define GS_BDF_STR_SIZE		17u
#define GS_BUS_STR_SIZE		12u
#define GS_BDF_LIST_STR_SIZE	(GS_MAX_PFS * GS_BDF_STR_SIZE)

/*
 * PCI functions per slot without ARI. Every listed card is held to them
 * (docs/ampere.md#known-limits).
 */
#define GS_SLOT_FNS		8u

/*
 * 0x41C bit 29 is NV_XVE_PRIV_MISC_1_CYA_HIDE_MSIX_CAP in NVIDIA's GA102 XVE
 * register manual. Bit set = MSI-X unlinked from the standard chain; clearing
 * it relinks MSI-X at 0xC8. A bus remove/rescan keeps the cleared value, and
 * FLR or secondary-bus reset sets it again
 * (docs/technical-reference.md#the-kernel-module).
 */
#define GS_MSIX_HIDE_BIT	29u

/* PCIe extended capability IDs used by the table (PCIe base spec). */
#define GS_EXT_CAP_ID_ARI	0x000eu
#define GS_EXT_CAP_ID_SRIOV	0x0010u
/* PCI capability ID of MSI-X. */
#define GS_CAP_ID_MSIX		0x11u

enum gs_generation { GS_AMPERE, GS_ADA };

struct gs_device {
	unsigned short device;
	unsigned short sriov_pos;
	unsigned short ari_pos;
	unsigned short msix_pos;
	unsigned short msix_hide_pos;
	unsigned short totalvfs;
	unsigned char generation;
	unsigned char laptop;
	const char *die;
	const char *name;
};

/*
 * Ampere (GA10x): 0xBB0 ReBAR header c1c10015 links to 0xC1C and skips a
 * hidden SR-IOV header c1410010 at 0xBCC (TotalVFs 16, first VF offset 4,
 * stride 1) and a hidden ARI header c1c1000e at 0xC14, in every Ampere GeForce
 * dump checked. MSI-X sits at 0xC8; on every Ampere dump with MSI-X hidden,
 * 0x41C bit 29 is set.
 * The card decodes only 3 bits of PCI function number: only functions 4-7
 * reach distinct VFs, and functions >= 8 alias the PF and its audio function,
 * so gs_vf_geometry_check() refuses VFs there.
 */
#define GS_AMPERE_ROW(dev, dname, pname, lap) \
	{ (dev), 0xbccu, 0xc14u, 0xc8u, 0x41cu, 16u, GS_AMPERE, (lap), (dname), (pname) }
#define GS_AMPERE_ENTRY(dev, dname, pname) GS_AMPERE_ROW(dev, dname, pname, 0)
#define GS_AMPERE_LAPTOP_ENTRY(dev, dname, pname) GS_AMPERE_ROW(dev, dname, pname, 1)

/*
 * Ada (AD10x): the same hidden SR-IOV (0xBCC) and ARI (0xC14) headers as
 * Ampere in every Ada dump checked. TotalVFs = InitialVFs = 1 on the GeForce
 * cards, the AD104 workstation cards and the RTX 2000 Ada Laptop (28b8), 16
 * on the AD107 RTX 2000 / 2000E Ada desktop card (28b0) (tvfs).
 * MSI-X is at 0xC8 and hidden by the same bit as on Ampere (0x41C bit 29; the
 * public config-space dumps record it set on every Ada GeForce row), so the
 * entry takes the same path as GS_AMPERE_ENTRY. NVIDIA's driver reads the ID
 * and TotalVFs of these chips through a BAR0 mirror, not config reads.
 */
#define GS_ADA_ROW(dev, dname, pname, tvfs, lap) \
	{ (dev), 0xbccu, 0xc14u, 0xc8u, 0x41cu, (tvfs), GS_ADA, (lap), (dname), (pname) }
#define GS_ADA_ENTRY(dev, dname, pname, tvfs) GS_ADA_ROW(dev, dname, pname, tvfs, 0)
#define GS_ADA_LAPTOP_ENTRY(dev, dname, pname, tvfs) GS_ADA_ROW(dev, dname, pname, tvfs, 1)

/*
 * The vGPU target of each generation: a <pgpu> devId of the stock
 * vgpuConfig.xml of 580.159.01 whose subsystemId is 0x0 (matches any), so
 * the board's own subsystem is kept. GA102 cards share the RTX A6000's die
 * and AD102 cards the RTX 6000 Ada's; every other die is presented as the
 * same target. No profile is filtered against the card's real framebuffer:
 * the target's profiles, up to 48 GiB, are offered to cards with less memory.
 */
struct gs_target {
	unsigned short device;
	const char *name;
};

static const struct gs_target gs_targets[] = {
	[GS_AMPERE] = { 0x2230, "RTX A6000" },
	[GS_ADA] = { 0x26b1, "RTX 6000 Ada" },
};

#define GS_GENERATION_COUNT (sizeof(gs_targets) / sizeof(gs_targets[0]))

/*
 * Laptop rows (GS_*_LAPTOP_ENTRY, also named in each die's comment). They
 * rest on public raw 4 KiB config-space dumps of at least two machines per
 * ID, and of one machine only for 249d, 24dd, 24e0, 2523, 25a0 and 25ba.
 * Every dump has the hidden SR-IOV, ARI and MSI-X capabilities at the same
 * offsets as the desktop rows of its generation, with TotalVFs 16 on Ampere
 * and 1 on Ada. They take the desktop path; the laptop flag only adds the
 * CLI's power guards, because a laptop GPU can be powered off (D3cold). No
 * laptop has been run in our lab. Laptop IDs without a dump are in their own
 * block at the end of the table.
 */
static const struct gs_device gs_devices[] = {
	/* GA102 */
	GS_AMPERE_ENTRY(0x2203, "GA102", "GeForce RTX 3090 Ti"),
	GS_AMPERE_ENTRY(0x2204, "GA102", "GeForce RTX 3090"),
	GS_AMPERE_ENTRY(0x2206, "GA102", "GeForce RTX 3080"),
	GS_AMPERE_ENTRY(0x2208, "GA102", "GeForce RTX 3080 Ti"),
	GS_AMPERE_ENTRY(0x220a, "GA102", "GeForce RTX 3080 12GB"),
	GS_AMPERE_ENTRY(0x2216, "GA102", "GeForce RTX 3080 LHR"),

	/* GA104. Laptop: 249c, 249d, 24a0, 24b6, 24dc, 24dd, 24e0. */
	GS_AMPERE_ENTRY(0x2482, "GA104", "GeForce RTX 3070 Ti"),
	GS_AMPERE_ENTRY(0x2484, "GA104", "GeForce RTX 3070"),
	GS_AMPERE_ENTRY(0x2486, "GA104", "GeForce RTX 3060 Ti"),
	GS_AMPERE_ENTRY(0x2487, "GA104", "GeForce RTX 3060"),
	GS_AMPERE_ENTRY(0x2488, "GA104", "GeForce RTX 3070 LHR"),
	GS_AMPERE_ENTRY(0x2489, "GA104", "GeForce RTX 3060 Ti LHR"),
	GS_AMPERE_LAPTOP_ENTRY(0x249c, "GA104", "GeForce RTX 3080 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x249d, "GA104", "GeForce RTX 3070 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x24a0, "GA104", "GeForce RTX 3070 Ti Laptop GPU"),
	GS_AMPERE_ENTRY(0x24b0, "GA104", "RTX A4000"),
	GS_AMPERE_LAPTOP_ENTRY(0x24b6, "GA104", "RTX A5000 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x24dc, "GA104", "GeForce RTX 3080 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x24dd, "GA104", "GeForce RTX 3070 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x24e0, "GA104", "GeForce RTX 3070 Ti Laptop GPU"),

	/* GA106. Laptop: 2520, 2523, 2560. */
	GS_AMPERE_ENTRY(0x2503, "GA106", "GeForce RTX 3060"),
	GS_AMPERE_ENTRY(0x2504, "GA106", "GeForce RTX 3060 LHR"),
	GS_AMPERE_ENTRY(0x2507, "GA106", "GeForce RTX 3050"),
	GS_AMPERE_LAPTOP_ENTRY(0x2520, "GA106", "GeForce RTX 3060 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x2523, "GA106", "GeForce RTX 3050 Ti Laptop GPU"),
	GS_AMPERE_ENTRY(0x2544, "GA106", "GeForce RTX 3060"),
	GS_AMPERE_LAPTOP_ENTRY(0x2560, "GA106", "GeForce RTX 3060 Laptop GPU"),
	GS_AMPERE_ENTRY(0x2571, "GA106", "RTX A2000 12GB"),

	/*
	 * GA107. Laptop: 25a0, 25a2, 25b9, 25ba, 25ec. No desktop GA107
	 * GeForce dump was available; the hidden block is shown on GA107
	 * laptop parts (25a0, 25a2) and the GA107 RTX A1000/A400. The die of
	 * 2582/2584 is from pci.ids.
	 */
	GS_AMPERE_ENTRY(0x2582, "GA107", "GeForce RTX 3050 8GB"),
	GS_AMPERE_ENTRY(0x2584, "GA107", "GeForce RTX 3050 6GB"),
	GS_AMPERE_LAPTOP_ENTRY(0x25a0, "GA107", "GeForce RTX 3050 Ti Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25a2, "GA107", "GeForce RTX 3050 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25b9, "GA107", "RTX A1000 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25ba, "GA107", "RTX A2000 8GB Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25ec, "GA107", "GeForce RTX 3050 6GB Laptop GPU"),

	/* AD102 */
	GS_ADA_ENTRY(0x2684, "AD102", "GeForce RTX 4090", 1u),
	GS_ADA_ENTRY(0x2685, "AD102", "GeForce RTX 4090 D", 1u),
	GS_ADA_ENTRY(0x2689, "AD102", "GeForce RTX 4070 Ti SUPER", 1u),

	/* AD103. Laptop: 2717, 2757. */
	GS_ADA_ENTRY(0x2702, "AD103", "GeForce RTX 4080 SUPER", 1u),
	GS_ADA_ENTRY(0x2704, "AD103", "GeForce RTX 4080", 1u),
	GS_ADA_ENTRY(0x2705, "AD103", "GeForce RTX 4070 Ti SUPER", 1u),
	GS_ADA_ENTRY(0x2709, "AD103", "GeForce RTX 4070", 1u),
	GS_ADA_LAPTOP_ENTRY(0x2717, "AD103", "GeForce RTX 4090 Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x2757, "AD103", "GeForce RTX 4090 Laptop GPU", 1u),

	/* AD104. Laptop: 27e0. */
	GS_ADA_ENTRY(0x2782, "AD104", "GeForce RTX 4070 Ti", 1u),
	GS_ADA_ENTRY(0x2783, "AD104", "GeForce RTX 4070 SUPER", 1u),
	GS_ADA_ENTRY(0x2786, "AD104", "GeForce RTX 4070", 1u),
	GS_ADA_ENTRY(0x2788, "AD104", "GeForce RTX 4060 Ti", 1u),
	GS_ADA_ENTRY(0x27b1, "AD104", "RTX 4500 Ada Generation", 1u),
	GS_ADA_ENTRY(0x27b2, "AD104", "RTX 4000 Ada Generation", 1u),
	GS_ADA_LAPTOP_ENTRY(0x27e0, "AD104", "GeForce RTX 4080 Laptop GPU", 1u),

	/* AD106, AD107. Laptop: 2820, 2860, 28a0, 28a1, 28b8, 28e0, 28e1. */
	GS_ADA_ENTRY(0x2803, "AD106", "GeForce RTX 4060 Ti", 1u),
	GS_ADA_ENTRY(0x2805, "AD106", "GeForce RTX 4060 Ti 16GB", 1u),
	GS_ADA_LAPTOP_ENTRY(0x2820, "AD106", "GeForce RTX 4070 Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x2860, "AD106", "GeForce RTX 4070 Laptop GPU", 1u),
	GS_ADA_ENTRY(0x2882, "AD107", "GeForce RTX 4060", 1u),
	GS_ADA_LAPTOP_ENTRY(0x28a0, "AD107", "GeForce RTX 4060 Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x28a1, "AD107", "GeForce RTX 4050 Laptop GPU", 1u),
	GS_ADA_ENTRY(0x28b0, "AD107", "RTX 2000 / 2000E Ada Generation", 16u),
	GS_ADA_LAPTOP_ENTRY(0x28b8, "AD107", "RTX 2000 Ada Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x28e0, "AD107", "GeForce RTX 4060 Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x28e1, "AD107", "GeForce RTX 4050 Laptop GPU", 1u),
	/*
	 * Laptop and embedded IDs with no config-space dump. No dump exists for these
	 * IDs; they are listed because the laptop parts of a generation share the
	 * layout of the dumped ones (the block above). Die and name are from
	 * pci.ids. The kernel module still validates every header on the card.
	 */
	GS_AMPERE_LAPTOP_ENTRY(0x24b7, "GA104", "RTX A4000 Mobile"),
	GS_AMPERE_LAPTOP_ENTRY(0x24b8, "GA104", "RTX A3000 Mobile"),
	GS_AMPERE_LAPTOP_ENTRY(0x24b9, "GA104", "RTX A3000 12GB Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x24ba, "GA104", "RTX A4500 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x24bb, "GA104", "RTX A3000 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x2563, "GA106", "GeForce RTX 3050 Ti Mobile / Max-Q"),
	GS_AMPERE_LAPTOP_ENTRY(0x25a5, "GA107", "GeForce RTX 3050 Mobile"),
	GS_AMPERE_LAPTOP_ENTRY(0x25a6, "GA107", "GeForce MX570"),
	GS_AMPERE_LAPTOP_ENTRY(0x25a7, "GA107", "GeForce MX570"),
	GS_AMPERE_LAPTOP_ENTRY(0x25a9, "GA107", "GeForce RTX 2050"),
	GS_AMPERE_LAPTOP_ENTRY(0x25ab, "GA107", "GeForce RTX 3050 4GB Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25ac, "GA107", "GeForce RTX 3050 6GB Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25ad, "GA107", "GeForce RTX 2050"),
	GS_AMPERE_LAPTOP_ENTRY(0x25b8, "GA107", "RTX A2000 Mobile"),
	GS_AMPERE_LAPTOP_ENTRY(0x25bb, "GA107", "RTX A500 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25bc, "GA107", "RTX A1000 6GB Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25bd, "GA107", "RTX A500 Laptop GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25e0, "GA107", "GeForce RTX 3050 Ti Mobile"),
	GS_AMPERE_LAPTOP_ENTRY(0x25e2, "GA107", "GeForce RTX 3050 Mobile"),
	GS_AMPERE_LAPTOP_ENTRY(0x25e5, "GA107", "GeForce RTX 3050 Mobile"),
	GS_AMPERE_LAPTOP_ENTRY(0x25ed, "GA107", "GeForce RTX 2050"),
	GS_AMPERE_LAPTOP_ENTRY(0x25f9, "GA107", "RTX A1000 Embedded GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25fa, "GA107", "RTX A2000 Embedded GPU"),
	GS_AMPERE_LAPTOP_ENTRY(0x25fb, "GA107", "RTX A500 Embedded GPU"),
	GS_ADA_LAPTOP_ENTRY(0x2730, "AD103", "RTX 5000 Ada Generation Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x2770, "AD103", "RTX 5000 Ada Generation Embedded GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x27a0, "AD104", "GeForce RTX 4080 Max-Q / Mobile", 1u),
	GS_ADA_LAPTOP_ENTRY(0x27ba, "AD104", "RTX 4000 Ada Generation Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x27bb, "AD104", "RTX 3500 Ada Generation Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x27fb, "AD104", "RTX 3500 Ada Generation Embedded GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x2838, "AD106", "RTX 3000 Ada Generation Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x28b9, "AD107", "RTX 1000 Ada Generation Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x28ba, "AD107", "RTX 500 Ada Generation Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x28bb, "AD107", "RTX 500 Ada Generation Laptop GPU", 1u),
	GS_ADA_LAPTOP_ENTRY(0x28e3, "AD107", "GeForce RTX 3050 A Laptop GPU", 1u),
};

#define GS_DEVICE_COUNT (sizeof(gs_devices) / sizeof(gs_devices[0]))

/* The table entry for an NVIDIA device ID, or 0 (NULL) if it is not listed. */
static inline const struct gs_device *gs_device_lookup(unsigned int device)
{
	unsigned int i;

	for (i = 0; i < GS_DEVICE_COUNT; i++)
		if (gs_devices[i].device == device)
			return &gs_devices[i];
	return 0;
}

static inline const char *gs_generation_name(unsigned int generation)
{
	switch (generation) {
	case GS_AMPERE:
		return "Ampere";
	case GS_ADA:
		return "Ada";
	}
	return "unknown";
}

/* The vGPU target an entry is presented as: its generation's. */
static inline const struct gs_target *gs_target_of(const struct gs_device *d)
{
	return &gs_targets[d->generation];
}

/*
 * Is @hdr an extended capability header with this ID and version?
 * (ID in bits 15:0, version in bits 19:16.)
 */
static inline int gs_ext_cap_hdr_is(unsigned int hdr, unsigned int id,
				    unsigned int version)
{
	return (hdr & 0xffffu) == id && ((hdr >> 16) & 0xfu) == version;
}

/*
 * The most VFs geforce-sriov may present for a card whose hardware TotalVFs
 * register reads @raw: @raw itself, except that a raw value of exactly 1 (the
 * count most Ada dumps report) counts as GS_DEFAULT_VFS on Ada. 0 stays 0: it
 * may mean no usable VF resources, and no dump shows it.
 */
static inline unsigned int gs_vf_ceiling(const struct gs_device *d,
					 unsigned int raw)
{
	if (raw == 1 && d->generation == GS_ADA)
		return GS_DEFAULT_VFS;
	return raw;
}

/*
 * The TotalVFs/InitialVFs value the kernel is shown: the configured count
 * @vfs, never more than gs_vf_ceiling() (Ampere reads 16 and gets 4 with the
 * default count).
 */
static inline unsigned int gs_vf_limit(const struct gs_device *d,
				       unsigned int raw, unsigned int vfs)
{
	unsigned int ceiling = gs_vf_ceiling(d, raw);

	return vfs < ceiling ? vfs : ceiling;
}

/*
 * Where SR-IOV puts the VFs of a PF at function 0 of a slot: VF i is routing
 * ID PF + offset + i * stride (PCIe Base Spec, SR-IOV First VF Offset and VF
 * Stride; Linux pci_iov_virtfn_devfn()). Every listed card is held to the
 * conservative rule that it decodes only function bits 2:0 (GS_SLOT_FNS):
 * every VF must land on functions 1-7 of the PF's own slot, on a function
 * no other device already uses.
 */
enum gs_vf_geometry {
	GS_GEOM_OK = 0,
	GS_GEOM_OFFSET_ZERO,	/* VF0 would be the PF itself */
	GS_GEOM_STRIDE_ZERO,	/* several VFs on one function */
	GS_GEOM_BEYOND_SLOT,	/* a VF beyond function 7 of the PF's slot */
	GS_GEOM_COLLISION,	/* a VF on a function that is already in use */
	GS_GEOM_NUMVFS_MISMATCH,	/* NumVFs reads back another value */
};

/*
 * Check @numvfs VFs at first VF offset @offset and stride @stride (register
 * values, 16 bits) against @occupied, the mask of functions of the slot that
 * are already in use (bit n = function n). @numvfs 0 places no VF and is OK.
 * Never overflows, and loops at most GS_SLOT_FNS - 1 times.
 */
static inline enum gs_vf_geometry
gs_vf_geometry_check(unsigned int numvfs, unsigned int offset,
		     unsigned int stride, unsigned int occupied)
{
	unsigned int i, fn;

	if (!numvfs)
		return GS_GEOM_OK;
	if (!offset)
		return GS_GEOM_OFFSET_ZERO;
	if (numvfs > 1 && !stride)
		return GS_GEOM_STRIDE_ZERO;
	if (offset > GS_SLOT_FNS - 1 ||
	    (numvfs > 1 &&
	     stride > (GS_SLOT_FNS - 1 - offset) / (numvfs - 1)))
		return GS_GEOM_BEYOND_SLOT;
	/* Now offset + stride * (numvfs - 1) <= 7, so numvfs <= 7. */
	for (i = 0, fn = offset; i < numvfs; i++, fn += stride)
		if (occupied & (1u << fn))
			return GS_GEOM_COLLISION;
	return GS_GEOM_OK;
}

/*
 * The VF layout decision while Linux sets up SR-IOV: @requested is the
 * NumVFs value the kernel wrote, @readback what NumVFs reads afterwards, and
 * @offset/@stride what the card then reports. Requested 0 places no VF. A
 * card that clamps or ignores the write (readback != requested) is refused,
 * because the kernel would later create @requested VFs; otherwise the layout
 * of @requested VFs is checked.
 */
static inline enum gs_vf_geometry
gs_vf_setup_check(unsigned int requested, unsigned int readback,
		  unsigned int offset, unsigned int stride,
		  unsigned int occupied)
{
	if (!requested)
		return GS_GEOM_OK;
	if (readback != requested)
		return GS_GEOM_NUMVFS_MISMATCH;
	return gs_vf_geometry_check(requested, offset, stride, occupied);
}

static inline const char *gs_vf_geometry_text(enum gs_vf_geometry g)
{
	switch (g) {
	case GS_GEOM_OK:
		return "every VF on its own free function 1-7 of the PF's slot";
	case GS_GEOM_OFFSET_ZERO:
		return "first VF offset 0 puts VF0 on the PF itself";
	case GS_GEOM_STRIDE_ZERO:
		return "VF stride 0 puts every VF on one function";
	case GS_GEOM_BEYOND_SLOT:
		return "a VF would land beyond function 7 of the PF's slot (such a function may answer as the PF or its audio function)";
	case GS_GEOM_COLLISION:
		return "a VF would land on a function another device already uses";
	case GS_GEOM_NUMVFS_MISMATCH:
		return "NumVFs does not read back the value the kernel wrote";
	}
	return "unknown";
}

/* A PCI function address. devfn = slot << 3 | function. */
struct gs_bdf {
	unsigned int domain;
	unsigned int bus;
	unsigned int devfn;
};

static inline int gs_hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* Parse exactly @n lowercase hex digits at *@s and advance it. */
static inline int gs_hex_n(const char **s, unsigned int n, unsigned int *out)
{
	unsigned int v = 0, i;
	int d;

	for (i = 0; i < n; i++) {
		d = gs_hex_digit((*s)[i]);
		if (d < 0)
			return 0;
		v = v * 16 + (unsigned int)d;
	}
	*s += n;
	*out = v;
	return 1;
}

/*
 * Parse a canonical PCI domain at *@s and advance it: 4 to 8 lowercase hex
 * digits, more than 4 only without a leading 0 (as "%04x" prints it), value
 * at most GS_PCI_DOMAIN_MAX. Returns 1 on success, 0 on any deviation.
 */
static inline int gs_parse_domain_at(const char **s, unsigned int *out)
{
	const char *p = *s;
	unsigned int n = 0, v;

	while (n <= 8 && gs_hex_digit(p[n]) >= 0)
		n++;
	if (n < 4 || n > 8 || (n > 4 && p[0] == '0'))
		return 0;
	if (!gs_hex_n(&p, n, &v) || v > GS_PCI_DOMAIN_MAX)
		return 0;
	*s = p;
	*out = v;
	return 1;
}

/*
 * Parse one canonical PCI function address "dddd:bb:ss.f" (lowercase hex,
 * exactly as Linux names the device in sysfs; the domain may have up to 8
 * digits, see gs_parse_domain_at()) from *@s, advancing *@s past it.
 * Returns 1 on success, 0 on any deviation.
 */
static inline int gs_parse_bdf_at(const char **s, struct gs_bdf *out)
{
	const char *p = *s;
	unsigned int dom, bus, slot, fn;

	if (!gs_parse_domain_at(&p, &dom) || *p++ != ':')
		return 0;
	if (!gs_hex_n(&p, 2, &bus) || *p++ != ':')
		return 0;
	if (!gs_hex_n(&p, 2, &slot) || *p++ != '.')
		return 0;
	if (!gs_hex_n(&p, 1, &fn))
		return 0;
	if (slot > 0x1f || fn > 7)
		return 0;
	out->domain = dom;
	out->bus = bus;
	out->devfn = slot << 3 | fn;
	*s = p;
	return 1;
}

/* Parse a whole string that must be exactly one canonical address. */
static inline int gs_parse_bdf(const char *s, struct gs_bdf *out)
{
	if (!s || !gs_parse_bdf_at(&s, out))
		return 0;
	return *s == '\0';
}

/*
 * Parse one canonical physical-function address (gs_parse_bdf_at()) whose
 * function is 0: the card's PF is function 0 and its VFs are other
 * functions of the same slot. Returns 1 on success, 0 on any deviation.
 */
static inline int gs_parse_pf_bdf_at(const char **s, struct gs_bdf *out)
{
	const char *p = *s;
	struct gs_bdf b;

	if (!gs_parse_bdf_at(&p, &b) || (b.devfn & 7u) != 0)
		return 0;
	*out = b;
	*s = p;
	return 1;
}

/* Parse a whole string that must be exactly one canonical PF BDF. */
static inline int gs_parse_pf_bdf(const char *s, struct gs_bdf *out)
{
	if (!s || !gs_parse_pf_bdf_at(&s, out))
		return 0;
	return *s == '\0';
}

/*
 * Parse a comma-separated list of canonical PF BDFs ("" is an empty list)
 * into @out (room for @max). Returns the number parsed, or -1 if a token is
 * not canonical, a BDF repeats, or there are more than @max.
 */
static inline int gs_parse_pf_bdf_list(const char *s, struct gs_bdf *out,
				       unsigned int max)
{
	unsigned int n = 0, i;
	struct gs_bdf b;

	if (!s)
		return -1;
	if (*s == '\0')
		return 0;
	for (;;) {
		if (!gs_parse_pf_bdf_at(&s, &b))
			return -1;
		for (i = 0; i < n; i++)
			if (out[i].domain == b.domain && out[i].bus == b.bus &&
			    out[i].devfn == b.devfn)
				return -1;
		if (n == max)
			return -1;
		out[n++] = b;
		if (*s == '\0')
			return (int)n;
		if (*s++ != ',')
			return -1;
	}
}

#endif /* GS_DEVICES_H */
