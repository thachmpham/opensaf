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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rde/rded/role.h"
#include <cinttypes>
#include <cstdint>
#include <thread>
#include "base/getenv.h"
#include "base/logtrace.h"
#include "base/ncs_main_papi.h"
#include "base/ncssysf_def.h"
#include "base/process.h"
#include "base/time.h"
#include "osaf/consensus/consensus.h"
#include "rde/rded/rde_cb.h"

const char* const Role::role_names_[] = {"Undefined", "ACTIVE",    "STANDBY",
                                         "QUIESCED",  "QUIESCING", "Invalid"};

const char* const Role::pre_active_script_ = PKGLIBDIR "/opensaf_sc_active";

PCS_RDA_ROLE Role::role() const { return role_; }

const char* Role::to_string(PCS_RDA_ROLE role) {
  return role >= 0 && role < sizeof(role_names_) / sizeof(role_names_[0])
             ? role_names_[role]
             : role_names_[0];
}

void Role::MonitorCallback(const std::string& key, const std::string& new_value,
                           SYSF_MBX mbx) {
  TRACE_ENTER();

  rde_msg* msg = static_cast<rde_msg*>(malloc(sizeof(rde_msg)));
  if (key == Consensus::kTakeoverRequestKeyname) {
    std::string request;
    Consensus consensus_service;

    if (new_value.empty() == true) {
      // sometimes the KV store plugin doesn't return the new value,
      // let's try to read it in this thread to avoid stalling
      // the main thread
      TRACE("Empty takeover request from callback. Try reading it");

      SaAisErrorT rc = SA_AIS_ERR_TRY_AGAIN;
      constexpr uint8_t max_retry = 5;
      uint8_t retries = 0;

      while (retries < max_retry && rc != SA_AIS_OK) {
        rc = consensus_service.ReadTakeoverRequest(request);
        ++retries;
      }
    } else {
      // use the value received in callback
      request = new_value;
    }

    msg->type = RDE_MSG_TAKEOVER_REQUEST_CALLBACK;
    size_t len = request.length() + 1;
    msg->info.takeover_request = new char[len];
    strncpy(msg->info.takeover_request, request.c_str(), len);
    TRACE("Sending takeover request '%s' to main thread",
           msg->info.takeover_request);
    if (consensus_service.SelfFence(request) == false &&
        consensus_service.PrioritisePartitionSize() == true) {
      // don't send this to the main thread straight away, as it will
      // need some time to process topology changes.
      std::this_thread::sleep_for(
        std::chrono::seconds(
          consensus_service.PrioritisePartitionSizeWaitTime()));
    }
  } else {
    msg->type = RDE_MSG_NEW_ACTIVE_CALLBACK;
  }

  uint32_t status;
  status = m_NCS_IPC_SEND(&mbx, msg, NCS_IPC_PRIORITY_NORMAL);
  osafassert(status == NCSCC_RC_SUCCESS);
}

void Role::PromoteNode(const uint64_t cluster_size,
                       const bool relaxed_mode) {
  TRACE_ENTER();
  SaAisErrorT rc;

  Consensus consensus_service;
  bool promotion_pending = false;

  rc = consensus_service.PromoteThisNode(true, cluster_size);
  if (rc == SA_AIS_ERR_EXIST) {
    LOG_WA("Another controller is already active");
    if (role() == PCS_RDA_UNDEFINED) SetRole(PCS_RDA_QUIESCED);
    return;
  } else if (rc != SA_AIS_OK && relaxed_mode == true) {
    LOG_WA("Unable to set active controller in consensus service");
    if (role() == PCS_RDA_QUIESCED) {
      LOG_WA("Another controller is already promoted");
      return;
    }
    LOG_WA("Will become active anyway");
    promotion_pending = true;
  } else if (rc != SA_AIS_OK) {
    LOG_ER("Unable to set active controller in consensus service");
    opensaf_quick_reboot("Unable to set active controller "
        "in consensus service");
    return;
  }

  RDE_CONTROL_BLOCK* cb = rde_get_control_block();

  // send msg to main thread
  rde_msg* msg = static_cast<rde_msg*>(malloc(sizeof(rde_msg)));
  msg->type = RDE_MSG_ACTIVE_PROMOTION_SUCCESS;
  uint32_t status;
  status = m_NCS_IPC_SEND(&cb->mbx, msg, NCS_IPC_PRIORITY_HIGH);
  osafassert(status == NCSCC_RC_SUCCESS);

  if (promotion_pending) {
    osafassert(consensus_service.IsRelaxedNodePromotionEnabled() == true);
    // the node has been promoted, even though the lock has not been obtained
    // keep trying the consensus service
    while (rc != SA_AIS_OK) {
      rc = consensus_service.PromoteThisNode(true, cluster_size);
      if (rc == SA_AIS_ERR_EXIST) {
        LOG_ER("Unable to set active controller in consensus service");
        opensaf_quick_reboot("Unable to set active controller in "
            "consensus service");
        return;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    LOG_NO("Successfully set active controller in consensus service");
  }
}

void Role::NodePromoted() {
  // promoted to active from election
  ExecutePreActiveScript();
  LOG_NO("Switched to ACTIVE from %s", to_string(role()));
  PCS_RDA_ROLE old_role = role_;
  role_ = PCS_RDA_ACTIVE;
  rde_rda_send_role(role_);
  if (UpdateMdsRegistration(role_, old_role) != NCSCC_RC_SUCCESS) {
    LOG_ER("Failed to update MDS Registration");
    abort();
  }

  Consensus consensus_service;
  RDE_CONTROL_BLOCK* cb = rde_get_control_block();
  if (cb->peer_controllers.empty() == false) {
    TRACE("Set state to kActiveElectedSeenPeer");
    cb->state = State::kActiveElectedSeenPeer;
  } else {
    TRACE("Set state to kActiveElected");
    cb->state = State::kActiveElected;
  }

  // register for callback if active controller is changed
  // in consensus service
  if (consensus_service.IsEnabled() == true &&
      cb->monitor_lock_thread_running == false) {
    cb->monitor_lock_thread_running = true;
    consensus_service.MonitorLock(MonitorCallback, cb->mbx);
  }
  if (consensus_service.IsEnabled() == true &&
      cb->monitor_takeover_req_thread_running == false) {
    cb->monitor_takeover_req_thread_running = true;
    consensus_service.MonitorTakeoverRequest(MonitorCallback, cb->mbx);
  }
}

Role::Role(NODE_ID own_node_id)
    : known_nodes_{},
      role_{PCS_RDA_QUIESCED},
      own_node_id_{own_node_id},
      proc_{new base::Process()},
      election_end_time_{},
      discover_peer_timeout_{base::GetEnv("RDE_DISCOVER_PEER_TIMEOUT",
                                          kDefaultDiscoverPeerTimeout)},
      pre_active_script_timeout_{base::GetEnv(
          "RDE_PRE_ACTIVE_SCRIPT_TIMEOUT", kDefaultPreActiveScriptTimeout)},
      received_peer_info_{true},
      peer_info_wait_time_{},
      peer_info_wait_timeout_ {kDefaultWaitPeerInfoTimeout} {}

timespec* Role::Poll(timespec* ts) {
  TRACE_ENTER();
  timespec* timeout = nullptr;
  if (role_ == PCS_RDA_UNDEFINED) {
    timespec now = base::ReadMonotonicClock();
    if (election_end_time_ >= now) {
      *ts = election_end_time_ - now;
      timeout = ts;
    } else {
      election_end_time_ = base::kTimespecMax;
      RDE_CONTROL_BLOCK* cb = rde_get_control_block();

      bool is_candidate = IsCandidate();
      Consensus consensus_service;
      if (consensus_service.IsEnabled() == true &&
        is_candidate == false &&
        consensus_service.IsWritable() == false) {
        // node promotion will fail resulting in node reboot,
        // reset timer and try later
        TRACE("reset timer and try later");
        ResetElectionTimer();
        now = base::ReadMonotonicClock();
        *ts = election_end_time_ - now;
        timeout = ts;
      } else {
        std::thread(&Role::PromoteNode,
                    this, cb->cluster_members.size(),
                    is_candidate).detach();
      }
    }
  } else if (role_ == PCS_RDA_ACTIVE) {
    RDE_CONTROL_BLOCK* cb = rde_get_control_block();
    if (cb->consensus_service_state == ConsensusState::kUnknown ||
        cb->consensus_service_state == ConsensusState::kDisconnected) {
      // consensus service was previously disconnected, refresh state
      Consensus consensus_service;
      if (consensus_service.IsEnabled() == true &&
        cb->state_refresh_thread_started == false) {
        cb->state_refresh_thread_started = true;
        std::thread(&Role::RefreshConsensusState, this, cb).detach();
      }
      if (consensus_service.IsEnabled() == false) {
        // We are already ACTIVE, and has just discovered a new node
        // which makes the election_end_time_ reset
        if (received_peer_info_ == false) {
          timespec now = base::ReadMonotonicClock();
          if (peer_info_wait_time_ >= now) {
            *ts = peer_info_wait_time_ - now;
            timeout = ts;
          } else {
            // Timeout but haven't received peer info
            // The peer RDE could be in ACTIVE
            // thus self-fence to avoid split-brain risk
            LOG_ER("Discovery peer up without peer info. Risk in split-brain,"
                "rebooting this node");
            opensaf_quick_reboot("Probable split-brain due to "
                "unknown RDE peer info");
          }
        }
      }
    }
  }
  return timeout;
}

void Role::ExecutePreActiveScript() {
  int argc = 1;
  char* argv[] = {const_cast<char*>(pre_active_script_), nullptr};
  proc_->Execute(argc, argv,
                 std::chrono::milliseconds(pre_active_script_timeout_));
}

void Role::AddPeer(NODE_ID node_id) {
  TRACE_ENTER();
  auto result = known_nodes_.insert(node_id);
  if (result.second) {
    ResetElectionTimer();
    if (role_ == PCS_RDA_ACTIVE) {
      ResetPeerInfoWaitTimer();
      received_peer_info_ = false;
    }
  }
}

void Role::RemovePeer(NODE_ID node_id) {
  TRACE_ENTER();
  if (received_peer_info_ == false && role_ != PCS_RDA_ACTIVE) {
    StopPeerInfoWaitTimer();
  }
  known_nodes_.erase(node_id);
}

// call from main thread only
bool Role::IsCandidate() {
  TRACE_ENTER();
  bool result = false;
  Consensus consensus_service;
  RDE_CONTROL_BLOCK* cb = rde_get_control_block();

  // if relaxed node promotion is enabled, allow this node to be promoted
  // active if it can see a peer SC and this node has the lowest node ID
  if (consensus_service.IsRelaxedNodePromotionEnabled() == true &&
      cb->state == State::kNotActiveSeenPeer) {
    LOG_NO("Relaxed node promotion enabled. This node is a candidate.");
    result = true;
  }

  return result;
}

bool Role::IsPeerPresent() {
  bool result = false;
  RDE_CONTROL_BLOCK* cb = rde_get_control_block();

  if (cb->peer_controllers.empty() == false) {
    result = true;
  }

  return result;
}

uint32_t Role::SetRole(PCS_RDA_ROLE new_role) {
  TRACE_ENTER();
  PCS_RDA_ROLE old_role = role_;
  if (new_role == PCS_RDA_ACTIVE &&
      (old_role == PCS_RDA_UNDEFINED || old_role == PCS_RDA_QUIESCED)) {
    LOG_NO("Requesting ACTIVE role");
    new_role = PCS_RDA_UNDEFINED;
    RDE_CONTROL_BLOCK* cb = rde_get_control_block();
    cb->promote_start = base::ReadMonotonicClock();
    cb->promote_pending = 0;
  }
  if (new_role != old_role) {
    LOG_NO("RDE role set to %s", to_string(new_role));
    if (new_role == PCS_RDA_ACTIVE) {
      ExecutePreActiveScript();

      // register for callback if active controller is changed
      // in consensus service
      Consensus consensus_service;
      RDE_CONTROL_BLOCK* cb = rde_get_control_block();
      cb->state = State::kActiveFailover;
      if (consensus_service.IsEnabled() == true &&
          cb->monitor_lock_thread_running == false) {
        cb->monitor_lock_thread_running = true;
        consensus_service.MonitorLock(MonitorCallback, cb->mbx);
      }
      if (consensus_service.IsEnabled() == true &&
          cb->monitor_takeover_req_thread_running == false) {
        cb->monitor_takeover_req_thread_running = true;
        consensus_service.MonitorTakeoverRequest(MonitorCallback, cb->mbx);
      }
    }
    role_ = new_role;
    if (new_role == PCS_RDA_UNDEFINED) {
      known_nodes_.clear();
      ResetElectionTimer();
    } else {
      rde_rda_send_role(new_role);
    }
  }
  return UpdateMdsRegistration(new_role, old_role);
}

void Role::ResetElectionTimer() {
  TRACE_ENTER();
  election_end_time_ = base::ReadMonotonicClock() +
                       base::MillisToTimespec(discover_peer_timeout_);
}

void Role::ResetPeerInfoWaitTimer() {
  TRACE_ENTER();
  LOG_NO("Start/restart waiting peer info timer");
  peer_info_wait_time_ = base::ReadMonotonicClock() +
                       base::MillisToTimespec(peer_info_wait_timeout_);
}

void Role::StopPeerInfoWaitTimer() {
  TRACE_ENTER();
  // Turn off peer_info_timer
  received_peer_info_ = true;
}

uint32_t Role::UpdateMdsRegistration(PCS_RDA_ROLE new_role,
                                     PCS_RDA_ROLE old_role) {
  uint32_t rc = NCSCC_RC_SUCCESS;
  bool mds_registered_before = old_role != PCS_RDA_QUIESCED;
  bool mds_registered_after = new_role != PCS_RDA_QUIESCED;
  if (mds_registered_after != mds_registered_before) {
    if (mds_registered_after) {
      if (rde_mds_register() != NCSCC_RC_SUCCESS) {
        LOG_ER("rde_mds_register() failed");
        rc = NCSCC_RC_FAILURE;
      }
    } else {
      if (rde_mds_unregister() != NCSCC_RC_SUCCESS) {
        LOG_ER("rde_mds_unregister() failed");
        rc = NCSCC_RC_FAILURE;
      }
    }
  }
  return rc;
}

void Role::SetPeerState(PCS_RDA_ROLE node_role, NODE_ID node_id,
                        uint64_t peer_promote_pending) {
  TRACE_ENTER();
  if (role() == PCS_RDA_UNDEFINED) {
    bool give_up = false;
    RDE_CONTROL_BLOCK *cb = rde_get_control_block();
    if (node_role == PCS_RDA_UNDEFINED) {
      if (cb->promote_pending == 0) {
        struct timespec now = base::ReadMonotonicClock();
        cb->promote_pending = base::TimespecToMillis(now - cb->promote_start);
      }
      if ((cb->promote_pending < peer_promote_pending) ||
          (cb->promote_pending == peer_promote_pending &&
           node_id < own_node_id_))
        give_up = true;
    }
    if (node_role == PCS_RDA_ACTIVE || node_role == PCS_RDA_STANDBY ||
        give_up) {
      // broadcast QUIESCED role to all peers to stop their waiting peer
      // info timer
      rde_msg peer_info_req;
      peer_info_req.type = RDE_MSG_PEER_INFO_RESP;
      peer_info_req.info.peer_info.ha_role = PCS_RDA_QUIESCED;
      peer_info_req.info.peer_info.promote_pending = 0;
      rde_mds_broadcast(&peer_info_req);

      SetRole(PCS_RDA_QUIESCED);
      LOG_NO("Giving up election against 0x%" PRIx32
             " with role %s. "
             "My role is now %s",
             node_id, to_string(node_role), to_string(role()));
    }
  }
  known_nodes_.insert(node_id);
  StopPeerInfoWaitTimer();
}

void Role::PromoteNodeLate() {
  TRACE_ENTER();

  // we are already active and split brain prevention has been
  // enabled during runtime, we need to obtain lock
  RDE_CONTROL_BLOCK* cb = rde_get_control_block();
  std::thread(&Role::PromoteNode,
              this, cb->cluster_members.size(),
              true).detach();
}

void Role::RefreshConsensusState(RDE_CONTROL_BLOCK* cb) {
  TRACE_ENTER();

  Consensus consensus_service;
  if (consensus_service.IsWritable() == true) {
    LOG_NO("Connectivity to consensus service established");
    cb->consensus_service_state = ConsensusState::kConnected;
  }

  cb->state_refresh_thread_started = false;
}
