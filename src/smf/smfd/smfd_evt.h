/*      OpenSAF
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
 * Author(s): Ericsson AB
 *
 */

#ifndef SMF_SMFD_SMFD_EVT_H_
#define SMF_SMFD_SMFD_EVT_H_

#ifdef __cplusplus
extern "C" {
#endif

void smfd_process_mbx(SYSF_MBX* mbx);

#ifdef __cplusplus
}
#endif

#endif  // SMF_SMFD_SMFD_EVT_H_
