/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2019 The OpenSAF Foundation
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
#include "mds/mds_tipc_fctrl_intf.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/poll.h>
#include <unistd.h>

#include <map>
#include <mutex>

#include "base/ncssysf_def.h"
#include "base/ncssysf_tsk.h"
#include "base/ncs_osprm.h"
#include "base/osaf_poll.h"

#include "mds/mds_log.h"
#include "mds/mds_tipc_fctrl_portid.h"
#include "mds/mds_tipc_fctrl_msg.h"

using mds::Event;
using mds::TipcPortId;
using mds::Timer;
using mds::DataMessage;
using mds::ChunkAck;
using mds::HeaderMessage;
using mds::Nack;
using mds::Intro;

namespace {
// flow control enabled/disabled
bool is_fctrl_enabled = false;

// multicast/broadcast enabled
// todo: to be removed if flow control support it
bool is_mcast_enabled = true;

// mailbox handles for events
SYSF_MBX mbx_events;

// ncs task handle
NCSCONTEXT p_task_hdl = nullptr;

// data socket associated with port id
int data_sock_fd = 0;
struct tipc_portid snd_rcv_portid;

// socket receiver's buffer size
// todo: This buffer size is read from the local node as an estimated
// buffer size of the receiver sides, in facts that could be different
// at the receiver's sides (in the other nodes). At this moment, we
// assume all nodes in cluster have set the same tipc buffer size
uint64_t sock_buf_size = 0;

// map of key:unique id(adest), value: TipcPortId instance
// unique id is derived from struct tipc_portid
std::map<uint64_t, TipcPortId*> portid_map;
std::mutex portid_map_mutex;

// probation timer event to enable flow control at receivers
const int64_t kBaseTimerInt = 100;  // in centisecond
const uint8_t kTxProbMaxRetries = 10;
Timer txprob_timer(Event::Type::kEvtTmrTxProb);

// chunk ack parameters
// todo: The chunk ack timeout and chunk ack size should be configurable
int chunk_ack_timeout = 1000;  // in miliseconds
uint16_t chunk_ack_size = 3;

TipcPortId* portid_lookup(struct tipc_portid id) {
  uint64_t uid = TipcPortId::GetUniqueId(id);
  if (portid_map.find(uid) == portid_map.end()) return nullptr;
  return portid_map[uid];
}

void tmr_exp_cbk(void* uarg) {
  Timer* timer = reinterpret_cast<Timer*>(uarg);
  if (timer != nullptr) {
    timer->is_active_ = false;
    // send to fctrl thread
    if (m_NCS_IPC_SEND(&mbx_events, new Event(timer->type_),
        NCS_IPC_PRIORITY_HIGH) != NCSCC_RC_SUCCESS) {
      m_MDS_LOG_ERR("FCTRL: Failed to send msg to mbx_events, Error[%s]",
          strerror(errno));
    }
  }
}

void process_timer_event(const Event& evt) {
  bool txprob_restart = false;
  m_MDS_LOG_DBG("FCTRL: process timer event start [evt:%d]",
    static_cast<int>(evt.type_));
  for (auto i : portid_map) {
    TipcPortId* portid = i.second;
    if (!portid) continue;
    if (evt.type_ == Event::Type::kEvtTmrTxProb) {
      if (portid->ReceiveTmrTxProb(kTxProbMaxRetries) == true) {
        txprob_restart = true;
      }
    }

    if (evt.type_ == Event::Type::kEvtTmrChunkAck) {
      portid->ReceiveTmrChunkAck();
      portid->SendUnsentMsg(false);
    }
  }
  if (txprob_restart) {
    txprob_timer.Start(kBaseTimerInt, tmr_exp_cbk);
    m_MDS_LOG_DBG("FCTRL: Restart txprob");
  }
  m_MDS_LOG_DBG("FCTRL: process timer event end");
}

uint32_t process_flow_event(const Event& evt) {
  uint32_t rc = NCSCC_RC_SUCCESS;
  m_MDS_LOG_DBG("FCTRL: process flow event start [evt:%d]",
    static_cast<int>(evt.type_));
  TipcPortId *portid = portid_lookup(evt.id_);
  if (portid == nullptr) {
    // the null portid normally should not happen; however because the
    // tipc_cb.Dsock and tipc_cb.BSRsock are separated; the data message
    // sent from BSRsock may come before reception of TIPC_PUBLISHED
    if (evt.type_ == Event::Type::kEvtRcvData) {
      portid = new TipcPortId(evt.id_, data_sock_fd,
          chunk_ack_size, sock_buf_size);
      portid_map[TipcPortId::GetUniqueId(evt.id_)] = portid;
      if (evt.legacy_data_ == true) {
        // we create portid and set state kDisabled even though we know
        // this portid has no flow control. It is because the 2nd, 3rd fragment
        // could not reflect the protocol version, so need to keep this portid
        // remained stateful
        portid->ChangeState(TipcPortId::State::kDisabled);
      } else {
        rc = portid->ReceiveData(evt.mseq_, evt.mfrag_,
              evt.fseq_, evt.svc_id_, evt.snd_type_, is_mcast_enabled);
      }
    } else if (evt.type_ == Event::Type::kEvtRcvIntro) {
      portid = new TipcPortId(evt.id_, data_sock_fd,
          chunk_ack_size, sock_buf_size);
      portid_map[TipcPortId::GetUniqueId(evt.id_)] = portid;
      portid->ReceiveIntro();
    } else {
      m_MDS_LOG_ERR("FCTRL: [me] <-- [node:%x, ref:%u], "
          "RcvEvt[evt:%d], Error[PortId not found]",
          evt.id_.node, evt.id_.ref, static_cast<int>(evt.type_));
    }
  } else {
    if (evt.type_ == Event::Type::kEvtRcvData) {
      if (evt.legacy_data_ == true) {
        portid->ChangeState(TipcPortId::State::kDisabled);
      } else {
        rc = portid->ReceiveData(evt.mseq_, evt.mfrag_,
            evt.fseq_, evt.svc_id_, evt.snd_type_, is_mcast_enabled);
      }
    }
    if (evt.type_ == Event::Type::kEvtRcvChunkAck) {
      portid->ReceiveChunkAck(evt.fseq_, evt.chunk_size_);
    }
    if (evt.type_ == Event::Type::kEvtSendChunkAck) {
      portid->SendChunkAck(evt.fseq_, evt.svc_id_, evt.chunk_size_);
    }
    if (evt.type_ == Event::Type::kEvtDropData ||
        evt.type_ == Event::Type::kEvtRcvNack) {
      portid->ReceiveNack(evt.mseq_, evt.mfrag_,
          evt.fseq_);
    }
    if (evt.type_ == Event::Type::kEvtRcvIntro) {
      portid->ReceiveIntro();
    }
  }
  m_MDS_LOG_DBG("FCTRL: process flow event end");
  return rc;
}

bool mds_fctrl_mbx_cleanup(NCSCONTEXT arg, NCSCONTEXT msg) {
  Event *evt = reinterpret_cast<Event*>(msg);
  if (evt != nullptr)
    delete evt;
  return true;
}

uint32_t process_all_events(void) {
  bool running = true;
  enum { FD_FCTRL = 0, NUM_FDS };

  int poll_tmo = chunk_ack_timeout;
  struct pollfd pfd[NUM_FDS] = {{0, 0, 0}};

  pfd[FD_FCTRL].fd =
      ncs_ipc_get_sel_obj(&mbx_events).rmv_obj;
  pfd[FD_FCTRL].events = POLLIN;

  while (running) {
    int pollres;

    pollres = poll(pfd, NUM_FDS, poll_tmo);

    if (pollres == -1) {
      if (errno == EINTR) continue;
      m_MDS_LOG_ERR("FCTRL: poll() failed, Error[%s]", strerror(errno));
      break;
    }

    if (pollres > 0) {
      if (pfd[FD_FCTRL].revents == POLLIN) {
        portid_map_mutex.lock();

        Event *evt = reinterpret_cast<Event*>(ncs_ipc_non_blk_recv(
            &mbx_events));

        if (evt == nullptr) {
          portid_map_mutex.unlock();
          continue;
        }
        if (evt->IsTimerEvent()) {
          process_timer_event(*evt);
        }
        if (evt->IsFlowEvent()) {
          process_flow_event(*evt);
        }
        if (evt->IsShutDownEvent()) {
          running = false;
        }

        portid_map_mutex.unlock();
        if (!running) m_NCS_SEL_OBJ_IND(&evt->destroy_ack_obj_);
        delete evt;
      }
    }
    // timeout, scan all portid and send ack msgs
    if (pollres == 0) {
      portid_map_mutex.lock();
      process_timer_event(Event(Event::Type::kEvtTmrChunkAck));
      portid_map_mutex.unlock();
    }
  }  /* while */
  m_MDS_LOG_DBG("FCTRL: process_all_events() thread end");
  return NCSCC_RC_SUCCESS;
}

uint32_t create_ncs_task(NCSCONTEXT *task_hdl) {
  int policy = SCHED_OTHER; /*root defaults */
  int max_prio = sched_get_priority_max(policy);
  int min_prio = sched_get_priority_min(policy);
  int prio_val = ((max_prio - min_prio) * 0.87);

  if (m_NCS_IPC_CREATE(&mbx_events) != NCSCC_RC_SUCCESS) {
    m_MDS_LOG_ERR("FCTRL: m_NCS_IPC_CREATE failed, Error[%s]",
        strerror(errno));
    return NCSCC_RC_FAILURE;
  }
  if (m_NCS_IPC_ATTACH(&mbx_events) != NCSCC_RC_SUCCESS) {
    m_MDS_LOG_ERR("FCTRL: m_NCS_IPC_ATTACH failed, Error[%s]",
        strerror(errno));
    m_NCS_IPC_RELEASE(&mbx_events, nullptr);
    return NCSCC_RC_FAILURE;
  }
  if (ncs_task_create((NCS_OS_CB)process_all_events, 0,
          "OSAF_MDS", prio_val, policy, NCS_MDTM_STACKSIZE,
          task_hdl) != NCSCC_RC_SUCCESS) {
    m_MDS_LOG_ERR("FCTRL: ncs_task_create(), Error[%s]",
        strerror(errno));
    m_NCS_IPC_RELEASE(&mbx_events, nullptr);
    return NCSCC_RC_FAILURE;
  }

  m_MDS_LOG_NOTIFY("FCTRL: Start process_all_events");
  return NCSCC_RC_SUCCESS;
}

}  // end local namespace

uint32_t mds_tipc_fctrl_initialize(int dgramsock, struct tipc_portid id,
    uint64_t rcv_buf_size, int32_t ackto, int32_t acksize,
    bool mcast_enabled) {

  data_sock_fd = dgramsock;
  snd_rcv_portid = id;
  sock_buf_size = rcv_buf_size;
  is_mcast_enabled = mcast_enabled;
  if (ackto != -1) chunk_ack_timeout = ackto;
  if (acksize != -1) chunk_ack_size = acksize;

  if (create_ncs_task(&p_task_hdl) !=
      NCSCC_RC_SUCCESS) {
    m_MDS_LOG_ERR("FCTRL: create_ncs_task() failed, Error[%s]",
        strerror(errno));
    return NCSCC_RC_FAILURE;
  }
  is_fctrl_enabled = true;
  m_MDS_LOG_NOTIFY("FCTRL: Initialize [node:%x, ref:%u]",
      id.node, id.ref);

  return NCSCC_RC_SUCCESS;
}

uint32_t mds_tipc_fctrl_shutdown(void) {
  if (is_fctrl_enabled == false) return NCSCC_RC_SUCCESS;

  NCS_SEL_OBJ destroy_ack_obj;
  m_NCS_SEL_OBJ_CREATE(&destroy_ack_obj);
  Event* pevt = new Event(Event::Type::kEvtShutDown, destroy_ack_obj);
  if (m_NCS_IPC_SEND(&mbx_events, pevt,
      NCS_IPC_PRIORITY_HIGH) != NCSCC_RC_SUCCESS) {
    m_MDS_LOG_ERR("FCTRL: Failed to send shutdown, Error[%s]",
        strerror(errno));
    abort();
  }
  osaf_poll_one_fd(m_GET_FD_FROM_SEL_OBJ(destroy_ack_obj), 10000);
  m_NCS_SEL_OBJ_DESTROY(&destroy_ack_obj);
  memset(&destroy_ack_obj, 0, sizeof(destroy_ack_obj));

  if (ncs_task_release(p_task_hdl) != NCSCC_RC_SUCCESS) {
    m_MDS_LOG_ERR("FCTRL: Stop of the Created Task-failed, Error[%s]",
        strerror(errno));
  }

  m_NCS_IPC_DETACH(&mbx_events, mds_fctrl_mbx_cleanup, nullptr);
  m_NCS_IPC_RELEASE(&mbx_events, nullptr);

  portid_map_mutex.lock();
  for (auto i : portid_map) delete i.second;
  portid_map.clear();

  portid_map_mutex.unlock();
  is_fctrl_enabled = false;

  m_MDS_LOG_NOTIFY("FCTRL: Shutdown [node:%x, ref:%u]",
      snd_rcv_portid.node, snd_rcv_portid.ref);

  return NCSCC_RC_SUCCESS;
}

void mds_tipc_fctrl_sndqueue_capable(struct tipc_portid id,
          uint16_t* next_seq) {
  if (is_fctrl_enabled == false) return;

  portid_map_mutex.lock();

  TipcPortId *portid = portid_lookup(id);
  if (portid && portid->state_ != TipcPortId::State::kDisabled) {
      // assign the sequence number of the outgoing message
      *next_seq = portid->GetCurrentSeq();
  }

  portid_map_mutex.unlock();
}

uint32_t mds_tipc_fctrl_trysend(struct tipc_portid id, const uint8_t *buffer,
    uint16_t len, uint8_t* is_queued) {
  *is_queued = 0;
  if (is_fctrl_enabled == false) return NCSCC_RC_SUCCESS;

  uint32_t rc = NCSCC_RC_SUCCESS;

  portid_map_mutex.lock();

  TipcPortId *portid = portid_lookup(id);
  if (portid == nullptr) {
    m_MDS_LOG_ERR("FCTRL: [me] --> [node:%x, ref:%u], "
        "[line:%u], Error[PortId not found]",
        id.node, id.ref, __LINE__);
    rc = NCSCC_RC_FAILURE;
  } else {
    if (portid->state_ != TipcPortId::State::kDisabled) {
      bool sendable = portid->ReceiveCapable(len);
      portid->Queue(buffer, len, sendable);
      *is_queued = 1;
      // start txprob timer for the first msg sent out
      // do not start for other states
      if (sendable && portid->state_ == TipcPortId::State::kStartup) {
        txprob_timer.Start(kBaseTimerInt, tmr_exp_cbk);
        m_MDS_LOG_DBG("FCTRL: Start txprob");
        portid->state_ = TipcPortId::State::kTxProb;
      }
      if (sendable == false) rc = NCSCC_RC_FAILURE;
    }
  }

  portid_map_mutex.unlock();

  return rc;
}

uint32_t mds_tipc_fctrl_portid_up(struct tipc_portid id, uint32_t type) {
  if (is_fctrl_enabled == false) return NCSCC_RC_SUCCESS;

  MDS_SVC_ID svc_id = (uint16_t)(type & MDS_EVENT_MASK_FOR_SVCID);

  portid_map_mutex.lock();

  // Add this new tipc portid to the map
  TipcPortId *portid = portid_lookup(id);
  uint64_t uid = TipcPortId::GetUniqueId(id);
  if (portid == nullptr) {
    portid_map[uid] = portid = new TipcPortId(id, data_sock_fd,
        chunk_ack_size, sock_buf_size);
    m_MDS_LOG_NOTIFY("FCTRL: Add portid[node:%x, ref:%u svc_id:%u], svc_cnt:%u",
        id.node, id.ref, svc_id, portid->svc_cnt_);
  } else {
    portid->svc_cnt_++;
    m_MDS_LOG_DBG("FCTRL: Add svc[node:%x, ref:%u svc_id:%u], svc_cnt:%u",
        id.node, id.ref, svc_id, portid->svc_cnt_);
  }

  portid_map_mutex.unlock();

  return NCSCC_RC_SUCCESS;
}

uint32_t mds_tipc_fctrl_portid_down(struct tipc_portid id, uint32_t type) {
  if (is_fctrl_enabled == false) return NCSCC_RC_SUCCESS;

  MDS_SVC_ID svc_id = (uint16_t)(type & MDS_EVENT_MASK_FOR_SVCID);

  portid_map_mutex.lock();

  // Delete this tipc portid out of the map
  TipcPortId *portid = portid_lookup(id);
  if (portid != nullptr) {
    portid->svc_cnt_--;
    m_MDS_LOG_DBG("FCTRL: Remove svc[node:%x, ref:%u svc_id:%u], svc_cnt:%u",
        id.node, id.ref, svc_id, portid->svc_cnt_);
    if (portid->svc_cnt_ == 0) {
      delete portid;
      portid_map.erase(TipcPortId::GetUniqueId(id));
      m_MDS_LOG_NOTIFY("FCTRL: Remove portid[node:%x, ref:%u]",
                      id.node, id.ref);
    }
  }
  portid_map_mutex.unlock();

  return NCSCC_RC_SUCCESS;
}

uint32_t mds_tipc_fctrl_portid_terminate(struct tipc_portid id) {
  if (is_fctrl_enabled == false) return NCSCC_RC_SUCCESS;

  portid_map_mutex.lock();

  // Delete this tipc portid out of the map
  TipcPortId *portid = portid_lookup(id);
  if (portid != nullptr) {
    delete portid;
    portid_map.erase(TipcPortId::GetUniqueId(id));
    m_MDS_LOG_NOTIFY("FCTRL: Remove portid[node:%x, ref:%u]", id.node, id.ref);
  }

  portid_map_mutex.unlock();

  return NCSCC_RC_SUCCESS;
}

uint32_t mds_tipc_fctrl_drop_data(uint8_t *buffer, uint16_t len,
    struct tipc_portid id) {
  HeaderMessage header;
  header.Decode(buffer);
  Event* pevt = nullptr;

  // if mds support flow control
  if (header.IsControlMessage()) {
    if (header.msg_type_ == ChunkAck::kChunkAckMsgType) {
      // receive single ack message
      ChunkAck ack;
      ack.Decode(buffer);
      // send to the event thread
      pevt = new Event(Event::Type::kEvtSendChunkAck, id, ack.svc_id_,
          header.mseq_, header.mfrag_, ack.acked_fseq_);
      pevt->chunk_size_ = ack.chunk_size_;
      if (m_NCS_IPC_SEND(&mbx_events, pevt,
          NCS_IPC_PRIORITY_HIGH) != NCSCC_RC_SUCCESS) {
        m_MDS_LOG_ERR("FCTRL: Failed to send msg to mbx_events, Error[%s]",
            strerror(errno));
        return NCSCC_RC_FAILURE;
      }
    }
  }

  if (header.IsFlowMessage()) {
    // receive data message
    DataMessage data;
    data.Decode(buffer);
    // send to the event thread
    pevt = new Event(Event::Type::kEvtDropData, id, data.svc_id_,
        header.mseq_, header.mfrag_, header.fseq_);
    if (m_NCS_IPC_SEND(&mbx_events, pevt,
        NCS_IPC_PRIORITY_HIGH) != NCSCC_RC_SUCCESS) {
      m_MDS_LOG_ERR("FCTRL: Failed to send msg to mbx_events, Error[%s]",
          strerror(errno));
      return NCSCC_RC_FAILURE;
    }
  }

  return NCSCC_RC_SUCCESS;
}

uint32_t mds_tipc_fctrl_rcv_data(uint8_t *buffer, uint16_t len,
    struct tipc_portid id) {
  uint32_t rc = NCSCC_RC_SUCCESS;
  HeaderMessage header;
  header.Decode(buffer);
  Event* pevt = nullptr;

  if (header.IsControlMessage()) {
    // skip the control message if flow control disabled
    if (is_fctrl_enabled == false) return NCSCC_RC_FAILURE;
    // if mds support flow control
    if (header.msg_type_ == ChunkAck::kChunkAckMsgType) {
      m_MDS_LOG_DBG("FCTRL: Receive ChunkAck");
      // receive single ack message
      ChunkAck ack;
      ack.Decode(buffer);
      // send to the event thread
      pevt = new Event(Event::Type::kEvtRcvChunkAck, id, ack.svc_id_,
          header.mseq_, header.mfrag_, ack.acked_fseq_);
      pevt->chunk_size_ = ack.chunk_size_;
      if (m_NCS_IPC_SEND(&mbx_events, pevt,
          NCS_IPC_PRIORITY_HIGH) != NCSCC_RC_SUCCESS) {
        m_MDS_LOG_ERR("FCTRL: Failed to send msg to mbx_events, Error[%s]",
            strerror(errno));
      }
    } else if (header.msg_type_ == Nack::kNackMsgType) {
      m_MDS_LOG_DBG("FCTRL: Receive Nack");
      // receive nack message
      Nack nack;
      nack.Decode(buffer);
      // send to the event thread
      pevt = new Event(Event::Type::kEvtRcvNack, id, nack.svc_id_,
          header.mseq_, header.mfrag_, nack.nacked_fseq_);
      if (m_NCS_IPC_SEND(&mbx_events, pevt,
          NCS_IPC_PRIORITY_HIGH) != NCSCC_RC_SUCCESS) {
        m_MDS_LOG_ERR("FCTRL: Failed to send msg to mbx_events, Error[%s]",
            strerror(errno));
      }
    } else if (header.msg_type_ == Intro::kIntroMsgType) {
      m_MDS_LOG_DBG("FCTRL: Receive Intro");
      // no need to decode intro message
      // the decoding intro message type is done in header decoding
      // send to the event thread
      portid_map_mutex.lock();
      Event evt(Event::Type::kEvtRcvIntro, id, 0, 0, 0, 0);
      process_flow_event(evt);
      portid_map_mutex.unlock();
    } else {
      m_MDS_LOG_ERR("FCTRL: [me] <-- [node:%x, ref:%u], "
          "[msg_type:%u], Error[not supported message type]",
          id.node, id.ref, header.msg_type_);
    }
    // return NCSCC_RC_FAILURE, so the tipc receiving thread (legacy) will
    // skip this data msg
    rc = NCSCC_RC_FAILURE;
  }

  // Let the tipc receiving thread (legacy) handle this data message
  if (is_fctrl_enabled == false) return NCSCC_RC_SUCCESS;

  if (header.IsLegacyMessage()) {
    m_MDS_LOG_DBG("FCTRL: [me] <-- [node:%x, ref:%u], "
        "Receive legacy data message, "
        "header.pro_ver:%u",
        id.node, id.ref, header.pro_ver_);
    // Receiving the first fragment legacy message, change the state to
    // kDisabled so the subsequent fragment will skip the flow control
    if ((header.mfrag_ & 0x7fff) == 1) {
      portid_map_mutex.lock();
      Event evt(Event::Type::kEvtRcvData, id, 0, 0, 0, 0);
      evt.legacy_data_ = true;
      process_flow_event(evt);
      portid_map_mutex.unlock();
    }
  }

  if (header.IsFlowMessage() || header.IsUndefinedMessage()) {
    // receive flow message explicitly identified by protocol version
    // or if it is still undefined, then consult the stateful portid
    // to proceed.
    DataMessage data;
    data.Decode(buffer);
    portid_map_mutex.lock();
    Event evt(Event::Type::kEvtRcvData, id, data.svc_id_, header.mseq_,
        header.mfrag_, header.fseq_);
    evt.snd_type_ = data.snd_type_;
    rc = process_flow_event(evt);
    if (rc == NCSCC_RC_CONTINUE) {
      process_timer_event(Event(Event::Type::kEvtTmrChunkAck));
      rc = NCSCC_RC_SUCCESS;
    }
    portid_map_mutex.unlock();
  }

  return rc;
}
