#include <embox/unit.h>

#include <xen_memory.h>
#include <kernel/printk.h>

#include <xen/memory.h>
#include <xen_hypercall-x86_32.h>
#include <string.h>
#include <assert.h>

#define	ENOMEM		12	/* Out of memory */

#include <xen/xen.h>
#include <xen/version.h>
#include <xen/features.h>

//phymem reuse
#include <stdint.h>
#include <sys/mman.h>
#include <mem/page.h>
#include <grant_table.h>

EMBOX_UNIT_INIT(xen_memory_init);

struct page_allocator *__xen_mem_allocator;

static int memory_init(char *const xen_mem_alloc_start, char *const xen_mem_alloc_end) {
	const size_t mem_len = xen_mem_alloc_end - xen_mem_alloc_start;
	void *va;

	printk("start=%p end=%p size=%zu\n", xen_mem_alloc_start, xen_mem_alloc_end, mem_len);

	va = mmap_device_memory(xen_mem_alloc_start,
			mem_len,
			PROT_WRITE | PROT_READ,
			MAP_FIXED,
			(uint64_t)((uintptr_t) xen_mem_alloc_start));

	if (va) {
		__xen_mem_allocator = page_allocator_init(xen_mem_alloc_start, mem_len, PAGE_SIZE());
	}
    
	return xen_mem_alloc_start == va ? 0 : -EIO;
}

void *xen_mem_alloc(size_t page_number) {
	return page_alloc(__xen_mem_allocator, page_number);
}

void xen_mem_free(void *page, size_t page_number) {
	page_free(__xen_mem_allocator, page, page_number);
}

struct page_allocator *xen_allocator(void) {
	return __xen_mem_allocator;
}
// end of phymem


#ifdef XEN_MEM_RESERVATION //del or replace this
extern char memory_pages[600][PAGE_SIZE()];
static int alloc_page_counter = 0;
//alloc page using xen balloon
int alloc_page(unsigned long *va, unsigned long *mfn) {
	printk("alloc page #%d\n", alloc_page_counter);
	if(alloc_page_counter == 600)
	{
		return -ENOMEM;
	}
//ask for mem (for rings txs rxs)
    struct xen_memory_reservation reservation;
    reservation.nr_extents = 1;
    reservation.extent_order = 3;
    reservation.address_bits= 0;
    reservation.domid = DOMID_SELF;
    unsigned long frame_list[1];
    set_xen_guest_handle(reservation.extent_start, frame_list);
   	int rc;
    rc = HYPERVISOR_memory_op(XENMEM_increase_reservation, &reservation);
    printk("XENMEM_increase_reservation=%d\n", rc);
    printk("frame_list=%lu\n", frame_list[0]);
	
//map it
	printk("addr:%p\n", &memory_pages[alloc_page_counter]);
	rc = HYPERVISOR_update_va_mapping((unsigned long) &memory_pages[alloc_page_counter],
			__pte((frame_list[0]<< PAGE_SHIFT) | 7),
			UVMF_INVLPG);
	printk("HYPERVISOR_update_va_mapping:%d\n", rc);

	*mfn = frame_list[0];
	*va = (unsigned long)&memory_pages[alloc_page_counter];
	alloc_page_counter++;
//done!
	return 0;
}
#endif

extern start_info_t xen_start_info;


typedef uint64_t pgentry_t;

unsigned long *phys_to_machine_mapping;
pgentry_t *pt_base;
unsigned long mfn_zero;
static unsigned long first_free_pfn;
static unsigned long last_free_pfn;
unsigned long nr_max_pages;

void get_max_pages(void) {
    long ret;
    domid_t domid = DOMID_SELF;

    ret = HYPERVISOR_memory_op(XENMEM_maximum_reservation, &domid);
    if ( ret < 0 )
    {
        //printk("Could not get maximum pfn\n");
        return;
    }

    nr_max_pages = ret;
    
    //printk("Maximum memory size: %ld pages\n", nr_max_pages);
}

void do_exit(void) {
    assert(NULL);
}
/*
 * Make pt_pfn a new 'level' page table frame and hook it into the page
 * table at offset in previous level MFN (pref_l_mfn). pt_pfn is a guest
 * PFN.
 */
static pgentry_t pt_prot[PAGETABLE_LEVELS] = {
    L1_PROT,
    L2_PROT,
    L3_PROT
};

static void new_pt_frame(unsigned long *pt_pfn, unsigned long prev_l_mfn, 
                         unsigned long offset, unsigned long level) {
    pgentry_t *tab;
    unsigned long pt_page = (unsigned long)pfn_to_virt(*pt_pfn); 
    mmu_update_t mmu_updates[1];
    int rc;

    /*
    printk("Allocating new L%ld pt frame for pfn=%lx, "
          "prev_l_mfn=%lx, offset=%lx\n", 
          level, *pt_pfn, prev_l_mfn, offset);
*/
    /* We need to clear the page, otherwise we might fail to map it
       as a page table page */
    memset((void*) pt_page, 0, PAGE_SIZE());  


    if(!(level >= 1 && level <= PAGETABLE_LEVELS))
    {
        do_exit();
        //printk("CRITICAL ERROR: FIX me!!!!\n");
    }

    /* Make PFN a page table page */
    tab = pt_base;

    tab = pte_to_virt(tab[l3_table_offset(pt_page)]);

    mmu_updates[0].ptr = (tab[l2_table_offset(pt_page)] & PAGE_MASK) + 
        sizeof(pgentry_t) * l1_table_offset(pt_page);
    mmu_updates[0].val = (pgentry_t)pfn_to_mfn(*pt_pfn) << PAGE_SHIFT | 
        (pt_prot[level - 1] & ~_PAGE_RW);
    
    if ( (rc = HYPERVISOR_mmu_update(mmu_updates, 1, NULL, DOMID_SELF)) < 0 )
    {
        /*
        printk("ERROR: PTE for new page table page could not be updated\n");
        printk("       mmu_update failed with rc=%d\n", rc);
        */
        do_exit();
    }

    /* Hook the new page table page into the hierarchy */
    mmu_updates[0].ptr =
        ((pgentry_t)prev_l_mfn << PAGE_SHIFT) + sizeof(pgentry_t) * offset;
    mmu_updates[0].val = (pgentry_t)pfn_to_mfn(*pt_pfn) << PAGE_SHIFT |
        pt_prot[level];

    if ( (rc = HYPERVISOR_mmu_update(mmu_updates, 1, NULL, DOMID_SELF)) < 0 ) 
    {
        //printk("ERROR: mmu_update failed with rc=%d\n", rc);
        do_exit();
    }

    *pt_pfn += 1;
}
/*
 * Build the initial pagetable.
 */
static void build_pagetable(unsigned long *start_pfn, unsigned long *max_pfn) {
    unsigned long start_address, end_address;
    unsigned long pfn_to_map, pt_pfn = *start_pfn;
    pgentry_t *tab = pt_base, page;
    unsigned long pt_mfn = pfn_to_mfn(virt_to_pfn(pt_base));
    unsigned long offset;
    static mmu_update_t mmu_updates[L1_PAGETABLE_ENTRIES + 1];
    int count = 0;
    int rc;

    /* Be conservative: even if we know there will be more pages already
       mapped, start the loop at the very beginning. */
    pfn_to_map = *start_pfn;

    if ( *max_pfn >= virt_to_pfn(HYPERVISOR_VIRT_START) )
    {
        /*printk("WARNING: Mini-OS trying to use Xen virtual space. "
               "Truncating memory from %luMB to ",
               ((unsigned long)pfn_to_virt(*max_pfn) - VIRT_START)>>20);
        */
        *max_pfn = virt_to_pfn(HYPERVISOR_VIRT_START - PAGE_SIZE());
        /*
        printk("%luMB\n",
               ((unsigned long)pfn_to_virt(*max_pfn) - VIRT_START)>>20);
        */
    }


    start_address = (unsigned long)pfn_to_virt(pfn_to_map);
    end_address = (unsigned long)pfn_to_virt(*max_pfn);

    /* We worked out the virtual memory range to map, now mapping loop */
    //printk("Mapping memory range 0x%lx - 0x%lx\n", start_address, end_address);

    while ( start_address < end_address )
    {
        tab = pt_base;
        pt_mfn = pfn_to_mfn(virt_to_pfn(pt_base));


        offset = l3_table_offset(start_address);
        /* Need new L2 pt frame */
        if ( !(tab[offset] & _PAGE_PRESENT) )
            new_pt_frame(&pt_pfn, pt_mfn, offset, L2_FRAME);

        page = tab[offset];
        pt_mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(pt_mfn) << PAGE_SHIFT);
        offset = l2_table_offset(start_address);        

        /* Need new L1 pt frame */
        if ( !(tab[offset] & _PAGE_PRESENT) )
            new_pt_frame(&pt_pfn, pt_mfn, offset, L1_FRAME);

        page = tab[offset];
        pt_mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(pt_mfn) << PAGE_SHIFT);
        offset = l1_table_offset(start_address);

        if ( !(tab[offset] & _PAGE_PRESENT) )
        {
            mmu_updates[count].ptr =
                ((pgentry_t)pt_mfn << PAGE_SHIFT) + sizeof(pgentry_t) * offset;
            mmu_updates[count].val =
                (pgentry_t)pfn_to_mfn(pfn_to_map) << PAGE_SHIFT | L1_PROT;
            count++;
        }
        pfn_to_map++;
        if ( count == L1_PAGETABLE_ENTRIES ||
             (count && pfn_to_map == *max_pfn) )
        {
            rc = HYPERVISOR_mmu_update(mmu_updates, count, NULL, DOMID_SELF);
            if ( rc < 0 )
            {
                /*
                printk("ERROR: build_pagetable(): PTE could not be updated\n");
                printk("       mmu_update failed with rc=%d\n", rc);
                */
                do_exit();
            }
            count = 0;
        }
        start_address += PAGE_SIZE();
    }

    *start_pfn = pt_pfn;
}
#if 0
/*
 * Clear some of the bootstrap memory
 */
static void clear_bootstrap(void)
{
    pte_t nullpte = { };
    int rc;

    /* Use first page as the CoW zero page */
    memset((void*)VIRT_START, 0, PAGE_SIZE());
    mfn_zero = virt_to_mfn(VIRT_START);

    if ( (rc = HYPERVISOR_update_va_mapping(0, nullpte, UVMF_INVLPG)) )
        printk("Unable to unmap NULL page. rc=%d\n", rc);
}
#endif

#define MAX_MEM_SIZE            0x3f000000ULL
void arch_init_mm(unsigned long* start_pfn_p, unsigned long* max_pfn_p) {
    unsigned long start_pfn, max_pfn;

    /* First page follows page table pages. */
    start_pfn = first_free_pfn;
    max_pfn = last_free_pfn;

    if ( max_pfn >= MAX_MEM_SIZE / PAGE_SIZE() )
    {
        max_pfn = MAX_MEM_SIZE / PAGE_SIZE() - 1;
    }

    //printk("  start_pfn: %lx\n", start_pfn);
    //printk("    max_pfn: %lx\n", max_pfn);

    build_pagetable(&start_pfn, &max_pfn);
    //clear_bootstrap();
    //set_readonly(&_text, &_erodata); //TODO: remove _text!!!!

    *start_pfn_p = start_pfn;
    *max_pfn_p = max_pfn;
}

int is_auto_translated_physmap(void) {
    unsigned char xen_features[32]; //__read_mostly
	struct xen_feature_info fi;

	int j;
    fi.submap_idx = 0;
    if (HYPERVISOR_xen_version(XENVER_get_features, &fi) < 0) 
    {
        printk("error while feature getting!");
    }
    for (j = 0; j < 32; j++)
    {
        xen_features[j] = !!(fi.submap & 1<<j);
    }

    return xen_features[XENFEAT_auto_translated_physmap];
}

static int xen_memory_init(void) {
    assert(is_auto_translated_physmap() == 0);

    phys_to_machine_mapping = (unsigned long *)xen_start_info.mfn_list;
    pt_base = (pgentry_t *)xen_start_info.pt_base;
    first_free_pfn = PFN_UP(to_phys(pt_base)) + xen_start_info.nr_pt_frames;
    last_free_pfn = xen_start_info.nr_pages;

    unsigned long start_pfn, max_pfn;
    get_max_pages();

    arch_init_mm(&start_pfn, &max_pfn);
    printk("start_pfn=%lu, max_pfn=%lu\n", start_pfn, max_pfn);
    
    memory_init(pfn_to_virt(start_pfn), pfn_to_virt(max_pfn));

    init_grant_table(NR_GRANT_FRAMES);

    return 0;
}
