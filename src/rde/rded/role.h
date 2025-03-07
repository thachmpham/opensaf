/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2016 The OpenSAF Foundation
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

#ifndef RDE_RDED_ROLE_H_
#define RDE_RDED_ROLE_H_

#include <time.h>
#include <cstdint>
#include <set>
#include <string>
#include <atomic>
#include "base/macros.h"
#include "mds/mds_papi.h"
#include "rde/agent/rda_papi.h"

namespace base {
class Process;
}

struct RDE_CONTROL_BLOCK;

class Role {
 public:
  explicit Role(NODE_ID own_node_id);
  void AddPeer(NODE_ID node_id);
  bool IsCandidate();
  bool IsPeerPresent();
  void SetPeerState(PCS_RDA_ROLE node_role, NODE_ID node_id,
                    uint64_t peer_promote_pending);
  timespec* Poll(timespec* ts);
  uint32_t SetRole(PCS_RDA_ROLE new_role);
  PCS_RDA_ROLE role() const;
  static const char* to_string(PCS_RDA_ROLE role);
  static void MonitorCallback(const std::string& key,
                              const std::string& new_value, SYSF_MBX mbx);
  void NodePromoted();
  void PromoteNodeLate();
  void RefreshConsensusState(RDE_CONTROL_BLOCK* cb);
  void RemovePeer(NODE_ID node_id);

 private:
  static const uint64_t kDefaultDiscoverPeerTimeout = 2000;
  static const uint64_t kDefaultWaitPeerInfoTimeout = 2000;
  static const uint64_t kDefaultPreActiveScriptTimeout = 5000;
  void ExecutePreActiveScript();
  void ResetElectionTimer();
  void ResetPeerInfoWaitTimer();
  void StopPeerInfoWaitTimer();
  uint32_t UpdateMdsRegistration(PCS_RDA_ROLE new_role, PCS_RDA_ROLE old_role);
  void PromoteNode(const uint64_t cluster_size, const bool relaxed_mode);

  std::set<NODE_ID> known_nodes_;
  std::atomic<PCS_RDA_ROLE> role_;
  NODE_ID own_node_id_;
  base::Process* proc_;
  timespec election_end_time_;
  uint64_t discover_peer_timeout_;
  uint64_t pre_active_script_timeout_;
  static const char* const role_names_[];
  static const char* const pre_active_script_;
  bool received_peer_info_;
  timespec peer_info_wait_time_;
  uint64_t peer_info_wait_timeout_;

  DELETE_COPY_AND_MOVE_OPERATORS(Role);
};

#endif  // RDE_RDED_ROLE_H_
