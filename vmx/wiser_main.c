#include "wiser.h"
#include <linux/mm.h>
#include <linux/proc_fs.h>	// for create proc entry
#include <linux/fs.h>		// struct file ops
#include <linux/mm.h>		// for remap_pfn_range
#include <linux/mutex.h>	// for mutexs
#include <asm/io.h>		// for virt_to_phys()
#include <asm/uaccess.h>		// for copy_from_user()
#include <linux/miscdevice.h>		// for copy_from_user()
//#include "machine.h"		// for VMCS fields
#include <linux/slab.h>		// for kmalloc()

#define N_ARENAS 11 		// number of 64 KB mem allocations
#define ARENA_LENGTH (64<<10)	// 65536 - size of each allocated mem area
#define MSR_VMX_CAPS 0x480	// index of VMX Capabilities MSRs
#define LEGACY_REACH 0x110000	// end of real addressible mem - 1114112
// This 64KB above the 1 MB boundary

#define PAGE_DIR_OFFSET	0x2000
#define PAGE_TBL_OFFSET	0x3000

#define IDT_KERN_OFFSET	0x4000 // 16384
#define GDT_KERN_OFFSET	0x4800 // 18432 - (2k diff)
#define LDT_KERN_OFFSET	0x4A00 // 18944 - (512 bytes diff

#define TSS_KERN_OFFSET	0x4C00 // 19546 - (512 bytes diff)
#define TOS_KERN_OFFSET	0x8000 // 32768
#define MSR_KERN_OFFSET	0x8000

#define __SELECTOR_TASK	0x0008
#define __SELECTOR_LDTR	0x0004
#define __SELECTOR_CODE	0x0004
#define __SELECTOR_DATA	0x000C
#define __SELECTOR_VRAM	0x0014
#define __SELECTOR_FLAT	0x001C

char modname[] = "wiser";
int my_major=108;
char cpu_oem[16];
int cpu_features;
void *kmem[N_ARENAS];

unsigned long msr0x480[11];
unsigned long msr_efer;
unsigned long vmxon_region;
unsigned long guest_region;
unsigned long pgdir_region;
unsigned long pgtbl_region;
unsigned long g_IDT_region;
unsigned long g_GDT_region;
unsigned long g_LDT_region;
unsigned long g_TSS_region;
unsigned long g_TOS_region;
unsigned long g_MSR_region;
unsigned long cr0, cr4;

DEFINE_MUTEX(my_mutex);

typedef struct vm_t {
	uint32_t vmcs_num_bytes;
	int	vmxSupport;
	int	eptSupport;
	unsigned long cr0, cr4;

} vmStruct;
vmStruct vm;

long wiser_dev_ioctl(struct file *file, unsigned int ioctl, unsigned long arg) {
return 1;
}
int wiser_dev_mmap(struct file *file, struct vm_area_struct *vma ){
return 1;
}

struct file_operations wiser_dev_ops = {
	.unlocked_ioctl = wiser_dev_ioctl,
	.compat_ioctl = wiser_dev_ioctl,
	.mmap = wiser_dev_mmap,
};
struct miscdevice wiser_dev = {
	MISC_DYNAMIC_MINOR,
	"wiser",
	&wiser_dev_ops,
};

int checkProcessor() {
	u32 low, hi;

	getProcCpuid();
	getCrRegs();
	getMSR(IA32_VMX_BASIC,  &low, &hi);
	vm.vmcs_num_bytes  =  hi & 0xfff; // Bits 44:32
	printk("vmcs_num_bytes = 0x%x\n", vm.vmcs_num_bytes);

	//verify processor supports Intel Virt Tech
	asm("xor %%eax, %%eax		\n"\
	    "cpuid			\n"\
	    "mov %%ebx, cpu_oem+0	\n"\
	    "mov %%edx, cpu_oem+4	\n"\
	    "mov %%ecx, cpu_oem+8	\n"\
	    :::"ax", "bx","cx","dx");
	printk("Prosessor is %s\n", cpu_oem);
	// check if proc has VMX support
	vm.vmxSupport = vmxCheckSupport(1);
	if (vm.vmxSupport ==1)
		printk("VMX supported by chipset\n");
	else
		printk("VMX not supported by chipset\n");
	
	// check if proc has EPT support
	vm.eptSupport = vmxCheckSupportEPT();
	if (vm.eptSupport ==1)
		printk("EPT supported by chipset\n");
	else
		printk("EPT not supported by chipset\n");

	// Save 32bit cr0, cr4 in our VM struct
	asm( " mov %%cr0, %%rax \n mov %%rax, cr0 " ::: "ax" );
	asm( " mov %%cr4, %%rax \n mov %%rax, cr4 " ::: "ax" );
	vm.cr0 = cr0;
	vm.cr4 = cr4;	
	printk("cr0 = 0x%016lx\n", vm.cr0);
	printk("cr4 = 0x%016lx\n", vm.cr4);

	// Get EFER MSR
	asm( "mov %0, %%ecx		\n"\
		 "rdmsr				\n"\
		 "mov %%eax, msr_efer+0 \n"\
		 "mov %%edx, msr_efer+4 \n"\
		::"i" (MSR_EFER): "ax", "cx", "dx");
	printk("EFER MSR = 0x%016lX\n", msr_efer);

	return 0;
}

void assignAddresses() {
	vmxon_region = virt_to_phys(kmem[10] + 0x000);
	guest_region = virt_to_phys(kmem[10] + 0x1000);

	pgdir_region = virt_to_phys(kmem[10] + PAGE_DIR_OFFSET);
	pgtbl_region = virt_to_phys(kmem[10] + PAGE_TBL_OFFSET);

	g_IDT_region = virt_to_phys(kmem[10] + IDT_KERN_OFFSET);
	g_GDT_region = virt_to_phys(kmem[10] + GDT_KERN_OFFSET);
	g_LDT_region = virt_to_phys(kmem[10] + LDT_KERN_OFFSET);

	g_TSS_region = virt_to_phys(kmem[10] + TSS_KERN_OFFSET);
	g_TOS_region = virt_to_phys(kmem[10] + TOS_KERN_OFFSET);
	g_MSR_region = virt_to_phys(kmem[10] + MSR_KERN_OFFSET);
}

int wiser_main() {
	u32 i, j, r;

	checkProcessor();

	// Create /dev/wiser
	// crw------- 1 root root 10, 57 Feb  4 10:51 /dev/wiser
	wiser_dev_ops.owner = THIS_MODULE;
	r = misc_register(&wiser_dev);
	if(r) {
		printk(KERN_ERR "Wiser: misc dev register failed\n");
		return -1;
	}
	// Check if /dev/[modname] has been created
	printk("Ensure that /dev/wiser has been created\n");

	// allocate page-aligned blocks of non-pageable kernel mem
	for (i=0;i<N_ARENAS; i++) {
		kmem[i] = kmalloc(ARENA_LENGTH, GFP_KERNEL);
		if (kmem[i] == NULL) {
			for(j=0;j<i;j++) kfree(kmem[j]);
			return -ENOMEM;
		} else 
			memset(kmem[i], 0x00, ARENA_LENGTH);
	}

	// Assign usages to mem areas
	assignAddresses();

	// enable VM extensions (bit 13 in CR4)
	setCr4Vmxe(NULL);
	smp_call_function(setCr4Vmxe, NULL, 1);

	return 0;
}

int wiser_exit() {
	misc_deregister(&wiser_dev);
	return 1;
}
