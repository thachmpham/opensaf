/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2008 The OpenSAF Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. This file and program are licensed
 * under the GNU Lesser General Public License Version 2.1, February 1999.
 * The complete license can be accessed from the following location:
 * http://opensource.org/licenses/lgpl-license.php
 * See the Copying file included with the OpenSAF distribution for full
 * licensing terms.
 *
 * Author(s): Emerson Network Power
 *
 */

/*****************************************************************************

  DESCRIPTION:

  This module contains ?

 ******************************************************************************
 */

#include "osaf/configmake.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include "base/ncs_main_papi.h"
#include "base/ncsgl_defs.h"
#include "base/ncs_osprm.h"

#include "base/ncs_svd.h"
#include "base/ncssysf_def.h"
#include "base/ncssysf_tmr.h"
#include "base/ncssysf_lck.h"
#include "base/ncssysfpool.h"
#include "base/sysf_exc_scr.h"
#include "base/usrbuf.h"
#include "base/osaf_extended_name.h"

/**
 *  File descriptor pointing to /proc/sysrq-trigger. If a process wants to be
 *  able to reboot the local node without running as root, this file is opened
 *  before dropping root privileges. The reason is that only root can open this
 *  file. Another advantage of keeping this file open is that we can reboot
 *  also in the error scenario when open() would fail because either the
 *  process itself or the kernel has run out of free file descriptors. Note
 *  that it is also a danger in keeping this file open, if a bug happens to
 *  write something to the wrong file descriptor.
 */
static int sysrq_trigger_fd = -1;

/*****************************************************************************

  PROCEDURE NAME:    leap_env_init

  DESCRIPTION:

   Initialize LEAP services

  ARGUMENTS:

  RETURNS:

  code

  NOTES:

*****************************************************************************/

static uint32_t leap_env_init_count = 0;

uint32_t leap_env_init(void)
{
	if (leap_env_init_count++ != 0) {
		return NCSCC_RC_SUCCESS;
	}

	TRACE("INITIALIZING LEAP ENVIRONMENT");

	osaf_extended_name_init();

	ncs_os_atomic_init();

#if (NCSL_ENV_INIT_TMR == 1)
	/* initialize LEAP Timer Service */
	if (!sysfTmrCreate()) {
		printf("\nleap_env_init: FAILED to initialize Timer Service\n");
		return NCSCC_RC_FAILURE;
	}
#endif /* #if (NCSL_ENV_INIT_TMR == 1) */

#if (NCSL_ENV_INIT_HM == 1)
	/* initialize Handle Manager */
	if (ncshm_init() != NCSCC_RC_SUCCESS) {
		printf(
		    "\nleap_env_init: FAILED to initialize Handle Manager\n");

#if (NCSL_ENV_INIT_TMR == 1)
		(void)sysfTmrDestroy();
#endif
		return NCSCC_RC_FAILURE;
	}
#endif /* #if (NCSL_ENV_INIT_HM == 1) */

	/* Initialize script execution control block */
	if (NCSCC_RC_SUCCESS != init_exec_mod_cb()) {
		/* Log error */
		printf(
		    "\nleap_env_init: FAILED to initialize Execute Module CB \n");

		return m_LEAP_DBG_SINK(NCSCC_RC_FAILURE);
	}
	TRACE("DONE INITIALIZING LEAP ENVIRONMENT");

	return NCSCC_RC_SUCCESS;
}

/*****************************************************************************

  PROCEDURE NAME:    leap_env_destroy

  DESCRIPTION:

   Destroy LEAP services

  ARGUMENTS:

  RETURNS:

  code

  NOTES:

*****************************************************************************/

uint32_t leap_env_destroy()
{

	if (--leap_env_init_count != 0) {
		return NCSCC_RC_SUCCESS;
	}

	TRACE("DESTROYING LEAP ENVIRONMENT");

	/* Destroying  execution control block */
	exec_mod_cb_destroy();

#if (NCSL_ENV_INIT_HM == 1)
	/* destroying Handle Manager */
	(void)ncshm_delete();
#endif

#if (NCSL_ENV_INIT_TMR == 1)
	/* destroying LEAP Timer Service */
	(void)sysfTmrDestroy();
#endif

	ncs_os_atomic_destroy();

	TRACE("DONE DESTROYING LEAP ENVIRONMENT");

	return NCSCC_RC_SUCCESS;
}

void opensaf_reboot_prepare(void)
{
	if (sysrq_trigger_fd != -1)
		return;
	int fd;
	do {
		fd = open("/proc/sysrq-trigger", O_WRONLY);
	} while (fd == -1 && errno == EINTR);
	if (fd >= 0 && fd <= 2) {
		/* We don't want to get file descriptors 0, 1 or 2 because:
		 *   1) it would be dangerous
		 *   2) it would by closed by deamonize()
		 *
		 * If this happens, we request a new file descriptor by calling
		 * ourselves recursively. Since the old file descriptor is
		 * still open during the recursive call, we are guaranteed to
		 * eventually get a descriptor outside the range [0, 2].
		 */
		opensaf_reboot_prepare();
		close(fd);
	} else {
		sysrq_trigger_fd = fd;
	}
}

/**
 *  This is a signal handler that is called when the time supervision of the
 *  opensaf_reboot script expires. It reboots the node immediately by writing
 *  "b" to the file "/proc/sysrq-trigger" and then exit the process.
 */
static void opensaf_reboot_fallback(int sig_no)
{
	(void)sig_no;
	int fd = sysrq_trigger_fd;
	if (fd == -1) {
		do {
			fd = open("/proc/sysrq-trigger", O_WRONLY);
		} while (fd == -1 && errno == EINTR);
	}
	if (fd != -1) {
		char buf[] = {'b'};
		ssize_t result;
		do {
			result = write(fd, buf, sizeof(buf));
		} while (result == -1 && errno == EINTR);
	}
	_Exit(EXIT_SUCCESS);
}

/**
 *
 * @param reason
 */
void opensaf_reboot(unsigned node_id, const char *ee_name, const char *reason)
{
	char *env_var = getenv("OPENSAF_REBOOT_TIMEOUT");
	unsigned long supervision_time = 0;
	if (env_var != NULL) {
		char *endptr;
		errno = 0;
		supervision_time = strtoul(env_var, &endptr, 0);
		if (errno != 0 || *env_var == '\0' || *endptr != '\0') {
			supervision_time = 0;
		}
	}

	unsigned own_node_id = ncs_get_node_id();
	bool use_fallback =
	    supervision_time > 0 && (node_id == 0 || node_id == own_node_id);
	if (use_fallback) {
		if (signal(SIGALRM, opensaf_reboot_fallback) == SIG_ERR) {
			opensaf_reboot_fallback(0);
		}
		alarm(supervision_time);
	}

	syslog(LOG_CRIT,
	       "Rebooting OpenSAF NodeId = %x EE Name = %s, Reason: %s, "
	       "OwnNodeId = %x, SupervisionTime = %lu",
	       node_id, ee_name == NULL ? "No EE Mapped" : ee_name, reason,
	       own_node_id, supervision_time);

	char str[256];
	snprintf(str, sizeof(str), PKGLIBDIR "/opensaf_reboot %u %s", node_id,
		 ee_name == NULL ? "" : ee_name);
	int reboot_result = system(str);
	if (reboot_result != EXIT_SUCCESS) {
		syslog(LOG_CRIT, "node reboot failure: exit code %d",
		       reboot_result);
	}

	if (use_fallback) {
		/* Wait for the alarm signal we set up earlier. */
		for (;;)
			pause();
	}
}

void opensaf_quick_reboot(const char *reason)
{
	syslog(LOG_CRIT, "Quick local node rebooting, Reason: %s ", reason);
	int reboot_result = system(PKGLIBDIR "/opensaf_reboot");
	if (reboot_result != EXIT_SUCCESS) {
		syslog(LOG_CRIT, "node reboot failure: exit code %d",
		       WEXITSTATUS(reboot_result));
	}
}

/**
 * syslog and abort to generate a core dump (if enabled)
 * @param __file
 * @param __line
 * @param __func
 * @param __assertion
 */
void __osafassert_fail(const char *__file, int __line, const char *__func,
		       const char *__assertion)
{
	syslog(LOG_ERR, "%s:%d: %s: Assertion '%s' failed.", __file, __line,
	       __func, __assertion);
	abort();
}
