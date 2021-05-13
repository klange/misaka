/**
 * @file  kernel/arch/x86_64/main.c
 * @brief Intel/AMD x86-64 (IA64/amd64) architecture-specific startup.
 *
 * Parses multiboot data, sets up GDT/IDT/TSS, initializes PML4 paging,
 * and sets up PC device drivers (PS/2, port I/O, serial).
 */
#include <kernel/types.h>
#include <kernel/multiboot.h>
#include <kernel/symboltable.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/pci.h>
#include <kernel/hashmap.h>
#include <kernel/vfs.h>
#include <kernel/mmu.h>
#include <kernel/video.h>
#include <kernel/generic.h>
#include <kernel/ramdisk.h>

#include <kernel/arch/x86_64/ports.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/cmos.h>
#include <kernel/arch/x86_64/pml.h>

#include <errno.h>

extern void arch_clock_initialize(void);

extern char end[];

extern void gdt_install(void);
extern void idt_install(void);
extern void pit_initialize(void);
extern fs_node_t * lfb_device;
extern void acpi_initialize(void);
extern void portio_initialize(void);
extern void keyboard_install(void);
extern void mouse_install(void);
extern void vmware_initialize(void);
extern void serial_initialize(void);

#define EARLY_LOG_DEVICE 0x3F8
static size_t _early_log_write(size_t size, uint8_t * buffer) {
	for (unsigned int i = 0; i < size; ++i) {
		outportb(EARLY_LOG_DEVICE, buffer[i]);
	}
	return size;
}

static void early_log_initialize(void) {
	printf_output = &_early_log_write;
}

static size_t memCount = 0;
static void multiboot_initialize(struct multiboot * mboot) {
	/* Set memCount to 1M + high mem */
	if (mboot->flags & MULTIBOOT_FLAG_MEM) {
		/* mem_upper is in kibibytes and is one mebibyte less than
		 * actual available memory, so add that back in and multiply... */
		memCount = (uintptr_t)mboot->mem_upper * 0x400 + 0x100000;
	}

	/* Check mmap if available */
#if 0
	/* The multiboot 0.6.96 spec actually says the upper_memory is at most
	 * the address of the first hole, minus 1MiB, so in theory there should
	 * not be any unavailable memory between 1MiB and mem_upper... that
	 * also technically means there might be even higher memory above that
	 * hole that we're missing... We should really be scanning the whole map
	 * to find the highest address of available memory, using that as our
	 * memory count, and then ensuring all of the holes are marked unavailable.
	 * but for now we'll just accept that there's a hole in lower memory and
	 * mem_upper is probably the total available physical RAM. That's probably
	 * good enough for 1GiB~4GiB cases...
	 */
	printf("mem_upper = %#zx\n", mboot->mem_upper);
	if (mboot->flags & MULTIBOOT_FLAG_MMAP) {
		mboot_memmap_t * mmap = (void *)(uintptr_t)mboot->mmap_addr;
		while ((uintptr_t)mmap < mboot->mmap_addr + mboot->mmap_length) {
			printf("  0x%016zx:0x%016zx %d (%s)\n", mmap->base_addr, mmap->length, mmap->type,
					mmap->type == 1 ? "available" : (mmap->type == 2 ? "reserved" : "unknown")
					);
			mmap = (mboot_memmap_t *) ((uintptr_t)mmap + mmap->size + sizeof(uint32_t));
		}
	}
#endif

	uintptr_t maxAddress = (uintptr_t)&end;
	if (mboot->flags & MULTIBOOT_FLAG_MODS) {
		mboot_mod_t * mods = (mboot_mod_t *)(uintptr_t)mboot->mods_addr;
		for (unsigned int i = 0; i < mboot->mods_count; ++i) {
			uintptr_t addr = (uintptr_t)mods[i].mod_start + mods[i].mod_end;
			if (addr > maxAddress) maxAddress = addr;
		}
	}

	/* Round the max address up a page */
	maxAddress = (maxAddress + 0x1000) & 0xFFFFffffFFFFf000;
	mmu_set_kernel_heap(maxAddress);
}

/**
 * FIXME: We don't currently use the kernel symbol table, but when modules
 *        are implemented again we need it for linking... but also we could
 *        just build the kernel with a dynamic symbol table attached?
 */
static hashmap_t * kernelSymbols = NULL;

static void symbols_install(void) {
	kernelSymbols = hashmap_create(10);
	kernel_symbol_t * k = (kernel_symbol_t *)&kernel_symbols_start;
	while ((uintptr_t)k < (uintptr_t)&kernel_symbols_end) {
		hashmap_set(kernelSymbols, k->name, (void*)k->addr);
		k = (kernel_symbol_t *)((uintptr_t)k + sizeof *k + strlen(k->name) + 1);
	}
}

/**
 * @brief Initializes the page attribute table.
 * FIXME: This seems to be assuming the lower entries are
 *        already sane - we should probably initialize all
 *        of the entries ourselves.
 */
static void pat_initialize(void) {
	asm volatile (
		"mov $0x277, %%ecx\n" /* IA32_MSR_PAT */
		"rdmsr\n"
		"or $0x1000000, %%edx\n" /* set bit 56 */
		"and $0xf9ffffff, %%edx\n" /* unset bits 57, 58 */
		"wrmsr\n"
		: : : "ecx", "edx", "eax"
	);
}

/**
 * @brief Turns on the floating-point unit.
 *
 * Enables a few bits so we can get SSE.
 *
 * We don't do any fancy lazy FPU reload as x86-64 assumes a wide
 * variety of FPU-provided registers are available so most userspace
 * code will be messing with the FPU anyway and we'd probably just
 * waste time with all the interrupts turning it off and on...
 */
static void fpu_initialize(void) {
	asm volatile (
		"clts\n"
		"mov %%cr0, %%rax\n"
		"and $0xFFFD, %%ax\n"
		"or $0x10, %%ax\n"
		"mov %%rax, %%cr0\n"
		"fninit\n"
		"mov %%cr0, %%rax\n"
		"and $0xfffb, %%ax\n"
		"or  $0x0002, %%ax\n"
		"mov %%rax, %%cr0\n"
		"mov %%cr4, %%rax\n"
		"or $0x600, %%rax\n"
		"mov %%rax, %%cr4\n"
		"push $0x1F80\n"
		"ldmxcsr (%%rsp)\n"
		"addq $8, %%rsp\n"
	: : : "rax");
}

static struct multiboot * mboot_struct = NULL;

/**
 * x86-64: The kernel commandline is retrieved from the multiboot struct.
 */
const char * arch_get_cmdline(void) {
	return (char*)((0xFFFFFFFF00000000UL) | mboot_struct->cmdline);
}

/**
 * x86-64: The bootloader name is retrieved from the multiboot struct.
 */
const char * arch_get_loader(void) {
	if (mboot_struct->flags & MULTIBOOT_FLAG_LOADER) {
		return (char*)((0xFFFFFFFF00000000UL) | mboot_struct->boot_loader_name);
	} else {
		return "(unknown)";
	}
}

/**
 * @brief x86-64 multiboot C entrypoint.
 *
 * Called by the x86-64 longmode bootstrap.
 */
int kmain(struct multiboot * mboot, uint32_t mboot_mag, void* esp) {
	/* The debug log is over /dev/ttyS0, but skips the PTY interface; it's available
	 * as soon as we can call printf(), which is as soon as we get to long mode. */
	early_log_initialize();

	/* Time the TSC and get the initial boot time from the RTC. */
	arch_clock_initialize();

	/* Parse multiboot data so we can get memory map, modules, command line, etc. */
	mboot_struct = mboot;
	multiboot_initialize(mboot);

	/* memCount comes from multiboot data */
	mmu_init(memCount);

	/* With the MMU initialized, set up things required for the scheduler. */
	pat_initialize();
	symbols_install();
	//acpi_initialize();
	gdt_install();
	idt_install();
	fpu_initialize();

	/* Early generic stuff */
	generic_startup();

	/* Scheduler is running, so we can set up drivers. */
	framebuffer_initialize();
	vfs_mount("/dev/fb0", lfb_device);

	/* Mount ramdisk (TODO: Should we be gzipping this and decompressing here?) */
	mboot_mod_t * mods = (mboot_mod_t *)(uintptr_t)mboot->mods_addr;
	ramdisk_mount(mods[0].mod_start, mods[0].mod_end - mods[0].mod_start);

	/* We set up the pit and interrupt stuff pretty late, after the scheduler is ready. */
	pit_initialize();
	keyboard_install();
	mouse_install();
	serial_initialize();
	portio_initialize();

	/* Special drivers should probably be modules... */
	vmware_initialize();

	/* Yield the generic main, which starts /bin/init */
	return generic_main();
}