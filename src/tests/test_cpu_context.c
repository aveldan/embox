/**
 * \file test_cpu_context.c
 *
 *  \date Jan 26, 2009
 *  \author Anton Bondarev
 */
#include "types.h"
#include "common.h"
#include "cpu_context.h"
#include "test_cpu_context.h"

//int ret_addr;
//int cpu_context_addr;
CPU_CONTEXT test_cpu_context_buff;

static int flag_restored_context = 0;


/**
 * this test call trap 80 (soft trap)
 * in this trap function save_proc_context takes place
 * this function save all procesor context to pointed memory
 * then call function restore_proc_context
 * @return 0 if success
 */
int test_cpu_context ()
{
	asm ("ta 0");
	printf ("save context done\n");

	if (!flag_restored_context)
	{
		flag_restored_context ++;
		printf ("restore context start\n");
		restore_proc_context(&test_cpu_context_buff);
//		asm ("ta 8");
		printf ("restore context failt\n");
	}
	else
	{
		printf ("restore context done\n");
	}
	return 0;
}
