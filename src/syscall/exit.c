/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2013 STMicroelectronics
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
 * 02110-1301 USA.
 */

#include <errno.h>       /* errno(3), E* */
#include <sys/utsname.h> /* struct utsname, */
#include <linux/net.h>   /* SYS_*, */
#include <string.h>      /* strlen(3), */

#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "syscall/socket.h"
#include "tracee/tracee.h"
#include "tracee/reg.h"
#include "tracee/mem.h"
#include "tracee/abi.h"
#include "path/path.h"
#include "ptrace/ptrace.h"
#include "ptrace/wait.h"
#include "extension/extension.h"

/**
 * Translate the output arguments of the current @tracee's syscall in
 * the @tracee->pid process area. This function sets the result of
 * this syscall to @tracee->status if an error occured previously
 * during the translation, that is, if @tracee->status is less than 0.
 */
void translate_syscall_exit(Tracee *tracee)
{
	word_t syscall_number;
	word_t syscall_result;
	int status;

	status = notify_extensions(tracee, SYSCALL_EXIT_START, 0, 0);
	if (status < 0) {
		poke_reg(tracee, SYSARG_RESULT, (word_t) status);
		goto end;
	}
	if (status > 0)
		return;

	/* Set the tracee's errno if an error occured previously during
	 * the translation. */
	if (tracee->status < 0) {
		poke_reg(tracee, SYSARG_RESULT, (word_t) tracee->status);
		goto end;
	}

	/* Translate output arguments:
	 * - break: update the syscall result register with "status"
	 * - goto end: nothing else to do.
	 */
	syscall_number = get_sysnum(tracee, ORIGINAL);
	syscall_result = peek_reg(tracee, CURRENT, SYSARG_RESULT);
	switch (syscall_number) {
	case PR_getcwd: {
		size_t new_size;
		size_t size;
		word_t output;

		/* Error reported by the kernel.  */
		if ((int) syscall_result < 0)
			goto end;

		output = peek_reg(tracee, ORIGINAL, SYSARG_1);

		size = (size_t) peek_reg(tracee, ORIGINAL, SYSARG_2);
		if (size == 0) {
			status = -EINVAL;
			break;
		}

		new_size = strlen(tracee->fs->cwd) + 1;
		if (size < new_size) {
			status = -ERANGE;
			break;
		}

		/* Overwrite the path.  */
		status = write_data(tracee, output, tracee->fs->cwd, new_size);
		if (status < 0)
			break;

		/* The value of "status" is used to update the returned value
		 * in translate_syscall_exit().  */
		status = new_size;
		break;
	}

	case PR_accept:
	case PR_accept4:
	case PR_getsockname:
	case PR_getpeername: {
		word_t sock_addr;
		word_t size_addr;
		word_t max_size;

		/* Error reported by the kernel.  */
		if ((int) syscall_result < 0)
			goto end;

		sock_addr = peek_reg(tracee, ORIGINAL, SYSARG_2);
		size_addr = peek_reg(tracee, MODIFIED, SYSARG_3);
		max_size  = peek_reg(tracee, MODIFIED, SYSARG_6);

		status = translate_socketcall_exit(tracee, sock_addr, size_addr, max_size);
		if (status < 0)
			break;

		/* Don't overwrite the syscall result.  */
		goto end;
	}

#define SYSARG_ADDR(n) (args_addr + ((n) - 1) * sizeof_word(tracee))

#define POKE_MEM(addr, value) poke_mem(tracee, addr, value);	\
	if (errno != 0) {					\
		status = -errno;				\
		break;						\
	}

#define PEEK_MEM(addr) peek_mem(tracee, addr);			\
	if (errno != 0) {					\
		status = -errno;				\
		break;						\
	}

	case PR_socketcall: {
		word_t args_addr;
		word_t sock_addr;
		word_t size_addr;
		word_t max_size;

		args_addr = peek_reg(tracee, ORIGINAL, SYSARG_2);

		switch (peek_reg(tracee, ORIGINAL, SYSARG_1)) {
		case SYS_ACCEPT:
		case SYS_ACCEPT4:
		case SYS_GETSOCKNAME:
		case SYS_GETPEERNAME:
			/* Handle these cases below.  */
			status = 1;
			break;

		case SYS_BIND:
		case SYS_CONNECT:
			/* Restore the initial parameters: this memory was
			 * overwritten at the enter stage.  Remember: POKE_MEM
			 * puts -errno in status and breaks if an error
			 * occured.  */
			POKE_MEM(SYSARG_ADDR(2), peek_reg(tracee, MODIFIED, SYSARG_5));
			POKE_MEM(SYSARG_ADDR(3), peek_reg(tracee, MODIFIED, SYSARG_6));

			status = 0;
			break;

		default:
			status = 0;
			break;
		}

		/* Error reported by the kernel or there's nothing else to do.  */
		if ((int) syscall_result < 0 || status == 0)
			goto end;

		/* An error occured in SYS_BIND or SYS_CONNECT.  */
		if (status < 0)
			break;

		/* Remember: PEEK_MEM puts -errno in status and breaks if an
		 * error occured.  */
		sock_addr = PEEK_MEM(SYSARG_ADDR(2));
		size_addr = PEEK_MEM(SYSARG_ADDR(3));
		max_size  = peek_reg(tracee, MODIFIED, SYSARG_6);

		status = translate_socketcall_exit(tracee, sock_addr, size_addr, max_size);
		if (status < 0)
			break;

		/* Don't overwrite the syscall result.  */
		goto end;
	}

#undef SYSARG_ADDR
#undef PEEK_MEM
#undef POKE_MEM

	case PR_fchdir:
	case PR_chdir:
		/* These syscalls are fully emulated, see enter.c for details
		 * (like errors).  */
		status = 0;
		break;

	case PR_readlink:
	case PR_readlinkat: {
		char referee[PATH_MAX];
		char referer[PATH_MAX];
		size_t old_size;
		size_t new_size;
		size_t max_size;
		word_t input;
		word_t output;

		/* Error reported by the kernel.  */
		if ((int) syscall_result < 0)
			goto end;

		old_size = syscall_result;

		if (syscall_number == PR_readlink) {
			output   = peek_reg(tracee, ORIGINAL, SYSARG_2);
			max_size = peek_reg(tracee, ORIGINAL, SYSARG_3);
			input    = peek_reg(tracee, MODIFIED, SYSARG_1);
		}
		else {
			output   = peek_reg(tracee, ORIGINAL,  SYSARG_3);
			max_size = peek_reg(tracee, ORIGINAL, SYSARG_4);
			input    = peek_reg(tracee, MODIFIED, SYSARG_2);
		}

		if (max_size > PATH_MAX)
			max_size = PATH_MAX;

		if (max_size == 0) {
			status = -EINVAL;
			break;
		}

		/* The kernel does NOT put the NULL terminating byte for
		 * readlink(2).  */
		status = read_data(tracee, referee, output, old_size);
		if (status < 0)
			break;
		referee[old_size] = '\0';

		/* Not optimal but safe (path is fully translated).  */
		status = read_string(tracee, referer, input, PATH_MAX);
		if (status < 0)
			break;

		if (status >= PATH_MAX) {
			status = -ENAMETOOLONG;
			break;
		}

		status = detranslate_path(tracee, referee, referer);
		if (status < 0)
			break;

		/* The original path doesn't require any transformation, i.e
		 * it is a symetric binding.  */
		if (status == 0)
			goto end;

		/* Overwrite the path.  Note: the output buffer might be
		 * initialized with zeros but it was updated with the kernel
		 * result, and then with the detranslated result.  This later
		 * might be shorter than the former, so it's safier to add a
		 * NULL terminating byte when possible.  This problem was
		 * exposed by IDA Demo 6.3.  */
		if ((size_t) status < max_size) {
			new_size = status - 1;
			status = write_data(tracee, output, referee, status);
		}
		else {
			new_size = max_size;
			status = write_data(tracee, output, referee, max_size);
		}
		if (status < 0)
			break;

		/* The value of "status" is used to update the returned value
		 * in translate_syscall_exit().  */
		status = new_size;
		break;
	}

#if defined(ARCH_X86_64)
	case PR_uname: {
		struct utsname utsname;
		word_t address;
		size_t size;

		if (get_abi(tracee) != ABI_2)
			goto end;

		/* Error reported by the kernel.  */
		if ((int) syscall_result < 0)
			goto end;

		address = peek_reg(tracee, ORIGINAL, SYSARG_1);

		status = read_data(tracee, &utsname, address, sizeof(utsname));
		if (status < 0)
			break;

		/* Some 32-bit programs like package managers can be
		 * confused when the kernel reports "x86_64".  */
		size = sizeof(utsname.machine);
		strncpy(utsname.machine, "i686", size);
		utsname.machine[size - 1] = '\0';

		status = write_data(tracee, address, &utsname, sizeof(utsname));
		if (status < 0)
			break;

		status = 0;
		break;
	}
#endif

	case PR_execve:
		if ((int) syscall_result >= 0) {
	case PR_rt_sigreturn:
	case PR_sigreturn:
			tracee->restore_original_regs = false;
		}
		goto end;

	case PR_ptrace:
		status = translate_ptrace_exit(tracee);
		break;

	case PR_wait4:
	case PR_waitpid:
		if (tracee->as_ptracer.waits_in != WAITS_IN_PROOT)
			goto end;

		status = translate_wait_exit(tracee);
		break;

	default:
		goto end;
	}


	/* "status" was updated in syscall/exit.c.  */
	poke_reg(tracee, SYSARG_RESULT, (word_t) status);

end:
	status = notify_extensions(tracee, SYSCALL_EXIT_END, 0, 0);
	if (status < 0)
		poke_reg(tracee, SYSARG_RESULT, (word_t) status);

	/* Reset the tracee's status. */
	tracee->status = 0;
}
