/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2008 The OpenSAF Foundation
 * (C) Copyright 2017 Ericsson AB - All Rights Reserved
 * Copyright (C) 2017, Oracle and/or its affiliates. All rights reserved.
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
..............................................................................

..............................................................................

  DESCRIPTION:

  This file contains routines for interfacing with AvD. It includes the event
  handlers for the messages received from AvD (except those that influence
  the SU SAF states).

..............................................................................

  FUNCTIONS INCLUDED in this module:


******************************************************************************
*/
#include <cinttypes>

#include "base/logtrace.h"
#include "amf/amfnd/avnd.h"
#include <algorithm>

static uint32_t avnd_node_oper_req(AVND_CB *cb, AVSV_PARAM_INFO *param) {
  uint32_t rc = NCSCC_RC_FAILURE;

  TRACE_ENTER2("'%s'", osaf_extended_name_borrow(&param->name));

  switch (param->act) {
    case AVSV_OBJ_OPR_MOD:
      switch (param->attr_id) {
        case saAmfNodeSuFailoverProb_ID:
          osafassert(sizeof(SaTimeT) == param->value_len);
          cb->su_failover_prob = m_NCS_OS_NTOHLL_P(param->value);
          break;
        case saAmfNodeSuFailoverMax_ID:
          osafassert(sizeof(uint32_t) == param->value_len);
          cb->su_failover_max = m_NCS_OS_NTOHL(*(uint32_t *)(param->value));
          break;
        default:
          LOG_NO("%s: Unsupported attribute %u", __FUNCTION__, param->attr_id);
          goto done;
      }
      break;

    default:
      LOG_NO("%s: Unsupported action %u", __FUNCTION__, param->act);
      goto done;
  }

  rc = NCSCC_RC_SUCCESS;

done:
  TRACE_LEAVE();
  return rc;
}

/**
 * Process Node admin operation request from director
 *
 * @param cb
 * @param evt
 */
static uint32_t avnd_evt_node_admin_op_req(AVND_CB *cb, AVND_EVT *evt) {
  AVSV_D2N_ADMIN_OP_REQ_MSG_INFO *info =
      &evt->info.avd->msg_info.d2n_admin_op_req_info;
  uint32_t rc = NCSCC_RC_SUCCESS;

  TRACE_ENTER2("%s op=%u", osaf_extended_name_borrow(&info->dn), info->oper_id);

  avnd_msgid_assert(info->msg_id);
  cb->rcv_msg_id = info->msg_id;

  switch (info->oper_id) {
    case SA_AMF_ADMIN_REPAIRED: {
      /* Node has been repaired. Reset states of this node and all DISABLED
       * SUs/comps. And update AMF director accordingly.
       * No need to reset states of ENABLED SUs(in case of
       * SA_AMF_NODE_SWITCHOVER, only faulty components/SU are disabled,
       * other SUs assignments are simply switched over.)
       * But for ENABLED Sus, send oper state message, so that, they
       * are evaluated for assignments.
       */
      LOG_NO("Repair request for Node'%s'", Amf::to_string(&info->dn).c_str());

      // Reset this node state first, so that when SU enabled message
      // reaches at Amfd, it starts giving assignments to those SUs.
      cb->oper_state = SA_AMF_OPERATIONAL_ENABLED;
      cb->term_state = AVND_TERM_STATE_UP;
      cb->node_err_esc_level = AVND_ERR_ESC_LEVEL_0;
      cb->failed_su = nullptr;
      cb->su_failover_cnt = 0;

      /* Reset all faulty SUs and Comps */
      for (AVND_SU *su = avnd_sudb_rec_get_next(cb->sudb, ""); su != nullptr;
          su = avnd_sudb_rec_get_next(cb->sudb, su->name)) {
        if (su->is_ncs || su->su_is_external)
          continue;
        su->comp_restart_cnt = 0;
        su->su_restart_cnt = 0;
        su->su_err_esc_level = AVND_ERR_ESC_LEVEL_0;
        if (m_AVND_SU_OPER_STATE_IS_ENABLED(su)){
          rc = avnd_di_oper_send(cb, su, 0);
          continue;
        }
        // Reset the components states.
        for (AVND_COMP *comp = m_AVND_COMP_FROM_SU_DLL_NODE_GET(
              m_NCS_DBLIST_FIND_FIRST(&su->comp_list));
            comp; comp = m_AVND_COMP_FROM_SU_DLL_NODE_GET(
              m_NCS_DBLIST_FIND_NEXT(&comp->su_dll_node))) {
          comp->admin_oper = false;
          m_AVND_COMP_STATE_RESET(comp);
          avnd_comp_pres_state_set(cb, comp, SA_AMF_PRESENCE_UNINSTANTIATED);

          m_AVND_COMP_OPER_STATE_SET(comp, SA_AMF_OPERATIONAL_ENABLED);
          avnd_di_uns32_upd_send(AVSV_SA_AMF_COMP, saAmfCompOperState_ID,
              comp->name, comp->oper);
        }

        // Reset the SUs states.
        if ((su->pres == SA_AMF_PRESENCE_TERMINATION_FAILED) &&
            (comp_in_term_failed_state() == false))
          avnd_failed_state_file_delete();

        su->admin_op_Id = static_cast<SaAmfAdminOperationIdT>(0);
        reset_suRestart_flag(su);
        m_AVND_SU_STATE_RESET(su);
        m_AVND_SU_OPER_STATE_SET(su, SA_AMF_OPERATIONAL_ENABLED);
        avnd_di_uns32_upd_send(AVSV_SA_AMF_SU, saAmfSUOperState_ID, su->name,
            su->oper);
        avnd_su_pres_state_set(cb, su, SA_AMF_PRESENCE_UNINSTANTIATED);
        rc = avnd_di_oper_send(cb, su, 0);
      } // End of for loop for su
      break;
    } // End of case SA_AMF_ADMIN_REPAIRED:

    default:
      LOG_NO("%s: unsupported adm op %u", __FUNCTION__, info->oper_id);
      rc = NCSCC_RC_FAILURE;
      break;
  }

  TRACE_LEAVE();
  return rc;
}

static uint32_t avnd_sg_oper_req(AVND_CB *cb, AVSV_PARAM_INFO *param) {
  uint32_t rc = NCSCC_RC_FAILURE;
  const std::string param_name = Amf::to_string(&param->name);

  TRACE_ENTER2("'%s'", param_name.c_str());

  switch (param->act) {
    case AVSV_OBJ_OPR_MOD: {
      AVND_SU *su;

      su = avnd_sudb_rec_get(cb->sudb, param_name);

      if (!su) {
        LOG_ER("%s: failed to get %s", __FUNCTION__, param_name.c_str());
        goto done;
      }

      switch (param->attr_id) {
        case saAmfSGCompRestartProb_ID:
          osafassert(sizeof(SaTimeT) == param->value_len);
          su->comp_restart_prob = m_NCS_OS_NTOHLL_P(param->value);
          break;
        case saAmfSGCompRestartMax_ID:
          osafassert(sizeof(uint32_t) == param->value_len);
          su->comp_restart_max = m_NCS_OS_NTOHL(*(uint32_t *)(param->value));
          break;
        case saAmfSGSuRestartProb_ID:
          osafassert(sizeof(SaTimeT) == param->value_len);
          su->su_restart_prob = m_NCS_OS_NTOHLL_P(param->value);
          break;
        case saAmfSGSuRestartMax_ID:
          osafassert(sizeof(uint32_t) == param->value_len);
          su->su_restart_max = m_NCS_OS_NTOHL(*(uint32_t *)(param->value));
          break;
        default:
          LOG_NO("%s: Unsupported attribute %u", __FUNCTION__, param->attr_id);
          goto done;
      }
      break;
    }

    default:
      LOG_NO("%s: Unsupported action %u", __FUNCTION__, param->act);
      goto done;
  }

  rc = NCSCC_RC_SUCCESS;

done:
  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_evt_avd_operation_request_msg

  Description   : This routine processes the operation request message from
                  AvD. These messages are generated in response to
                  configuration requests to AvD that modify a single object.

  Arguments     : cb  - ptr to the AvND control block
                  evt - ptr to the AvND event

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_evt_avd_operation_request_evh(AVND_CB *cb, AVND_EVT *evt) {
  AVSV_D2N_OPERATION_REQUEST_MSG_INFO *info;
  AVSV_PARAM_INFO *param;
  AVND_MSG msg;
  uint32_t rc = NCSCC_RC_SUCCESS;

  info = &evt->info.avd->msg_info.d2n_op_req;
  param = &info->param_info;

  TRACE_ENTER2("Class=%u, action=%u", param->class_id, param->act);

  /* dont process unless AvD is up */
  if (!m_AVND_CB_IS_AVD_UP(cb)) goto done;

  // TODO() hide the below code in a "set_msg_id()" function
  // If message was not broadcasted, (msg_id == 0)
  if (info->msg_id != 0) {
    avnd_msgid_assert(info->msg_id);
    cb->rcv_msg_id = info->msg_id;
  }

  switch (param->class_id) {
    case AVSV_SA_AMF_NODE:
      rc = avnd_node_oper_req(cb, param);
      break;
    case AVSV_SA_AMF_SG:
      rc = avnd_sg_oper_req(cb, param);
      break;
    case AVSV_SA_AMF_SU:
      rc = avnd_su_oper_req(cb, param);
      break;
    case AVSV_SA_AMF_COMP:
      rc = avnd_comp_oper_req(cb, param);
      break;
    case AVSV_SA_AMF_COMP_TYPE:
      rc = avnd_comptype_oper_req(cb, param);
      break;
    case AVSV_SA_AMF_HEALTH_CHECK:
      rc = avnd_hc_oper_req(cb, param);
      break;
    case AVSV_SA_AMF_HEALTH_CHECK_TYPE:
      rc = avnd_hctype_oper_req(cb, param);
      break;
    default:
      LOG_NO("%s: Unknown class ID %u", __FUNCTION__, param->class_id);
      rc = NCSCC_RC_FAILURE;
      break;
  }

  /* Send the response to avd. */
  if (info->node_id == cb->node_info.nodeId) {
    memset(&msg, 0, sizeof(AVND_MSG));
    msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));

    msg.type = AVND_MSG_AVD;
    msg.info.avd->msg_type = AVSV_N2D_OPERATION_REQUEST_MSG;
    msg.info.avd->msg_info.n2d_op_req.msg_id = ++(cb->snd_msg_id);
    msg.info.avd->msg_info.n2d_op_req.node_id = cb->node_info.nodeId;
    msg.info.avd->msg_info.n2d_op_req.param_info = *param;
    osaf_extended_name_alloc(
        osaf_extended_name_borrow(&param->name),
        &msg.info.avd->msg_info.n2d_op_req.param_info.name);
    osaf_extended_name_alloc(
        osaf_extended_name_borrow(&param->name_sec),
        &msg.info.avd->msg_info.n2d_op_req.param_info.name_sec);
    msg.info.avd->msg_info.n2d_op_req.error = rc;

    rc = avnd_di_msg_send(cb, &msg);
    if (NCSCC_RC_SUCCESS == rc)
      msg.info.avd = 0;  // TODO Mem leak?
    else
      LOG_ER("avnd_di_msg_send FAILED for msg type '%u'",
             msg.info.avd->msg_type);

    avnd_msg_content_free(cb, &msg);
  }

done:
  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : add_sisu_state_info

  Description   : This routine adds susi assignment to sisu state info message

  Arguments     : msg - ptr to message
                  si_assign - ptr to sisu assignment

  Return Values : None

  Notes         : None.
******************************************************************************/
void add_sisu_state_info(AVND_MSG *msg, SaAmfSIAssignment *si_assign) {
  AVSV_SISU_STATE_MSG *sisu_state = static_cast<AVSV_SISU_STATE_MSG *>(
      calloc(1, sizeof(AVSV_SISU_STATE_MSG)));

  osaf_extended_name_alloc(osaf_extended_name_borrow(&si_assign->su),
                           &sisu_state->safSU);
  osaf_extended_name_alloc(osaf_extended_name_borrow(&si_assign->si),
                           &sisu_state->safSI);
  sisu_state->saAmfSISUHAState = si_assign->saAmfSISUHAState;
  sisu_state->assignmentAct = si_assign->assignmentAct;
  LOG_NO("Synced SISU:%s,%s <%d, %d>",
      osaf_extended_name_borrow(&si_assign->si),
      osaf_extended_name_borrow(&si_assign->su),
      si_assign->saAmfSISUHAState, si_assign->assignmentAct);
  sisu_state->next = msg->info.avd->msg_info.n2d_nd_sisu_state_info.sisu_list;
  msg->info.avd->msg_info.n2d_nd_sisu_state_info.sisu_list = sisu_state;
  msg->info.avd->msg_info.n2d_nd_sisu_state_info.num_sisu++;
}

/****************************************************************************
  Name          : add_su_state_info

  Description   : This routine adds su info to sisu state info message

  Arguments     : msg - ptr to message
                  su - ptr to su

  Return Values : None

  Notes         : None.
******************************************************************************/
void add_su_state_info(AVND_MSG *msg, const AVND_SU *su) {
  AVSV_SU_STATE_MSG *su_state =
      static_cast<AVSV_SU_STATE_MSG *>(calloc(1, sizeof(AVSV_SU_STATE_MSG)));

  osaf_extended_name_alloc(su->name.c_str(), &su_state->safSU);
  su_state->su_restart_cnt = su->su_restart_cnt;
  su_state->su_oper_state = su->oper;
  su_state->su_pres_state = su->pres;
  LOG_NO("Synced SU:%s <%d, %d, %d>", su->name.c_str(), su->su_restart_cnt,
      su->oper, su->pres);
  su_state->next = msg->info.avd->msg_info.n2d_nd_sisu_state_info.su_list;
  msg->info.avd->msg_info.n2d_nd_sisu_state_info.su_list = su_state;
  msg->info.avd->msg_info.n2d_nd_sisu_state_info.num_su++;
}

/****************************************************************************
  Name          : add_csicomp_state_info

  Description   : This routine adds csi assignment info to csi comp state info
                  message

  Arguments     : msg - ptr to message
                  csi_assign - ptr to csi assignment

  Return Values : None

  Notes         : None.
******************************************************************************/
void add_csicomp_state_info(AVND_MSG *msg, SaAmfCSIAssignment *csi_assign) {
  AVSV_CSICOMP_STATE_MSG *csicomp_state = static_cast<AVSV_CSICOMP_STATE_MSG *>(
      calloc(1, sizeof(AVSV_CSICOMP_STATE_MSG)));

  osaf_extended_name_alloc(osaf_extended_name_borrow(&csi_assign->csi),
                           &csicomp_state->safCSI);
  osaf_extended_name_alloc(osaf_extended_name_borrow(&csi_assign->comp),
                           &csicomp_state->safComp);
  csicomp_state->saAmfCSICompHAState = csi_assign->saAmfCSICompHAState;

  csicomp_state->next =
      msg->info.avd->msg_info.n2d_nd_csicomp_state_info.csicomp_list;
  msg->info.avd->msg_info.n2d_nd_csicomp_state_info.csicomp_list =
      csicomp_state;
  msg->info.avd->msg_info.n2d_nd_csicomp_state_info.num_csicomp++;
}

/****************************************************************************
  Name          : add_comp_state_info

  Description   : This routine adds csi assignment info to comp state info
                  message

  Arguments     : msg - ptr to message
                  comp - ptr to comp

  Return Values : None

  Notes         : None.
******************************************************************************/
void add_comp_state_info(AVND_MSG *msg, const AVND_COMP *comp) {
  AVSV_COMP_STATE_MSG *comp_state = static_cast<AVSV_COMP_STATE_MSG *>(
      calloc(1, sizeof(AVSV_COMP_STATE_MSG)));

  osaf_extended_name_alloc(comp->name.c_str(), &comp_state->safComp);
  comp_state->comp_restart_cnt = comp->err_info.restart_cnt;
  comp_state->comp_oper_state = comp->oper;
  comp_state->comp_pres_state = comp->pres;

  comp_state->next =
      msg->info.avd->msg_info.n2d_nd_csicomp_state_info.comp_list;
  msg->info.avd->msg_info.n2d_nd_csicomp_state_info.comp_list = comp_state;
  msg->info.avd->msg_info.n2d_nd_csicomp_state_info.num_comp++;
}

/****************************************************************************
  Name          : avnd_evt_avd_ack_message

  Description   : This routine processes Ack message
                  response from AvD.

  Arguments     : cb  - ptr to the AvND control block
                  evt - ptr to the AvND event

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_evt_avd_ack_evh(AVND_CB *cb, AVND_EVT *evt) {
  uint32_t rc = NCSCC_RC_SUCCESS;

  TRACE_ENTER();

  /* dont process unless AvD is up */
  if (!m_AVND_CB_IS_AVD_UP(cb)) goto done;

  /* process the ack response */
  avnd_di_msg_ack_process(cb, evt->info.avd->msg_info.d2n_ack_info.msg_id_ack);

done:
  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_evt_tmr_rcv_msg_rsp

  Description   : This routine handles the expiry of the msg response timer.
                  It resends the message if the retry count is not already
                  exceeded.

  Arguments     : cb  - ptr to the AvND control block
                  evt - ptr to the AvND event

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_evt_tmr_rcv_msg_rsp_evh(AVND_CB *cb, AVND_EVT *evt) {
  AVND_TMR_EVT *tmr = &evt->info.tmr;
  AVND_DND_MSG_LIST *rec = 0;
  uint32_t rc = NCSCC_RC_SUCCESS;
  bool rec_tobe_deleted = false;

  TRACE_ENTER();

  /* retrieve the message record */
  if ((0 == (rec = (AVND_DND_MSG_LIST *)ncshm_take_hdl(NCS_SERVICE_ID_AVND,
                                                       tmr->opq_hdl))))
    goto done;

  /* Resend on time out if it's NODE_UP msg only */
  if (rec->msg.info.avd->msg_type == AVSV_N2D_NODE_UP_MSG) {
    rc = avnd_diq_rec_send(cb, rec);
  } else if (rec->msg.info.avd->msg_type == AVSV_N2D_NODE_DOWN_MSG) {
    if (rec->no_retries < AVND_NODE_DOWN_MAX_RETRY) {
      rc = avnd_diq_rec_send(cb, rec);
    } else {
      LOG_WA("Node Down timer retries is over");
      avnd_last_step_clean(cb);
      rec_tobe_deleted = true;
    }
  } else {
    LOG_WA("Unexpected message response timeout with msg_type(%u)", rec->msg.info.avd->msg_type);
    rec_tobe_deleted = true;
  }

  ncshm_give_hdl(tmr->opq_hdl);
  if (rec_tobe_deleted) {
    auto position = std::find(cb->dnd_list.begin(), cb->dnd_list.end(), rec);
    osafassert(position != cb->dnd_list.end());
    cb->dnd_list.erase(position);
    avnd_diq_rec_del(cb, rec);
  }
done:
  TRACE_LEAVE();
  return rc;
}

void avnd_send_node_up_msg(void) {
  AVND_CB *cb = avnd_cb;
  AVND_MSG msg = {};
  uint32_t rc;

  TRACE_ENTER();

  if (cb->node_info.member != true) {
    TRACE("not member");
    goto done;
  }

  if (!m_AVND_CB_IS_AVD_UP(avnd_cb)) {
    TRACE("AVD not up");
    goto done;
  }

  // We don't send node_up if it has already been sent and waiting for ACK
  for (auto pending_rec : cb->dnd_list) {
    if (pending_rec->msg.info.avd->msg_type == AVSV_N2D_NODE_UP_MSG) {
      TRACE(
          "Don't send another node_up since it has been sent and waiting for ack");
      goto done;
    }
  }

  msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));
  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type = AVSV_N2D_NODE_UP_MSG;
  msg.info.avd->msg_info.n2d_node_up.msg_id = ++(cb->snd_msg_id);
  msg.info.avd->msg_info.n2d_node_up.leds_set = cb->is_ncs_su_assigned;
  osaf_extended_name_alloc(cb->amf_nodeName.c_str(),
                           &msg.info.avd->msg_info.n2d_node_up.node_name);
  msg.info.avd->msg_info.n2d_node_up.node_id = cb->node_info.nodeId;
  msg.info.avd->msg_info.n2d_node_up.adest_address = cb->avnd_dest;

  rc = avnd_di_msg_send(cb, &msg);
  if (NCSCC_RC_SUCCESS == rc) msg.info.avd = 0;

  avnd_msg_content_free(cb, &msg);

done:
  TRACE_LEAVE();
}

/*
 * @brief      Sends message to AMFA about SCs Absence and Presence.
 * @param [in]  status
 */
void avnd_send_sc_status_message(OsafAmfSCStatusT status) {
  TRACE_ENTER();
  uint32_t rc = NCSCC_RC_SUCCESS;
  AVND_MSG msg;
  MDS_DEST mds_dest = 0;
  memset(&msg, 0, sizeof(AVND_MSG));

  for (auto& it: agent_mds_ver_db) {
    if (it.second < AVSV_AVND_AVA_MSG_FMT_VER_3) {
      TRACE("AMFA does not support the callback");
      continue;
    }
    msg.info.ava =
      static_cast<AVSV_NDA_AVA_MSG *>(calloc(1, sizeof(AVSV_NDA_AVA_MSG)));
    msg.type = AVND_MSG_AVA;
    msg.info.ava->type = AVSV_AVND_AMF_CBK_MSG;
    msg.info.ava->info.cbk_info = static_cast<AVSV_AMF_CBK_INFO*>(calloc(1, sizeof(AVSV_AMF_CBK_INFO)));
    msg.info.ava->info.cbk_info->param.sc_status_change.sc_status = status;
    msg.info.ava->info.cbk_info->type = AVSV_AMF_SC_STATUS_CHANGE;
    mds_dest = it.first;
    rc = avnd_mds_send(avnd_cb, &msg, &mds_dest, 0);
    if (rc != NCSCC_RC_SUCCESS)
      LOG_WA("Message failed to dest");
    avnd_msg_content_free(avnd_cb, &msg);
  }
  TRACE_LEAVE();
}
/****************************************************************************
  Name          : avnd_evt_mds_avd_up

  Description   : This routine processes the AvD up event from MDS. It sends
                  the node-up message to AvD. it also starts AvD supervision
                  we are on a controller and the up event is for our node.

  Arguments     : cb  - ptr to the AvND control block
                  evt - ptr to the AvND event

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_evt_mds_avd_up_evh(AVND_CB *cb, AVND_EVT *evt) {
  TRACE_ENTER2("%" PRIx64, evt->info.mds.mds_dest);

  /* If AvD UP event has come just after AvD DOWN for Act controller, then it is
     a case of TIPC flicker */

  if ((m_MDS_DEST_IS_AN_ADEST(evt->info.mds.mds_dest) &&
       avnd_cb->cont_reboot_in_progress) &&
      (evt->info.mds.mds_dest == cb->active_avd_adest)) {
    cb->reboot_in_progress = true;
    opensaf_reboot(
        avnd_cb->node_info.nodeId,
        osaf_extended_name_borrow(&avnd_cb->node_info.executionEnvironment),
        "Link reset with Act controller");
    goto done;
  }

  /* Validate whether this is a ADEST or VDEST */
  if (m_MDS_DEST_IS_AN_ADEST(evt->info.mds.mds_dest)) {
    NCS_NODE_ID node_id, my_node_id = ncs_get_node_id();
    node_id = m_NCS_NODE_ID_FROM_MDS_DEST(evt->info.mds.mds_dest);

    if (node_id == my_node_id) {
      TRACE("Starting hb supervision of local avd");
      avnd_stop_tmr(cb, &cb->hb_duration_tmr);
      avnd_start_tmr(cb, &cb->hb_duration_tmr, AVND_TMR_HB_DURATION,
                     cb->hb_duration, 0);
    }
  } else {
    /* Avd is already UP, reboot the node */
    if (m_AVND_CB_IS_AVD_UP(cb)) {
      opensaf_reboot(
          avnd_cb->node_info.nodeId,
          osaf_extended_name_borrow(&avnd_cb->node_info.executionEnvironment),
          "AVD already up");
      goto done;
    }

    m_AVND_CB_AVD_UP_SET(cb);

    /* store the AVD MDS address */
    cb->avd_dest = evt->info.mds.mds_dest;

    /* amfnd receives NCSMDS_UP in either cluster start up; or recovery from
     * headless after a long gap greater than no-active timer in MDS. We send
     * node_up in both cases but only sync info is sent for recovery
     */
    if (evt->info.mds.i_change == NCSMDS_UP) {
      if (cb->is_avd_down && cb->is_ncs_su_assigned) {
        avnd_sync_sisu(cb);
        avnd_sync_csicomp(cb);
      }

      LOG_NO("Sending node up due to NCSMDS_UP");
      avnd_send_node_up_msg();
    }
    /* amfnd receives NCSMDS_NEW_ACTIVE in either Failover; or recovery from
     * headless provided that the no-active timer in MDS has not expired. We
     * only want to send node_up/sync info in case of recovery.
     */
    if (evt->info.mds.i_change == NCSMDS_NEW_ACTIVE && cb->is_avd_down) {
      if (cb->is_ncs_su_assigned) {
        // node_up, sync sisu, compcsi info to AVND for recovery
        avnd_sync_sisu(cb);
        avnd_sync_csicomp(cb);
      }
      LOG_NO("Sending node up due to NCSMDS_NEW_ACTIVE");
      avnd_send_node_up_msg();
    }
    cb->is_avd_down = false;

    //Inform AMFA about SCs presence now.
    avnd_send_sc_status_message(OSAF_AMF_SC_PRESENT);
  }

  if (m_AVND_TMR_IS_ACTIVE(cb->sc_absence_tmr))
    avnd_stop_tmr(cb, &cb->sc_absence_tmr);
done:
  TRACE_LEAVE();
  return NCSCC_RC_SUCCESS;
}

/****************************************************************************
  Name          : avnd_evt_mds_avd_dn

  Description   : This routine processes the AvD down event from MDS.

  Arguments     : cb  - ptr to the AvND control block
                  evt - ptr to the AvND event

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : What should be done? TBD.
******************************************************************************/
uint32_t avnd_evt_mds_avd_dn_evh(AVND_CB *cb, AVND_EVT *evt) {
  uint32_t rc = NCSCC_RC_SUCCESS;
  AVND_SU *su;

  TRACE_ENTER();

  if (evt->info.mds.i_change == NCSMDS_NO_ACTIVE) {
    m_AVND_CB_AVD_UP_RESET(cb);
    TRACE_LEAVE();
    return rc;
  }

  if (m_MDS_DEST_IS_AN_ADEST(evt->info.mds.mds_dest)) {
    if (evt->info.mds.node_id != ncs_get_node_id()) {
      /* Ignore the other AVD Adest Down.*/
      if (evt->info.mds.mds_dest == cb->active_avd_adest)
        avnd_cb->cont_reboot_in_progress = true;
      TRACE_LEAVE();
      return rc;
    }
  }

  // Ignore the second NCSMDS_DOWN which comes from timeout of
  // MDS_AWAIT_ACTIVE_TMR_VAL
  if (cb->is_avd_down == true) {
    TRACE_LEAVE();
    return rc;
  }

  m_AVND_CB_AVD_UP_RESET(cb);
  cb->active_avd_adest = 0;

  LOG_WA("AMF director unexpectedly crashed");

  if (m_AVND_IS_SHUTTING_DOWN(cb)) {
    LOG_WA("Ignore because AMFND is in SHUTDOWN state");
    TRACE_LEAVE();
    return rc;
  }

  // if headless is disabled OR if the amfd down came from the local node, just
  // reboot
  if (cb->scs_absence_max_duration == 0 ||
      evt->info.mds.node_id == ncs_get_node_id()) {
    /* Don't issue reboot if it has been already issued.*/
    if (false == cb->reboot_in_progress) {
      cb->reboot_in_progress = true;
      opensaf_reboot(
          avnd_cb->node_info.nodeId,
          osaf_extended_name_borrow(&avnd_cb->node_info.executionEnvironment),
          "local AVD down(Adest) or both AVD down(Vdest) received");
    }

    TRACE_LEAVE();
    return rc;
  }

  /*
   * No contact with any controller
   * Reboot this node if:
   * 1) director is of an older version that does not support restart
   * 2) we have a pending message TO the director
   * 3) we have a pending message FROM the director
   */

  if (cb->scs_absence_max_duration == 0) {
    // check for pending messages TO director
    if ((cb->dnd_list.empty() == false)) {
      uint32_t no_pending_msg = 0;
      for (auto rec : cb->dnd_list) {
        osafassert(rec->msg.type == AVND_MSG_AVD);
        no_pending_msg++;
      }

      /* Don't issue reboot if it has been already issued.*/
      if (false == cb->reboot_in_progress) {
        LOG_ER("%d pending messages to director. Rebooting to re-sync.",
               no_pending_msg);

        cb->reboot_in_progress = true;
        opensaf_reboot(
            avnd_cb->node_info.nodeId,
            osaf_extended_name_borrow(&avnd_cb->node_info.executionEnvironment),
            "local AVD down(Adest) or both AVD down(Vdest) received");
      }
    }
  } else {
    TRACE("Delete/Buffer pending messages to be sent to AMFD");
    avnd_diq_rec_check_buffered_msg(cb);
  }

  // check for pending messages FROM director
  // scan all SUs "siq" message list, if anyone is not empty reboot
  for (su = avnd_sudb_rec_get_next(cb->sudb, ""); su != nullptr;
       su = avnd_sudb_rec_get_next(cb->sudb, su->name)) {
    LOG_NO("Checking '%s' for pending messages", su->name.c_str());

    const AVND_SU_SIQ_REC *siq =
        (AVND_SU_SIQ_REC *)m_NCS_DBLIST_FIND_LAST(&su->siq);

    if (siq != nullptr) {
      /* Don't issue reboot if it has been already issued.*/
      if (false == cb->reboot_in_progress) {
        LOG_ER("Pending messages from director. Rebooting to re-sync.");

        cb->reboot_in_progress = true;
        opensaf_reboot(
            avnd_cb->node_info.nodeId,
            osaf_extended_name_borrow(&avnd_cb->node_info.executionEnvironment),
            "local AVD down(Adest) or both AVD down(Vdest) received");
      }
    }
  }
  // record we are now 'headless'
  cb->is_avd_down = true;
  cb->amfd_sync_required = true;
  // start the sc absence timer if it hasn't started.
  // During headless, MDS reports avd_down 2 times,
  // the 2nd time is 3 mins later then the 1st time.
  // The absence timer should only start at the 1st time.
  if (!m_AVND_TMR_IS_ACTIVE(cb->sc_absence_tmr)) {
    avnd_start_tmr(cb, &cb->sc_absence_tmr, AVND_TMR_SC_ABSENCE,
                   cb->scs_absence_max_duration, 0);
  }

  // reset msg_id counter
  cb->rcv_msg_id = 0;
  cb->snd_msg_id = 0;
  cb->active_ack_msg_id = 0;

  //Inform AMFA about SCs absence now.
  avnd_send_sc_status_message(OSAF_AMF_SC_ABSENT);

  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_di_oper_send

  Description   : This routine sends the operational state of the node & the
                  service unit to AvD.

  Arguments     : cb   - ptr to the AvND control block
                  su   - ptr to the su
                  rcvr - recommended recovery

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_di_oper_send(AVND_CB *cb, const AVND_SU *su, uint32_t rcvr) {
  AVND_MSG msg;
  uint32_t rc = NCSCC_RC_SUCCESS;

  memset(&msg, 0, sizeof(AVND_MSG));
  TRACE_ENTER2("SU '%p', recv '%u'", su, rcvr);

  /* populate the oper msg */
  msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));
  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type = AVSV_N2D_OPERATION_STATE_MSG;
  msg.info.avd->msg_info.n2d_opr_state.node_id = cb->node_info.nodeId;
  msg.info.avd->msg_info.n2d_opr_state.node_oper_state = cb->oper_state;

  if (su) {
    osaf_extended_name_alloc(su->name.c_str(),
                             &msg.info.avd->msg_info.n2d_opr_state.su_name);
    msg.info.avd->msg_info.n2d_opr_state.su_oper_state = su->oper;
    TRACE("SU '%s, su oper '%u'", su->name.c_str(), su->oper);

    if ((su->is_ncs == true) && (su->oper == SA_AMF_OPERATIONAL_DISABLED))
      TRACE("%s SU Oper state got disabled", su->name.c_str());
  }

  msg.info.avd->msg_info.n2d_opr_state.rec_rcvr.raw = rcvr;

  if (cb->is_avd_down == true || cb->amfd_sync_required == true) {
    if (msg.info.avd->msg_info.n2d_opr_state.su_oper_state ==
        SA_AMF_OPERATIONAL_DISABLED ||
        msg.info.avd->msg_info.n2d_opr_state.node_oper_state ==
            SA_AMF_OPERATIONAL_DISABLED) {
      // We are in headless, buffer this msg
      msg.info.avd->msg_info.n2d_opr_state.msg_id = 0;
      if (avnd_diq_rec_add(cb, &msg) == nullptr) {
        rc = NCSCC_RC_FAILURE;
      }
      LOG_NO(
          "avnd_di_oper_send() deferred as AMF director is offline(%d),"
          " or sync is required(%d)",
          cb->is_avd_down, cb->amfd_sync_required);
    }
  } else {
    // We are in normal cluster, send msg to director
    msg.info.avd->msg_info.n2d_opr_state.msg_id = ++(cb->snd_msg_id);
    /* send the msg to AvD */
    rc = avnd_di_msg_send(cb, &msg);
    if (NCSCC_RC_SUCCESS == rc) msg.info.avd = 0;
  }

  /* free the contents of avnd message */
  avnd_msg_content_free(cb, &msg);

  TRACE_LEAVE2("rc '%u'", rc);
  return rc;
}

/*
 * @brief      Check if all SI assignments for this SU are in stable state.
 *
 * @param [in]  ptr to SU
 *
 * @returns     true/false
 */

static bool su_assign_state_is_stable(const AVND_SU *su) {
  AVND_SU_SI_REC *si;

  for (si = (AVND_SU_SI_REC *)m_NCS_DBLIST_FIND_FIRST(&su->si_list); si;
       si = (AVND_SU_SI_REC *)m_NCS_DBLIST_FIND_NEXT(&si->su_dll_node)) {
    if (m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_ASSIGNING(si) ||
        m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_REMOVING(si)) {
      return false;
    }
  }

  return true;
}

/****************************************************************************
  Name          : avnd_di_susi_resp_send

  Description   : This routine sends the SU-SI assignment/removal response to
                  AvD.

  Arguments     : cb - ptr to the AvND control block
                  su - ptr to the su
                  si - ptr to SI record (if 0, response is meant for all the
                       SIs that belong to the SU)

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_di_susi_resp_send(AVND_CB *cb, AVND_SU *su, AVND_SU_SI_REC *si) {
  AVND_SU_SI_REC *curr_si = 0;
  AVND_MSG msg;
  uint32_t rc = NCSCC_RC_SUCCESS;

  memset(&msg, 0, sizeof(AVND_MSG));

  if (cb->term_state == AVND_TERM_STATE_OPENSAF_SHUTDOWN_STARTED) return rc;

  if (m_AVND_SU_OPER_STATE_IS_ENABLED(su))
    // should be in assignment pending state to be here
    osafassert(m_AVND_SU_IS_ASSIGN_PEND(su));

  /* get the curr-si */
  curr_si = (si) ? si : (AVND_SU_SI_REC *)m_NCS_DBLIST_FIND_FIRST(&su->si_list);
  osafassert(curr_si);

  TRACE_ENTER2("Sending Resp su=%s, si=%s, curr_state=%u, prv_state=%u",
               su->name.c_str(), curr_si->name.c_str(), curr_si->curr_state,
               curr_si->prv_state);
  /* populate the susi resp msg */
  msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));
  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type = AVSV_N2D_INFO_SU_SI_ASSIGN_MSG;
  msg.info.avd->msg_info.n2d_su_si_assign.node_id = cb->node_info.nodeId;
  if (si) {
    msg.info.avd->msg_info.n2d_su_si_assign.single_csi =
        ((si->single_csi_add_rem_in_si == AVSV_SUSI_ACT_BASE) ? false : true);
  }
  TRACE("curr_assign_state '%u'", curr_si->curr_assign_state);
  msg.info.avd->msg_info.n2d_su_si_assign.msg_act =
      (m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_ASSIGNED(curr_si) ||
       m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_ASSIGNING(curr_si))
          ? ((!curr_si->prv_state) ? AVSV_SUSI_ACT_ASGN : AVSV_SUSI_ACT_MOD)
          : AVSV_SUSI_ACT_DEL;
  osaf_extended_name_alloc(su->name.c_str(),
                           &msg.info.avd->msg_info.n2d_su_si_assign.su_name);
  if (si) {
    osaf_extended_name_alloc(si->name.c_str(),
                             &msg.info.avd->msg_info.n2d_su_si_assign.si_name);
    if (AVSV_SUSI_ACT_ASGN == si->single_csi_add_rem_in_si) {
      TRACE("si->curr_assign_state '%u'", curr_si->curr_assign_state);
      msg.info.avd->msg_info.n2d_su_si_assign.msg_act =
          (m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_ASSIGNED(curr_si) ||
           m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_ASSIGNING(curr_si))
              ? AVSV_SUSI_ACT_ASGN
              : AVSV_SUSI_ACT_DEL;
    }
  }
  msg.info.avd->msg_info.n2d_su_si_assign.ha_state =
      (SA_AMF_HA_QUIESCING == curr_si->curr_state) ? SA_AMF_HA_QUIESCED
                                                   : curr_si->curr_state;
  msg.info.avd->msg_info.n2d_su_si_assign.error =
      (m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_ASSIGNED(curr_si) ||
       m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_REMOVED(curr_si))
          ? NCSCC_RC_SUCCESS
          : NCSCC_RC_FAILURE;

  if (msg.info.avd->msg_info.n2d_su_si_assign.msg_act == AVSV_SUSI_ACT_ASGN)
    osafassert(si);

  /* send the msg to AvD */
  TRACE(
      "Sending. msg_id'%u', node_id'%x', msg_act'%u', su'%s', si'%s', ha_state'%u', error'%u', single_csi'%u'",
      msg.info.avd->msg_info.n2d_su_si_assign.msg_id,
      msg.info.avd->msg_info.n2d_su_si_assign.node_id,
      msg.info.avd->msg_info.n2d_su_si_assign.msg_act,
      osaf_extended_name_borrow(
          &msg.info.avd->msg_info.n2d_su_si_assign.su_name),
      osaf_extended_name_borrow(
          &msg.info.avd->msg_info.n2d_su_si_assign.si_name),
      msg.info.avd->msg_info.n2d_su_si_assign.ha_state,
      msg.info.avd->msg_info.n2d_su_si_assign.error,
      msg.info.avd->msg_info.n2d_su_si_assign.single_csi);

  if ((su->si_list.n_nodes > 1) && (si == nullptr)) {
    if (msg.info.avd->msg_info.n2d_su_si_assign.msg_act == AVSV_SUSI_ACT_DEL)
      LOG_NO("Removed 'all SIs' from '%s'", su->name.c_str());

    if (msg.info.avd->msg_info.n2d_su_si_assign.msg_act == AVSV_SUSI_ACT_MOD)
      LOG_NO("Assigned 'all SIs' %s of '%s'",
             ha_state[msg.info.avd->msg_info.n2d_su_si_assign.ha_state],
             su->name.c_str());
  }

  if (cb->is_avd_down == true ||
      (cb->amfd_sync_required == true && su->is_ncs == false)) {
    // We are in headless, buffer this msg
    msg.info.avd->msg_info.n2d_su_si_assign.msg_id = 0;
    if (avnd_diq_rec_add(cb, &msg) == nullptr) {
      rc = NCSCC_RC_FAILURE;
    }
    m_AVND_SU_ALL_SI_RESET(su);
    LOG_NO(
        "avnd_di_susi_resp_send() deferred as AMF director is offline(%d),"
        " or sync is required(%d)",
        cb->is_avd_down, cb->amfd_sync_required);

  } else {
    // We are in normal cluster, send msg to director
    msg.info.avd->msg_info.n2d_su_si_assign.msg_id = ++(cb->snd_msg_id);
    /* send the msg to AvD */
    rc = avnd_di_msg_send(cb, &msg);
    if (NCSCC_RC_SUCCESS == rc) msg.info.avd = 0;
    /* we have completed the SU SI msg processing */
    if (su_assign_state_is_stable(su)) {
      m_AVND_SU_ASSIGN_PEND_RESET(su);
    }
    m_AVND_SU_ALL_SI_RESET(su);
  }

  /* free the contents of avnd message */
  avnd_msg_content_free(cb, &msg);

  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_di_object_upd_send

  Description   : This routine sends update of those objects that reside
                  on AvD but are maintained on AvND.

  Arguments     : cb    - ptr to the AvND control block
                  param - ptr to the params that are to be updated to AvD

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_di_object_upd_send(AVND_CB *cb, AVSV_PARAM_INFO *param) {
  AVND_MSG msg;
  uint32_t rc = NCSCC_RC_SUCCESS;
  TRACE_ENTER2("Comp '%s'", osaf_extended_name_borrow(&param->name));

  if (cb->is_avd_down == true) {
    TRACE_LEAVE2("AVD is down. %u", rc);
    return rc;
  }


  /* populate the msg */
  memset(&msg, 0, sizeof(AVND_MSG));
  msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));
  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type = AVSV_N2D_DATA_REQUEST_MSG;
  msg.info.avd->msg_info.n2d_data_req.node_id = cb->node_info.nodeId;
  msg.info.avd->msg_info.n2d_data_req.param_info = *param;

  if ((cb->is_avd_down == false) && (cb->amfd_sync_required == true)) {
    msg.info.avd->msg_info.n2d_data_req.msg_id = 0;
    if (avnd_diq_rec_add(cb, &msg) == nullptr) {
      /* free the contents of avnd message */
      avnd_msg_content_free(cb, &msg);
      rc = NCSCC_RC_FAILURE;
    }
    LOG_NO(
        "avnd_di_object_upd_send() deferred as AMF director is offline(%d),"
        " or sync is required(%d)",
        cb->is_avd_down, cb->amfd_sync_required);

    TRACE_LEAVE2("AVD is down. %u", rc);
    return rc;
  } else {
    msg.info.avd->msg_info.n2d_data_req.msg_id = ++(cb->snd_msg_id);
  }

  /* send the msg to AvD */
  rc = avnd_di_msg_send(cb, &msg);
  if (NCSCC_RC_SUCCESS == rc) msg.info.avd = 0;

  /* free the contents of avnd message */
  avnd_msg_content_free(cb, &msg);
  TRACE_LEAVE2("%u", rc);
  return rc;
}

/**
 * Send update message for an uint32_t value
 * @param class_id - as specified in avsv_def.h
 * @param attr_id - as specified in avsv_def.h
 * @param dn - dn of object to update
 * @param value - ptr to uint32_t value to be sent
 *
 */
void avnd_di_uns32_upd_send(int class_id, int attr_id, const std::string &dn,
                            uint32_t value) {
  AVSV_PARAM_INFO param = {0};

  param.class_id = class_id;
  param.attr_id = attr_id;
  osaf_extended_name_alloc(dn.c_str(), &param.name);
  param.act = AVSV_OBJ_OPR_MOD;
  *((uint32_t *)param.value) = m_NCS_OS_HTONL(value);
  param.value_len = sizeof(uint32_t);

  if (avnd_di_object_upd_send(avnd_cb, &param) != NCSCC_RC_SUCCESS)
    LOG_WA("Could not send update msg for '%s'", dn.c_str());
}

/****************************************************************************
  Name          : avnd_di_pg_act_send

  Description   : This routine sends pg track start and stop requests to AvD.

  Arguments     : cb           - ptr to the AvND control block
                  csi_name     - ptr to the csi-name
                  act          - req action (start/stop)
                  fover        - true if the message being sent on fail-over.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_di_pg_act_send(AVND_CB *cb, const std::string &csi_name,
                             AVSV_PG_TRACK_ACT actn, bool fover) {
  AVND_MSG msg;
  TRACE_ENTER2("Csi '%s'", csi_name.c_str());

  memset(&msg, 0, sizeof(AVND_MSG));

  /* populate the msg */
  msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));
  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type = AVSV_N2D_PG_TRACK_ACT_MSG;
  msg.info.avd->msg_info.n2d_pg_trk_act.msg_id = ++(cb->snd_msg_id);
  msg.info.avd->msg_info.n2d_pg_trk_act.node_id = cb->node_info.nodeId;
  osaf_extended_name_alloc(csi_name.c_str(),
                           &msg.info.avd->msg_info.n2d_pg_trk_act.csi_name);
  msg.info.avd->msg_info.n2d_pg_trk_act.actn = actn;
  msg.info.avd->msg_info.n2d_pg_trk_act.msg_on_fover = fover;

  /* send the msg to AvD */
  uint32_t rc = avnd_di_msg_send(cb, &msg);
  if (NCSCC_RC_SUCCESS == rc) msg.info.avd = 0;

  /* free the contents of avnd message */
  avnd_msg_content_free(cb, &msg);
  TRACE_LEAVE2("rc, '%u'", rc);
  return rc;
}

/****************************************************************************
  Name          : avnd_di_msg_send

  Description   : This routine sends the message to AvD.

  Arguments     : cb  - ptr to the AvND control block
                  msg - ptr to the message

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.

  Notes         : None
******************************************************************************/
uint32_t avnd_di_msg_send(AVND_CB *cb, AVND_MSG *msg) {
  AVND_DND_MSG_LIST *rec = 0;
  uint32_t rc = NCSCC_RC_SUCCESS;
  TRACE_ENTER();
  /* nothing to send */
  if (!msg) goto done;

  TRACE("Msg type '%u'", msg->info.avd->msg_type);

  /* Verify Ack nack msgs are not buffered */
  if (m_AVSV_N2D_MSG_IS_VER_ACK_NACK(msg->info.avd) ||
      m_AVSV_N2D_MSG_IS_PG_TRACK_ACT(msg->info.avd)) {
    /*send the response to active AvD (In case MDS has not updated its
       tables by this time) */
    TRACE_1("%s, Active AVD Adest: %" PRIu64, __FUNCTION__,
            cb->active_avd_adest);
    rc = avnd_mds_red_send(cb, msg, &cb->avd_dest, &cb->active_avd_adest);
    goto done;
  } else if ((msg->info.avd->msg_type == AVSV_N2D_ND_SISU_STATE_INFO_MSG) ||
             (msg->info.avd->msg_type == AVSV_N2D_ND_CSICOMP_STATE_INFO_MSG)) {
    rc = avnd_mds_send(cb, msg, &cb->avd_dest, 0);
    goto done;
  }

  /* add the record to the AvD msg list */
  if ((0 != (rec = avnd_diq_rec_add(cb, msg)))) {
    /* send the message */
    avnd_diq_rec_send(cb, rec);
  } else
    rc = NCSCC_RC_FAILURE;

done:
  if (NCSCC_RC_SUCCESS != rc && rec) {
    /* pop & delete */
    auto position = std::find(cb->dnd_list.begin(), cb->dnd_list.end(), rec);
    osafassert(position != cb->dnd_list.end());
    cb->dnd_list.erase(position);
    avnd_diq_rec_del(cb, rec);
  }
  TRACE_LEAVE2("%u", rc);
  return rc;
}

/****************************************************************************
  Name          : avnd_di_ack_nack_msg_send

  Description   : This routine processes the data verify message sent by newly
                  Active AVD.

  Arguments     : cb  - ptr to the AvND control block
                  rcv_id - Receive message ID for AVND
                  view_num - Cluster view number

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_di_ack_nack_msg_send(AVND_CB *cb, uint32_t rcv_id,
                                   uint32_t view_num) {
  AVND_MSG msg;
  uint32_t rc = NCSCC_RC_SUCCESS;
  TRACE_ENTER2("Receive id = %u", rcv_id);

  /*** send the response to AvD ***/
  memset(&msg, 0, sizeof(AVND_MSG));

  msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));
  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type = AVSV_N2D_VERIFY_ACK_NACK_MSG;
  msg.info.avd->msg_info.n2d_ack_nack_info.msg_id = (cb->snd_msg_id + 1);
  msg.info.avd->msg_info.n2d_ack_nack_info.node_id = cb->node_info.nodeId;

  if (rcv_id != cb->rcv_msg_id) {
    LOG_WA("Mismatch msg id, AVD send ID count: %u, "
          "AVND receive ID count: %u", rcv_id, cb->rcv_msg_id);
    // During SC failover, message sent on ACTIVE AMFD can not
    // be checked point to AMFD on STANDBY SC. But the AMFND still
    // receive msg id. STANDBY SC takes ACTIVE and mismatch message
    // id b/w AMFD and AMFND on new ACTIVE. In this case AVND receive
    // ID count greater than AVD sent id count. Shoudl rsp ack(true).
    if (cb->rcv_msg_id > rcv_id) {
      cb->rcv_msg_id = rcv_id;
      msg.info.avd->msg_info.n2d_ack_nack_info.ack = true;
    } else {
      msg.info.avd->msg_info.n2d_ack_nack_info.ack = false;
    }
  } else {
    msg.info.avd->msg_info.n2d_ack_nack_info.ack = true;
  }

  TRACE_1("MsgId=%u,ACK=%u", msg.info.avd->msg_info.n2d_ack_nack_info.msg_id,
          msg.info.avd->msg_info.n2d_ack_nack_info.ack);

  rc = avnd_di_msg_send(cb, &msg);
  if (NCSCC_RC_SUCCESS == rc) msg.info.avd = 0;

  /* free the contents of avnd message */
  avnd_msg_content_free(cb, &msg);

  TRACE_LEAVE2("retval=%u", rc);
  return rc;
}

/****************************************************************************
  Name          : avnd_di_reg_su_rsp_snd

  Description   : This routine sends response to register SU message.

  Arguments     : cb  - ptr to the AvND control block
                  su_name - SU name.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_di_reg_su_rsp_snd(AVND_CB *cb, const std::string &su_name,
                                uint32_t ret_code) {
  AVND_MSG msg;
  uint32_t rc = NCSCC_RC_SUCCESS;

  memset(&msg, 0, sizeof(AVND_MSG));
  msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));
  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type = AVSV_N2D_REG_SU_MSG;
  msg.info.avd->msg_info.n2d_reg_su.msg_id = ++(cb->snd_msg_id);
  msg.info.avd->msg_info.n2d_reg_su.node_id = cb->node_info.nodeId;
  osaf_extended_name_alloc(su_name.c_str(),
                           &msg.info.avd->msg_info.n2d_reg_su.su_name);
  msg.info.avd->msg_info.n2d_reg_su.error =
      (NCSCC_RC_SUCCESS == ret_code) ? NCSCC_RC_SUCCESS : NCSCC_RC_FAILURE;

  rc = avnd_di_msg_send(cb, &msg);
  if (NCSCC_RC_SUCCESS == rc) msg.info.avd = 0;

  /* free the contents of avnd message */
  avnd_msg_content_free(cb, &msg);

  return NCSCC_RC_SUCCESS;
}
/****************************************************************************
  Name          : avnd_di_node_down_msg_send

  Description   : This routine sends node_down message to active amf director.

  Arguments     : cb  - ptr to the AvND control block

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_di_node_down_msg_send(AVND_CB *cb)
{
  AVND_MSG msg;
  uint32_t rc = NCSCC_RC_SUCCESS;
  TRACE_ENTER();
  memset(&msg, 0, sizeof(AVND_MSG));
  msg.info.avd = static_cast<AVSV_DND_MSG*>(calloc(1, sizeof(AVSV_DND_MSG)));
  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type = AVSV_N2D_NODE_DOWN_MSG;
  msg.info.avd->msg_info.n2d_node_down_info.msg_id = ++(cb->snd_msg_id);
  msg.info.avd->msg_info.n2d_node_down_info.node_id = cb->node_info.nodeId;
  rc = avnd_di_msg_send(cb, &msg);
  if (rc == NCSCC_RC_SUCCESS) {
    msg.info.avd = 0;
  }
  // free the contents of avnd message
  avnd_msg_content_free(cb, &msg);
  TRACE_LEAVE();
  return NCSCC_RC_SUCCESS;
}

/****************************************************************************
  Name          : avnd_di_msg_ack_process

  Description   : This routine processes the the acks that are generated by
                  AvD in response to messages from AvND. It pops the
                  corresponding record from the AvD msg list & frees it.

  Arguments     : cb  - ptr to the AvND control block
                  mid - message-id

  Return Values : None.

  Notes         : None.
******************************************************************************/
void avnd_di_msg_ack_process(AVND_CB *cb, uint32_t mid) {
  TRACE_ENTER2("%u", mid);

  cb->active_ack_msg_id = mid;

  for (auto iter =  cb->dnd_list.begin(); iter != cb->dnd_list.end(); ++iter) {
    auto rec = *iter;
    osafassert(rec->msg.type == AVND_MSG_AVD);
    const uint32_t msg_id = *(reinterpret_cast<uint32_t*>(&(rec->msg.info.avd->msg_info)));

    // matching record
    if (msg_id == mid) {
      cb->dnd_list.erase(iter);
      // iter is now invalid, exit iterator loop asap
      if (rec->msg.info.avd->msg_type == AVSV_N2D_NODE_DOWN_MSG) {
        // first to stop timer to avoid processing timeout event
        // then perform last step clean up
        avnd_stop_tmr(cb, &rec->resp_tmr);
        avnd_last_step_clean(cb);
      } else if (rec->msg.info.avd->msg_type == AVSV_N2D_NODE_UP_MSG) {
        TRACE("msg node_up acked");
        // Resend buffered headless msg for NCS SUs
        if (cb->is_ncs_su_assigned) {
          avnd_diq_rec_send_buffered_msg(cb, true);
        }
      }
      TRACE("remove msg %u from queue", msg_id);
      avnd_diq_rec_del(cb, rec);
      break;
    }
  }

  TRACE_LEAVE2();
  return;
}

/****************************************************************************
  Name          : avnd_diq_rec_check_buffered_msg

  Description   : The routine buffers messages that are waiting for ack and will
                  resend to AMFD when AMFD is up.
                  All messages are deleted, except following messages to be
                  buffered:
                  - AVSV_N2D_INFO_SU_SI_ASSIGN_MSG
                  - AVSV_N2D_OPERATION_STATE_MSG


  Arguments     : cb - ptr to the AvND control block

  Return Values : None.

  Notes         : None.
******************************************************************************/
void avnd_diq_rec_check_buffered_msg(AVND_CB *cb) {
  for (auto iter = cb->dnd_list.begin(); iter != cb->dnd_list.end();) {
    auto rec = *iter;
    osafassert(rec->msg.type == AVND_MSG_AVD);
    // Assignment response had been sent, but not ack because
    // last controller go down, reset msg_id and will be resent later
    if (rec->msg.info.avd->msg_type == AVSV_N2D_INFO_SU_SI_ASSIGN_MSG) {
      if (rec->msg.info.avd->msg_info.n2d_su_si_assign.msg_id != 0) {
        rec->msg.info.avd->msg_info.n2d_su_si_assign.msg_id = 0;
        LOG_NO(
          "Found not-ack su_si_assign msg for SU:'%s', "
          "SI:'%s', ha_state:'%u', msg_act:'%u', single_csi:'%u', "
          "error:'%u', msg_id:'%u'",
          osaf_extended_name_borrow(&rec->msg.info.avd->msg_info
                                        .n2d_su_si_assign.su_name),
          osaf_extended_name_borrow(&rec->msg.info.avd->msg_info
                                        .n2d_su_si_assign.si_name),
          rec->msg.info.avd->msg_info.n2d_su_si_assign.ha_state,
          rec->msg.info.avd->msg_info.n2d_su_si_assign.msg_act,
          rec->msg.info.avd->msg_info.n2d_su_si_assign
             .single_csi,
          rec->msg.info.avd->msg_info.n2d_su_si_assign.error,
          rec->msg.info.avd->msg_info.n2d_su_si_assign.msg_id);
      }
      // leave in dnd_list
      ++iter;
      continue;
    } else if (rec->msg.info.avd->msg_type == AVSV_N2D_OPERATION_STATE_MSG &&
        (rec->msg.info.avd->msg_info.n2d_opr_state.su_oper_state ==
            SA_AMF_OPERATIONAL_DISABLED ||
            rec->msg.info.avd->msg_info.n2d_opr_state.node_oper_state ==
                SA_AMF_OPERATIONAL_DISABLED)) {
      if (rec->msg.info.avd->msg_info.n2d_opr_state.msg_id != 0) {
        rec->msg.info.avd->msg_info.n2d_opr_state.msg_id = 0;
        LOG_NO(
          "Found not-ack oper_state msg for SU:'%s', "
          "su_oper_state:'%u', node_oper_state:'%u', recovery:'%u'",
          osaf_extended_name_borrow(&rec->msg.info.avd->msg_info
                                        .n2d_opr_state.su_name),
            rec->msg.info.avd->msg_info.n2d_opr_state
               .su_oper_state,
            rec->msg.info.avd->msg_info.n2d_opr_state
               .node_oper_state,
            rec->msg.info.avd->msg_info.n2d_opr_state.rec_rcvr
               .raw);
      }
      // leave in dnd_list
      ++iter;
      continue;
    } else {
    // delete other messages for now
    iter = cb->dnd_list.erase(iter);
    avnd_diq_rec_del(cb, rec);
    }
  }
  return;
}

/****************************************************************************
  Name          : avnd_diq_rec_add

  Description   : This routine adds a record to the AvD msg list.

  Arguments     : cb  - ptr to the AvND control block
                  msg - ptr to the message

  Return Values : ptr to the newly added msg record

  Notes         : None.
******************************************************************************/
AVND_DND_MSG_LIST *avnd_diq_rec_add(AVND_CB *cb, AVND_MSG *msg) {
  AVND_DND_MSG_LIST *rec = 0;
  TRACE_ENTER();

  rec = new AVND_DND_MSG_LIST();

  /* create the association with hdl-mngr */
  if ((0 == (rec->opq_hdl = ncshm_create_hdl(cb->pool_id, NCS_SERVICE_ID_AVND,
                                             (NCSCONTEXT)rec))))
    goto error;

  /* store the msg (transfer memory ownership) */
  rec->msg.type = msg->type;
  rec->msg.info.avd = msg->info.avd;
  rec->no_retries = 0;
  msg->info.avd = 0;

  /* push the record to the AvD msg list */
  cb->dnd_list.push_back(rec);
  TRACE_LEAVE();
  return rec;

error:
  TRACE_LEAVE2("'%p'", rec);
  if (rec) avnd_diq_rec_del(cb, rec);

  return 0;
}

/****************************************************************************
  Name          : avnd_diq_rec_del

  Description   : This routine deletes the record from the AvD msg list.

  Arguments     : cb  - ptr to the AvND control block
                  rec - ptr to the AvD msg record

  Return Values : None.

  Notes         : None.
******************************************************************************/
void avnd_diq_rec_del(AVND_CB *cb, AVND_DND_MSG_LIST *rec) {
  TRACE_ENTER();
  /* remove the association with hdl-mngr */
  if (rec->opq_hdl) ncshm_destroy_hdl(NCS_SERVICE_ID_AVND, rec->opq_hdl);

  /* stop the AvD msg response timer */
  if (m_AVND_TMR_IS_ACTIVE(rec->resp_tmr)) {
    m_AVND_TMR_MSG_RESP_STOP(cb, *rec);
    /* resend pg start track */
    avnd_di_resend_pg_start_track(cb);
  }

  /* free the avnd message contents */
  avnd_msg_content_free(cb, &rec->msg);

  /* free the record */
  delete rec;
  TRACE_LEAVE();
  return;
}
/****************************************************************************
  Name          : avnd_diq_rec_send_buffered_msg

  Description   : Resend buffered msg

  Arguments     : cb  - ptr to the AvND control block
                  only_ncs  - only send msg for NCS SUs

  Return Values : None.

  Notes         : None.
******************************************************************************/
void avnd_diq_rec_send_buffered_msg(AVND_CB *cb, bool only_ncs) {
  TRACE_ENTER();
  // Resend msgs from queue because amfnd dropped during headless
  // or headless-synchronization
  std::vector<AVND_DND_MSG_LIST*> tmp_dnd_list;

  for (auto iter = cb->dnd_list.begin(); iter != cb->dnd_list.end();) {
    auto pending_rec = *iter;
    osafassert(pending_rec->msg.type == AVND_MSG_AVD);

    // Check su_si_assign message
    if (pending_rec->msg.info.avd->msg_type ==
            AVSV_N2D_INFO_SU_SI_ASSIGN_MSG &&
        pending_rec->msg.info.avd->msg_info.n2d_su_si_assign.msg_id ==
            0) {

      // only resend if this SUSI does exist
      AVND_SU *su = cb->sudb.find(Amf::to_string(
          &pending_rec->msg.info.avd->msg_info.n2d_su_si_assign.su_name));
      if (only_ncs && su && !su->is_ncs) {
        ++iter;
        continue;
      }
      if (su != nullptr && su->si_list.n_nodes > 0) {
        pending_rec->msg.info.avd->msg_info.n2d_su_si_assign.msg_id =
            ++(cb->snd_msg_id);
        LOG_NO(
            "Found and resend buffered su_si_assign msg for SU:'%s', "
            "SI:'%s', ha_state:'%u', msg_act:'%u', single_csi:'%u', "
            "error:'%u', msg_id:'%u'",
            osaf_extended_name_borrow(&pending_rec->msg.info.avd->msg_info
                                           .n2d_su_si_assign.su_name),
            osaf_extended_name_borrow(&pending_rec->msg.info.avd->msg_info
                                           .n2d_su_si_assign.si_name),
            pending_rec->msg.info.avd->msg_info.n2d_su_si_assign.ha_state,
            pending_rec->msg.info.avd->msg_info.n2d_su_si_assign.msg_act,
            pending_rec->msg.info.avd->msg_info.n2d_su_si_assign
                .single_csi,
            pending_rec->msg.info.avd->msg_info.n2d_su_si_assign.error,
            pending_rec->msg.info.avd->msg_info.n2d_su_si_assign.msg_id);
        ++iter;
      } else {
        iter = cb->dnd_list.erase(iter);
        avnd_msg_content_free(cb, &pending_rec->msg);
        delete pending_rec;
        continue;
      }
    } else if (pending_rec->msg.info.avd->msg_type == AVSV_N2D_OPERATION_STATE_MSG &&
               pending_rec->msg.info.avd->msg_info.n2d_opr_state.msg_id == 0) {
        AVND_SU *su = cb->sudb.find(Amf::to_string(
            &pending_rec->msg.info.avd->msg_info.n2d_opr_state.su_name));
        if (only_ncs && su && !su->is_ncs) {
          ++iter;
          continue;
        }
        pending_rec->msg.info.avd->msg_info.n2d_opr_state.msg_id =
            ++(cb->snd_msg_id);
        LOG_NO(
            "Found and resend buffered oper_state msg for SU:'%s', "
            "su_oper_state:'%u', node_oper_state:'%u', recovery:'%u'",
            osaf_extended_name_borrow(&pending_rec->msg.info.avd->msg_info
                                           .n2d_opr_state.su_name),
            pending_rec->msg.info.avd->msg_info.n2d_opr_state
                .su_oper_state,
            pending_rec->msg.info.avd->msg_info.n2d_opr_state
                .node_oper_state,
            pending_rec->msg.info.avd->msg_info.n2d_opr_state.rec_rcvr
                .raw);
        ++iter;
    } else if (pending_rec->msg.info.avd->msg_type == AVSV_N2D_DATA_REQUEST_MSG &&
               pending_rec->msg.info.avd->msg_info.n2d_data_req.msg_id == 0) {
        AVND_SU *su = cb->sudb.find(Amf::to_string(
            &pending_rec->msg.info.avd->msg_info.n2d_data_req.param_info.name));
        if (only_ncs && su && !su->is_ncs) {
          ++iter;
          continue;
        }
        pending_rec->msg.info.avd->msg_info.n2d_data_req.msg_id =
              ++(cb->snd_msg_id);
        LOG_NO(
            "Found and resend buffered Data Req msg for SU:'%s', msg_id:'%u'",
            osaf_extended_name_borrow(&pending_rec->msg.info.avd->msg_info
                                           .n2d_data_req.param_info.name),
            pending_rec->msg.info.avd->msg_info.n2d_data_req.msg_id);
       ++iter;
    } else {
      ++iter;
      if (only_ncs) continue;
    }
    tmp_dnd_list.push_back(pending_rec);
  }

  TRACE("retransmit message to amfd");
  for (auto pending_rec : tmp_dnd_list) {
    avnd_diq_rec_send(cb, pending_rec);
  }
  TRACE_LEAVE();
  return;
}

/****************************************************************************
  Name          : avnd_diq_rec_send

  Description   : This routine sends message (contained in the record) to
                  AvD. It allocates a new message, copies the contents from
                  the message contained in the record & then sends it to AvD.

  Arguments     : cb  - ptr to the AvND control block
                  rec - ptr to the msg record

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.

  Notes         : None
******************************************************************************/
uint32_t avnd_diq_rec_send(AVND_CB *cb, AVND_DND_MSG_LIST *rec) {
  AVND_MSG msg;
  TRACE_ENTER();

  memset(&msg, 0, sizeof(AVND_MSG));

  /* copy the contents from the record */
  uint32_t rc = avnd_msg_copy(cb, &msg, &rec->msg);

  /* send the message to AvD */
  if (NCSCC_RC_SUCCESS == rc) rc = avnd_mds_send(cb, &msg, &cb->avd_dest, 0);

  /* start the msg response timer */
  if (NCSCC_RC_SUCCESS == rc) {
    rec->no_retries++;
	if (rec->msg.info.avd->msg_type == AVSV_N2D_NODE_UP_MSG ||
        rec->msg.info.avd->msg_type == AVSV_N2D_NODE_DOWN_MSG) {
      m_AVND_TMR_MSG_RESP_START(cb, *rec, rc);
    }
    msg.info.avd = 0;
  }

  /* free the msg */
  avnd_msg_content_free(cb, &msg);
  TRACE_LEAVE2("%u", rc);
  return rc;
}

/**
 * Dispatch admin operation requests to the real handler
 * @param cb
 * @param evt
 *
 * @return uns32
 */
uint32_t avnd_evt_avd_admin_op_req_evh(AVND_CB *cb, AVND_EVT *evt) {
  uint32_t rc = NCSCC_RC_FAILURE;
  AVSV_D2N_ADMIN_OP_REQ_MSG_INFO *info =
      &evt->info.avd->msg_info.d2n_admin_op_req_info;

  TRACE_ENTER2("%u", info->class_id);

  switch (info->class_id) {
    case AVSV_SA_AMF_NODE:
      rc = avnd_evt_node_admin_op_req(cb, evt);
      break;
    case AVSV_SA_AMF_COMP:
      rc = avnd_evt_comp_admin_op_req(cb, evt);
      break;
    case AVSV_SA_AMF_SU:
      rc = avnd_evt_su_admin_op_req(cb, evt);
      break;
    default:
      LOG_NO("%s: unsupported adm op for class %u", __FUNCTION__,
             info->class_id);
      break;
  }

  TRACE_LEAVE();
  return rc;
}

/**
 * Handle a received heart beat message, just reset the duration timer.
 * @param cb
 * @param evt
 *
 * @return uns32
 */
uint32_t avnd_evt_avd_hb_evh(AVND_CB *cb, AVND_EVT *evt) {
  TRACE_ENTER2("%u", evt->info.avd->msg_info.d2n_hb_info.seq_id);
  avnd_stop_tmr(cb, &cb->hb_duration_tmr);
  avnd_start_tmr(cb, &cb->hb_duration_tmr, AVND_TMR_HB_DURATION,
                 cb->hb_duration, 0);
  return NCSCC_RC_SUCCESS;
}

/**
 * The heart beat duration timer expired. Reboot this node.
 * @param cb
 * @param evt
 *
 * @return uns32
 */
uint32_t avnd_evt_tmr_avd_hb_duration_evh(AVND_CB *cb, AVND_EVT *evt) {
  int status;

  TRACE_ENTER();

  LOG_ER("AMF director heart beat timeout, generating core for amfd");

  if ((status = system("killall -ABRT osafamfd")) == -1)
    syslog(LOG_ERR, "system(killall) FAILED %x", status);

  /* allow core to be created */
  sleep(1);

  opensaf_reboot(
      avnd_cb->node_info.nodeId,
      osaf_extended_name_borrow(&avnd_cb->node_info.executionEnvironment),
      "AMF director heart beat timeout");

  return NCSCC_RC_SUCCESS;
}

/******************************************************************************
  Name          : avnd_evt_avd_role_change_evh

  Description   : This routine takes cares of role change of AvND.

  Arguments     : cb  - ptr to the AvND control block.
                  evt - ptr to the AvND event.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None
******************************************************************************/
uint32_t avnd_evt_avd_role_change_evh(AVND_CB *cb, AVND_EVT *evt) {
  uint32_t rc = NCSCC_RC_SUCCESS;
  V_DEST_RL mds_role;
  SaAmfHAStateT prev_ha_state;

  TRACE_ENTER();

  /* Correct the counters first. */
  AVSV_D2N_ROLE_CHANGE_INFO *info =
      &evt->info.avd->msg_info.d2n_role_change_info;

  TRACE("MsgId: %u,NodeId:%x, role rcvd:%u role present:%u", info->msg_id,
        info->node_id, info->role, cb->avail_state_avnd);

  avnd_msgid_assert(info->msg_id);
  cb->rcv_msg_id = info->msg_id;

  /* dont process unless AvD is up */
  if (!m_AVND_CB_IS_AVD_UP(cb)) {
    LOG_IN("AVD is not up yet");
    return NCSCC_RC_FAILURE;
  }

  prev_ha_state = cb->avail_state_avnd;

  /* Ignore the duplicate roles. */
  if (prev_ha_state == (SaAmfHAStateT)info->role) {
    return NCSCC_RC_SUCCESS;
  }

  if ((SA_AMF_HA_ACTIVE == cb->avail_state_avnd) &&
      (SA_AMF_HA_QUIESCED == info->role)) {
    TRACE_1("SA_AMF_HA_QUIESCED role received");
    if (NCSCC_RC_SUCCESS !=
        (rc = avnd_mds_set_vdest_role(
             cb, static_cast<SaAmfHAStateT>(V_DEST_RL_QUIESCED)))) {
      TRACE("avnd_mds_set_vdest_role returned failure, role:%u", info->role);
      return rc;
    }
    return rc;
  }

  cb->avail_state_avnd = static_cast<SaAmfHAStateT>(info->role);

  if (cb->avail_state_avnd == SA_AMF_HA_ACTIVE) {
    mds_role = V_DEST_RL_ACTIVE;
    TRACE_1("SA_AMF_HA_ACTIVE role received");
  } else {
    mds_role = V_DEST_RL_STANDBY;
    TRACE_1("SA_AMF_HA_STANDBY role received");
  }

  if (NCSCC_RC_SUCCESS != (rc = avnd_mds_set_vdest_role(
                               cb, static_cast<SaAmfHAStateT>(mds_role)))) {
    TRACE_1("avnd_mds_set_vdest_role returned failure");
    return rc;
  }

  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_di_resend_pg_start_track

  Description   : This routing will get called on AVD fail-over or coming back
                  from headless to send the PG start messages to the new AVD.

  Arguments     : cb  - ptr to the AvND control block

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_di_resend_pg_start_track(AVND_CB *cb) {
  uint32_t rc = NCSCC_RC_SUCCESS;

  TRACE_ENTER();

  for (const auto &pg_rec : cb->pgdb) {
    AVND_PG *pg = pg_rec.second;
    rc = avnd_di_pg_act_send(cb, pg->csi_name, AVSV_PG_TRACK_ACT_START, true);

    if (NCSCC_RC_SUCCESS != rc) break;
  }

  TRACE_LEAVE();
  return rc;
}

/**
 * The SC absence timer expired. Reboot this node.
 * @param cb
 * @param evt
 *
 * @return uns32
 */
uint32_t avnd_evt_tmr_sc_absence_evh(AVND_CB *cb, AVND_EVT *evt) {
  TRACE_ENTER();

  LOG_ER("AMF director absence timeout");

  opensaf_reboot(
      avnd_cb->node_info.nodeId,
      osaf_extended_name_borrow(&avnd_cb->node_info.executionEnvironment),
      "AMF director absence timeout");

  return NCSCC_RC_SUCCESS;
}

/**
 * Send csi comp state info to amfd when cluster comes back from headless
 * @param cb
 *
 * @return void
 */
void avnd_sync_csicomp(AVND_CB *cb) {
  AVND_MSG msg;
  uint32_t rc = NCSCC_RC_SUCCESS;
  const AVND_COMP *comp;
  const AVND_COMP_CSI_REC *csi;
  SaAmfCSIAssignment csi_assignment;

  TRACE_ENTER();

  /* Send the state info to avd. */
  memset(&msg, 0, sizeof(AVND_MSG));
  msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));

  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type = AVSV_N2D_ND_CSICOMP_STATE_INFO_MSG;
  msg.info.avd->msg_info.n2d_nd_csicomp_state_info.msg_id = cb->snd_msg_id;
  msg.info.avd->msg_info.n2d_nd_csicomp_state_info.node_id =
      cb->node_info.nodeId;
  msg.info.avd->msg_info.n2d_nd_csicomp_state_info.num_csicomp = 0;
  msg.info.avd->msg_info.n2d_nd_csicomp_state_info.csicomp_list = nullptr;

  // add CSICOMP objects
  for (comp = avnd_compdb_rec_get_next(cb->compdb, ""); comp != nullptr;
       comp = avnd_compdb_rec_get_next(cb->compdb, comp->name)) {
    TRACE("syncing comp: %s", comp->name.c_str());
    for (csi = m_AVND_CSI_REC_FROM_COMP_DLL_NODE_GET(
             m_NCS_DBLIST_FIND_FIRST(&comp->csi_list));
         csi != nullptr; csi = m_AVND_CSI_REC_FROM_COMP_DLL_NODE_GET(
                             m_NCS_DBLIST_FIND_NEXT(&csi->comp_dll_node))) {
      osafassert(csi != nullptr);

      osaf_extended_name_alloc(comp->name.c_str(), &csi_assignment.comp);
      osaf_extended_name_alloc(csi->name.c_str(), &csi_assignment.csi);

      if (csi->si != nullptr) {
        csi_assignment.saAmfCSICompHAState = csi->si->curr_state;
        TRACE("si found. HA state is %d", csi_assignment.saAmfCSICompHAState);
      } else {
        TRACE("csi->si is nullptr");
        csi_assignment.saAmfCSICompHAState = SA_AMF_HA_QUIESCED;
      }

      add_csicomp_state_info(&msg, &csi_assignment);
    }

    add_comp_state_info(&msg, comp);
  }

  LOG_NO("%d CSICOMP states sent",
         msg.info.avd->msg_info.n2d_nd_csicomp_state_info.num_csicomp);
  LOG_NO("%d COMP states sent",
         msg.info.avd->msg_info.n2d_nd_csicomp_state_info.num_comp);

  rc = avnd_di_msg_send(cb, &msg);
  if (rc == NCSCC_RC_SUCCESS)
    msg.info.avd = 0;
  else
    LOG_ER("avnd_di_msg_send FAILED");

  avnd_msg_content_free(cb, &msg);

  TRACE_LEAVE();
}

/**
 * Send susi state info to amfd when cluster comes back from headless
 * @param cb
 *
 * @return void
 */
void avnd_sync_sisu(AVND_CB *cb) {
  AVND_MSG msg;
  uint32_t rc = NCSCC_RC_SUCCESS;
  SaAmfSIAssignment si_assignment;
  const AVND_SU *su;
  const AVND_SU_SI_REC *si;

  TRACE_ENTER();

  /* Send the state info to avd. */
  memset(&msg, 0, sizeof(AVND_MSG));
  msg.info.avd = static_cast<AVSV_DND_MSG *>(calloc(1, sizeof(AVSV_DND_MSG)));

  msg.type = AVND_MSG_AVD;
  msg.info.avd->msg_type =
      AVSV_N2D_ND_SISU_STATE_INFO_MSG;  // AVSV_N2D_ND_ASSIGN_STATES_MSG
  msg.info.avd->msg_info.n2d_nd_sisu_state_info.msg_id = cb->snd_msg_id;
  msg.info.avd->msg_info.n2d_nd_sisu_state_info.node_id = cb->node_info.nodeId;
  msg.info.avd->msg_info.n2d_nd_sisu_state_info.num_sisu = 0;
  msg.info.avd->msg_info.n2d_nd_sisu_state_info.sisu_list = nullptr;

  // gather SISU states
  for (su = avnd_sudb_rec_get_next(cb->sudb, ""); su != nullptr;
       su = avnd_sudb_rec_get_next(cb->sudb, su->name)) {
    TRACE("syncing su: %s", su->name.c_str());

    // attach SISUs
    for (si = (AVND_SU_SI_REC *)m_NCS_DBLIST_FIND_FIRST(&su->si_list);
         si != nullptr;
         si = (AVND_SU_SI_REC *)m_NCS_DBLIST_FIND_NEXT(&si->su_dll_node)) {
      osaf_extended_name_alloc(su->name.c_str(), &si_assignment.su);
      osaf_extended_name_alloc(si->name.c_str(), &si_assignment.si);
      si_assignment.saAmfSISUHAState = si->curr_state;

      if (m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_ASSIGNED(si)) {
        si_assignment.assignmentAct =
            static_cast<uint32_t>(AVSV_SUSI_ACT_ASGND);
      } else if (m_AVND_SU_SI_CURR_ASSIGN_STATE_IS_ASSIGNING(si)) {
        if (si->prv_state) {
          si_assignment.assignmentAct =
              static_cast<uint32_t>(AVSV_SUSI_ACT_MOD);
        } else {
          si_assignment.assignmentAct =
              static_cast<uint32_t>(AVSV_SUSI_ACT_ASGN);
        }
      } else {
        si_assignment.assignmentAct = static_cast<uint32_t>(AVSV_SUSI_ACT_DEL);
      }

      add_sisu_state_info(&msg, &si_assignment);
    }

    add_su_state_info(&msg, su);
  }

  rc = avnd_di_msg_send(cb, &msg);
  if (rc == NCSCC_RC_SUCCESS)
    msg.info.avd = 0;
  else
    LOG_ER("avnd_di_msg_send FAILED");

  avnd_msg_content_free(cb, &msg);

  TRACE_LEAVE();
}
