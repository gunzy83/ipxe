/*
 * Copyright (C) 2014 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <ipxe/malloc.h>
#include <ipxe/pci.h>
#include <ipxe/cpuid.h>
#include <ipxe/msr.h>
#include <ipxe/xen.h>
#include <ipxe/xenver.h>
#include <ipxe/xenmem.h>
#include <ipxe/xenstore.h>
#include <ipxe/xenbus.h>
#include <ipxe/xengrant.h>
#include "hvm.h"

/** @file
 *
 * Xen HVM driver
 *
 */

/**
 * Get CPUID base
 *
 * @v hvm		HVM device
 * @ret rc		Return status code
 */
static int hvm_cpuid_base ( struct hvm_device *hvm ) {
	struct {
		uint32_t ebx;
		uint32_t ecx;
		uint32_t edx;
	} __attribute__ (( packed )) signature;
	uint32_t base;
	uint32_t version;
	uint32_t discard_eax;
	uint32_t discard_ebx;
	uint32_t discard_ecx;
	uint32_t discard_edx;

	/* Scan for magic signature */
	for ( base = HVM_CPUID_MIN ; base <= HVM_CPUID_MAX ;
	      base += HVM_CPUID_STEP ) {
		cpuid ( base, &discard_eax, &signature.ebx, &signature.ecx,
			&signature.edx );
		if ( memcmp ( &signature, HVM_CPUID_MAGIC,
			      sizeof ( signature ) ) == 0 ) {
			hvm->cpuid_base = base;
			cpuid ( ( base + HVM_CPUID_VERSION ), &version,
				&discard_ebx, &discard_ecx, &discard_edx );
			DBGC2 ( hvm, "HVM using CPUID base %#08x (v%d.%d)\n",
				base, ( version >> 16 ), ( version & 0xffff ) );
			return 0;
		}
	}

	DBGC ( hvm, "HVM could not find hypervisor\n" );
	return -ENODEV;
}

/**
 * Map hypercall page(s)
 *
 * @v hvm		HVM device
 * @ret rc		Return status code
 */
static int hvm_map_hypercall ( struct hvm_device *hvm ) {
	uint32_t pages;
	uint32_t msr;
	uint32_t discard_ecx;
	uint32_t discard_edx;
	physaddr_t hypercall_phys;
	uint32_t version;
	static xen_extraversion_t extraversion;
	int xenrc;
	int rc;

	/* Get number of hypercall pages and MSR to use */
	cpuid ( ( hvm->cpuid_base + HVM_CPUID_PAGES ), &pages, &msr,
		&discard_ecx, &discard_edx );

	/* Allocate pages */
	hvm->hypercall_len = ( pages * PAGE_SIZE );
	hvm->xen.hypercall = malloc_dma ( hvm->hypercall_len, PAGE_SIZE );
	if ( ! hvm->xen.hypercall ) {
		DBGC ( hvm, "HVM could not allocate %d hypercall page(s)\n",
		       pages );
		return -ENOMEM;
	}
	hypercall_phys = virt_to_phys ( hvm->xen.hypercall );
	DBGC2 ( hvm, "HVM hypercall page(s) at [%#08lx,%#08lx) via MSR %#08x\n",
		hypercall_phys, ( hypercall_phys + hvm->hypercall_len ), msr );

	/* Write to MSR */
	wrmsr ( msr, hypercall_phys );

	/* Check that hypercall mechanism is working */
	version = xenver_version ( &hvm->xen );
	if ( ( xenrc = xenver_extraversion ( &hvm->xen, &extraversion ) ) != 0){
		rc = -EXEN ( xenrc );
		DBGC ( hvm, "HVM could not get extraversion: %s\n",
		       strerror ( rc ) );
		return rc;
	}
	DBGC2 ( hvm, "HVM found Xen version %d.%d%s\n",
		( version >> 16 ), ( version & 0xffff ) , extraversion );

	return 0;
}

/**
 * Unmap hypercall page(s)
 *
 * @v hvm		HVM device
 */
static void hvm_unmap_hypercall ( struct hvm_device *hvm ) {

	/* Free pages */
	free_dma ( hvm->xen.hypercall, hvm->hypercall_len );
}

/**
 * Allocate and map MMIO space
 *
 * @v hvm		HVM device
 * @v space		Source mapping space
 * @v pages		Number of pages
 * @ret mmio		MMIO space address, or NULL on error
 */
static void * hvm_ioremap ( struct hvm_device *hvm, unsigned int space,
			    unsigned int pages ) {
	struct xen_add_to_physmap add;
	struct xen_remove_from_physmap remove;
	physaddr_t mmio_phys;
	unsigned int i;
	size_t len;
	void *mmio;
	int xenrc;
	int rc;

	/* Check for available space */
	len = ( pages * PAGE_SIZE );
	if ( ( hvm->mmio_offset + len ) > hvm->mmio_len ) {
		DBGC ( hvm, "HVM could not allocate %zd bytes of MMIO space "
		       "(%zd of %zd remaining)\n", len,
		       ( hvm->mmio_len - hvm->mmio_offset ), hvm->mmio_len );
		goto err_no_space;
	}

	/* Map this space */
	mmio = ioremap ( ( hvm->mmio + hvm->mmio_offset ), len );
	if ( ! mmio ) {
		DBGC ( hvm, "HVM could not map MMIO space [%08lx,%08lx)\n",
		       ( hvm->mmio + hvm->mmio_offset ),
		       ( hvm->mmio + hvm->mmio_offset + len ) );
		goto err_ioremap;
	}
	mmio_phys = virt_to_phys ( mmio );

	/* Add to physical address space */
	for ( i = 0 ; i < pages ; i++ ) {
		add.domid = DOMID_SELF;
		add.idx = i;
		add.space = space;
		add.gpfn = ( ( mmio_phys / PAGE_SIZE ) + i );
		if ( ( xenrc = xenmem_add_to_physmap ( &hvm->xen, &add ) ) !=0){
			rc = -EXEN ( xenrc );
			DBGC ( hvm, "HVM could not add space %d idx %d at "
			       "[%08lx,%08lx): %s\n", space, i,
			       ( mmio_phys + ( i * PAGE_SIZE ) ),
			       ( mmio_phys + ( ( i + 1 ) * PAGE_SIZE ) ),
			       strerror ( rc ) );
			goto err_add_to_physmap;
		}
	}

	/* Update offset */
	hvm->mmio_offset += len;

	return mmio;

	i = pages;
 err_add_to_physmap:
	for ( i-- ; ( signed int ) i >= 0 ; i-- ) {
		remove.domid = DOMID_SELF;
		add.gpfn = ( ( mmio_phys / PAGE_SIZE ) + i );
		xenmem_remove_from_physmap ( &hvm->xen, &remove );
	}
	iounmap ( mmio );
 err_ioremap:
 err_no_space:
	return NULL;
}

/**
 * Unmap MMIO space
 *
 * @v hvm		HVM device
 * @v mmio		MMIO space address
 * @v pages		Number of pages
 */
static void hvm_iounmap ( struct hvm_device *hvm, void *mmio,
			  unsigned int pages ) {
	struct xen_remove_from_physmap remove;
	physaddr_t mmio_phys = virt_to_phys ( mmio );
	unsigned int i;
	int xenrc;
	int rc;

	/* Unmap this space */
	iounmap ( mmio );

	/* Remove from physical address space */
	for ( i = 0 ; i < pages ; i++ ) {
		remove.domid = DOMID_SELF;
		remove.gpfn = ( ( mmio_phys / PAGE_SIZE ) + i );
		if ( ( xenrc = xenmem_remove_from_physmap ( &hvm->xen,
							    &remove ) ) != 0 ) {
			rc = -EXEN ( xenrc );
			DBGC ( hvm, "HVM could not remove space [%08lx,%08lx): "
			       "%s\n", ( mmio_phys + ( i * PAGE_SIZE ) ),
			       ( mmio_phys + ( ( i + 1 ) * PAGE_SIZE ) ),
			       strerror ( rc ) );
			/* Nothing we can do about this */
		}
	}
}

/**
 * Map shared info page
 *
 * @v hvm		HVM device
 * @ret rc		Return status code
 */
static int hvm_map_shared_info ( struct hvm_device *hvm ) {
	physaddr_t shared_info_phys;
	int rc;

	/* Map shared info page */
	hvm->xen.shared = hvm_ioremap ( hvm, XENMAPSPACE_shared_info, 1 );
	if ( ! hvm->xen.shared ) {
		rc = -ENOMEM;
		goto err_alloc;
	}
	shared_info_phys = virt_to_phys ( hvm->xen.shared );
	DBGC2 ( hvm, "HVM shared info page at [%#08lx,%#08lx)\n",
		shared_info_phys, ( shared_info_phys + PAGE_SIZE ) );

	/* Sanity check */
	DBGC2 ( hvm, "HVM wallclock time is %d\n",
		readl ( &hvm->xen.shared->wc_sec ) );

	return 0;

	hvm_iounmap ( hvm, hvm->xen.shared, 1 );
 err_alloc:
	return rc;
}

/**
 * Unmap shared info page
 *
 * @v hvm		HVM device
 */
static void hvm_unmap_shared_info ( struct hvm_device *hvm ) {

	/* Unmap shared info page */
	hvm_iounmap ( hvm, hvm->xen.shared, 1 );
}

/**
 * Map grant table
 *
 * @v hvm		HVM device
 * @ret rc		Return status code
 */
static int hvm_map_grant ( struct hvm_device *hvm ) {
	struct gnttab_query_size size;
	struct gnttab_set_version version;
	physaddr_t grant_phys;
	size_t len;
	int xenrc;
	int rc;

	/* Get grant table size */
	size.dom = DOMID_SELF;
	if ( ( xenrc = xengrant_query_size ( &hvm->xen, &size ) ) != 0 ) {
		rc = -EXEN ( xenrc );
		DBGC ( hvm, "HVM could not get grant table size: %s\n",
		       strerror ( rc ) );
		goto err_query_size;
	}
	len = ( size.nr_frames * PAGE_SIZE );

	/* Configure to use version 2 tables */
	version.version = 2;
	if ( ( xenrc = xengrant_set_version ( &hvm->xen, &version ) ) != 0 ) {
		rc = -EXEN ( xenrc );
		DBGC ( hvm, "HVM could not set version 2 grant table: %s\n",
		       strerror ( rc ) );
		goto err_set_version;
	}
	if ( version.version != 2 ) {
		DBGC ( hvm, "HVM could not set version 2 grant table\n" );
		rc = -ENOTTY;
		goto err_set_version;
	}

	/* Map grant table */
	hvm->xen.grant.table = hvm_ioremap ( hvm, XENMAPSPACE_grant_table,
					     size.nr_frames );
	if ( ! hvm->xen.grant.table ) {
		rc = -ENODEV;
		goto err_ioremap;
	}
	grant_phys = virt_to_phys ( hvm->xen.grant.table );
	DBGC2 ( hvm, "HVM mapped grant table at [%08lx,%08lx)\n",
		grant_phys, ( grant_phys + len ) );
	hvm->xen.grant.count = ( len / sizeof ( hvm->xen.grant.table[0] ) );

	return 0;

	hvm_iounmap ( hvm, hvm->xen.grant.table, size.nr_frames );
 err_ioremap:
 err_set_version:
 err_query_size:
	return rc;
}

/**
 * Unmap grant table
 *
 * @v hvm		HVM device
 */
static void hvm_unmap_grant ( struct hvm_device *hvm ) {
	size_t len;

	/* Unmap grant table */
	len = ( hvm->xen.grant.count * sizeof ( hvm->xen.grant.table[0] ) );
	hvm_iounmap ( hvm, hvm->xen.grant.table, ( len / PAGE_SIZE ) );
}

/**
 * Map XenStore
 *
 * @v hvm		HVM device
 * @ret rc		Return status code
 */
static int hvm_map_xenstore ( struct hvm_device *hvm ) {
	uint64_t xenstore_evtchn;
	uint64_t xenstore_pfn;
	physaddr_t xenstore_phys;
	char *name;
	int xenrc;
	int rc;

	/* Get XenStore event channel */
	if ( ( xenrc = xen_hvm_get_param ( &hvm->xen, HVM_PARAM_STORE_EVTCHN,
					   &xenstore_evtchn ) ) != 0 ) {
		rc = -EXEN ( xenrc );
		DBGC ( hvm, "HVM could not get XenStore event channel: %s\n",
		       strerror ( rc ) );
		return rc;
	}
	hvm->xen.store.port = xenstore_evtchn;

	/* Get XenStore PFN */
	if ( ( xenrc = xen_hvm_get_param ( &hvm->xen, HVM_PARAM_STORE_PFN,
					   &xenstore_pfn ) ) != 0 ) {
		rc = -EXEN ( xenrc );
		DBGC ( hvm, "HVM could not get XenStore PFN: %s\n",
		       strerror ( rc ) );
		return rc;
	}
	xenstore_phys = ( xenstore_pfn * PAGE_SIZE );

	/* Map XenStore */
	hvm->xen.store.intf = ioremap ( xenstore_phys, PAGE_SIZE );
	if ( ! hvm->xen.store.intf ) {
		DBGC ( hvm, "HVM could not map XenStore at [%08lx,%08lx)\n",
		       xenstore_phys, ( xenstore_phys + PAGE_SIZE ) );
		return -ENODEV;
	}
	DBGC2 ( hvm, "HVM mapped XenStore at [%08lx,%08lx) with event port "
		"%d\n", xenstore_phys, ( xenstore_phys + PAGE_SIZE ),
		hvm->xen.store.port );

	/* Check that XenStore is working */
	if ( ( rc = xenstore_read ( &hvm->xen, &name, "name", NULL ) ) != 0 ) {
		DBGC ( hvm, "HVM could not read domain name: %s\n",
		       strerror ( rc ) );
		return rc;
	}
	DBGC2 ( hvm, "HVM running in domain \"%s\"\n", name );
	free ( name );

	return 0;
}

/**
 * Unmap XenStore
 *
 * @v hvm		HVM device
 */
static void hvm_unmap_xenstore ( struct hvm_device *hvm ) {

	/* Unmap XenStore */
	iounmap ( hvm->xen.store.intf );
}

/**
 * Probe PCI device
 *
 * @v pci		PCI device
 * @ret rc		Return status code
 */
static int hvm_probe ( struct pci_device *pci ) {
	struct hvm_device *hvm;
	int rc;

	/* Allocate and initialise structure */
	hvm = zalloc ( sizeof ( *hvm ) );
	if ( ! hvm ) {
		rc = -ENOMEM;
		goto err_alloc;
	}
	hvm->mmio = pci_bar_start ( pci, HVM_MMIO_BAR );
	hvm->mmio_len = pci_bar_size ( pci, HVM_MMIO_BAR );
	DBGC2 ( hvm, "HVM has MMIO space [%08lx,%08lx)\n",
		hvm->mmio, ( hvm->mmio + hvm->mmio_len ) );

	/* Fix up PCI device */
	adjust_pci_device ( pci );

	/* Attach to hypervisor */
	if ( ( rc = hvm_cpuid_base ( hvm ) ) != 0 )
		goto err_cpuid_base;
	if ( ( rc = hvm_map_hypercall ( hvm ) ) != 0 )
		goto err_map_hypercall;
	if ( ( rc = hvm_map_shared_info ( hvm ) ) != 0 )
		goto err_map_shared_info;
	if ( ( rc = hvm_map_grant ( hvm ) ) != 0 )
		goto err_map_grant;
	if ( ( rc = hvm_map_xenstore ( hvm ) ) != 0 )
		goto err_map_xenstore;

	/* Probe Xen devices */
	if ( ( rc = xenbus_probe ( &hvm->xen, &pci->dev ) ) != 0 ) {
		DBGC ( hvm, "HVM could not probe Xen bus: %s\n",
		       strerror ( rc ) );
		goto err_xenbus_probe;
	}

	pci_set_drvdata ( pci, hvm );
	return 0;

	xenbus_remove ( &hvm->xen, &pci->dev );
 err_xenbus_probe:
	hvm_unmap_xenstore ( hvm );
 err_map_xenstore:
	hvm_unmap_grant ( hvm );
 err_map_grant:
	hvm_unmap_shared_info ( hvm );
 err_map_shared_info:
	hvm_unmap_hypercall ( hvm );
 err_map_hypercall:
 err_cpuid_base:
	free ( hvm );
 err_alloc:
	return rc;
}

/**
 * Remove PCI device
 *
 * @v pci		PCI device
 */
static void hvm_remove ( struct pci_device *pci ) {
	struct hvm_device *hvm = pci_get_drvdata ( pci );

	xenbus_remove ( &hvm->xen, &pci->dev );
	hvm_unmap_xenstore ( hvm );
	hvm_unmap_grant ( hvm );
	hvm_unmap_shared_info ( hvm );
	hvm_unmap_hypercall ( hvm );
	free ( hvm );
}

/** PCI device IDs */
static struct pci_device_id hvm_ids[] = {
	PCI_ROM ( 0x5853, 0x0001, "hvm", "hvm", 0 ),
};

/** PCI driver */
struct pci_driver hvm_driver __pci_driver = {
	.ids = hvm_ids,
	.id_count = ( sizeof ( hvm_ids ) / sizeof ( hvm_ids[0] ) ),
	.probe = hvm_probe,
	.remove = hvm_remove,
};

/* Drag in netfront driver */
REQUIRE_OBJECT ( netfront );