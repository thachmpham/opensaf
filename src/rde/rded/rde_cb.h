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

#ifndef RDE_RDED_RDE_CB_H_
#define RDE_RDED_RDE_CB_H_

#include <atomic>
#include <cstdint>
#include <set>
#include "base/osaf_utility.h"
#include "mds/mds_papi.h"
#include "rde/agent/rda_papi.h"
#include "rde/common/rde_rda_common.h"
#include "rde/rded/rde_amf.h"
#include "rde/rded/rde_rda.h"

/*
 **  RDE_CONTROL_BLOCK
 **
 **  Structure containing all state information for RDE
 **
 */

enum class State {kNotActive = 0, kNotActiveSeenPeer, kActiveElected,
                  kActiveElectedSeenPeer, kActiveFailover};

enum class ConsensusState {kUnknown = 0, kConnected, kDisconnected};

struct RDE_CONTROL_BLOCK {
  SYSF_MBX mbx;
  NCSCONTEXT task_handle;
  bool task_terminate;
  RDE_RDA_CB rde_rda_cb;
  RDE_AMF_CB rde_amf_cb;
  bool monitor_lock_thread_running{false};
  bool monitor_takeover_req_thread_running{false};
  std::set<NODE_ID> cluster_members{};
  // used for discovering peer controllers, regardless of their role
  std::set<NODE_ID> peer_controllers{};
  State state{State::kNotActive};
  std::atomic<ConsensusState> consensus_service_state{ConsensusState::kUnknown};
  std::atomic<bool> state_refresh_thread_started{false}; // consensus service
  struct timespec promote_start{0};
  uint64_t promote_pending{0};
};

enum RDE_MSG_TYPE {
  RDE_MSG_PEER_UP = 1,
  RDE_MSG_PEER_DOWN = 2,
  RDE_MSG_PEER_INFO_REQ = 3,
  RDE_MSG_PEER_INFO_RESP = 4,
  RDE_MSG_NEW_ACTIVE_CALLBACK = 5,
  RDE_MSG_NODE_UP = 6,
  RDE_MSG_NODE_DOWN = 7,
  RDE_MSG_TAKEOVER_REQUEST_CALLBACK = 8,
  RDE_MSG_ACTIVE_PROMOTION_SUCCESS = 9,
  RDE_MSG_CONTROLLER_UP = 10,
  RDE_MSG_CONTROLLER_DOWN = 11
};

struct rde_peer_info {
  PCS_RDA_ROLE ha_role;
  uint64_t promote_pending;
};

struct rde_msg {
  rde_msg *next;
  RDE_MSG_TYPE type;
  MDS_DEST fr_dest;
  NODE_ID fr_node_id;
  union {
    rde_peer_info peer_info;
    char* takeover_request = nullptr;
  } info;
};

extern const char *rde_msg_name[];

/*****************************************************************************\
*                                                                             *
*                       Function Prototypes                                   *
*                                                                             *
\*****************************************************************************/

extern RDE_CONTROL_BLOCK *rde_get_control_block();
extern uint32_t rde_mds_register();
extern uint32_t rde_discovery_mds_register();
extern uint32_t rde_mds_unregister();
extern uint32_t rde_discovery_mds_unregister();
extern uint32_t rde_mds_broadcast(rde_msg *msg);
extern uint32_t rde_set_role(PCS_RDA_ROLE role);

#endif  // RDE_RDED_RDE_CB_H_
