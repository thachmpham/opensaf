/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2008 The OpenSAF Foundation
 * Copyright (C) 2017, Oracle and/or its affiliates. All rights reserved.
 * Copyright Ericsson AB 2020 - All Rights Reserved.
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

  DESCRIPTION: This file contains avnd to avnd communication handling functions.

  FUNCTIONS INCLUDED in this module:

****************************************************************************/
#include <cinttypes>

#include "amf/amfnd/avnd.h"
// Remember MDS install version of Agents. It can be used to send msg to Agent
// based on their versions.
std::map<MDS_DEST, MDS_SVC_PVT_SUB_PART_VER> agent_mds_ver_db;
std::set<std::string> container_csis;
extern const AVND_EVT_HDLR g_avnd_func_list[AVND_EVT_MAX];

static uint32_t avnd_evt_avnd_avnd_api_msg_hdl(AVND_CB *cb, AVND_EVT *evt);
static uint32_t avnd_evt_avnd_avnd_cbk_msg_hdl(AVND_CB *cb, AVND_EVT *evt);
static uint32_t avnd_evt_avnd_avnd_api_resp_msg_hdl(AVND_CB *cb, AVND_EVT *evt);
/******************************************************************************
  Name          : avnd_evt_avnd_avnd_msg

  Description   : This routine handles events from another AvNDs.

  Arguments     : cb  - ptr to the AvND control block.
                  evt - ptr to the AvND event.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None
******************************************************************************/
uint32_t avnd_evt_avnd_avnd_evh(AVND_CB *cb, AVND_EVT *evt) {
  uint32_t res = NCSCC_RC_SUCCESS;
  AVSV_ND2ND_AVND_MSG *avnd_avnd_msg = evt->info.avnd;
  AVSV_ND2ND_AVA_MSG *msg = nullptr;

  TRACE_ENTER2("%u", avnd_avnd_msg->type);

  if (AVND_AVND_CBK_DEL == avnd_avnd_msg->type) {
    /* This is a Callback Del message */
    AVND_COMP_CBK *cbk_rec = nullptr;
    std::string comp_name;

    AVSV_ND2ND_CBK_DEL *del_cbk = &avnd_avnd_msg->info.cbk_del;
    comp_name = Amf::to_string(&del_cbk->comp_name);
    AVND_COMP *o_comp =
        m_AVND_INT_EXT_COMPDB_REC_GET(cb->internode_avail_comp_db, comp_name);

    if (nullptr == o_comp) {
      LOG_ER("Comp not in Inter/Ext Comp DB: %s : opq_hdl= %u",
             comp_name.c_str(), del_cbk->opq_hdl);
      return NCSCC_RC_FAILURE;
    }

    m_AVND_COMP_CBQ_ORIG_INV_GET(o_comp, del_cbk->opq_hdl, cbk_rec);

    if (!cbk_rec) {
      TRACE_3("No callback record found: %s, opq_hdl=%u", comp_name.c_str(),
              del_cbk->opq_hdl);
      goto done;
    }

    avnd_comp_cbq_rec_pop_and_del(cb, o_comp, cbk_rec->opq_hdl, false);
    goto done;
  }

  msg = avnd_avnd_msg->info.msg;

  if (AVSV_AVA_API_MSG == msg->type) {
    res = avnd_evt_avnd_avnd_api_msg_hdl(cb, evt);
  } else if (AVSV_AVND_AMF_CBK_MSG == msg->type) {
    res = avnd_evt_avnd_avnd_cbk_msg_hdl(cb, evt);
  } else if (AVSV_AVND_AMF_API_RESP_MSG == msg->type) {
    res = avnd_evt_avnd_avnd_api_resp_msg_hdl(cb, evt);
  } else {
    res = NCSCC_RC_FAILURE;
  }

done:
  if (NCSCC_RC_SUCCESS != res) {
    LOG_ER("avnd_evt_avnd_avnd_msg:failure:Msg Type %u and res %u",
           avnd_avnd_msg->type, res);
  }

  TRACE_LEAVE();
  return res;
}

/******************************************************************************
  Name          : avnd_evt_avnd_avnd_api_msg_hdl

  Description   : This routine handles AVA messages (AVSV_AVA_API_MSG)
                  from proxy AvNDs.

  Arguments     : cb  - ptr to the AvND control block.
                  evt - ptr to the AvND event.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None
******************************************************************************/
uint32_t avnd_evt_avnd_avnd_api_msg_hdl(AVND_CB *cb, AVND_EVT *evt) {
  uint32_t res = NCSCC_RC_SUCCESS;
  AVND_EVT_TYPE evt_type;
  TRACE_ENTER();

  /* The event is coming from AvND and not from AvA, so we need to
     copy mds context information to evt so that we spoof it like
     coming from AvA and then call existing AvA handling functions.
     mds context information can be returned to sender AvND so that
     it can send response back to waiting AvA. mds context information
     is needed only in case of SYNC call from AvA ->proxy AvND.
     Presently only two calls from AvA are ASYNC and
     those are quiescing_compl and ava_resp, these wouln't require
     mds context information and it will be nullptr for these two calls */

  memcpy(&(evt->mds_ctxt), &(evt->info.avnd->mds_ctxt),
         sizeof(MDS_SYNC_SND_CTXT));

  /* Create AvA message, so that it look like it is coming from AvA. */
  evt_type =
      static_cast<AVND_EVT_TYPE>((int)(evt->info.avnd->info.msg->info.api_info.type -
                                 AVSV_AMF_FINALIZE + AVND_EVT_AVA_FINALIZE));
  evt->info.ava.msg = evt->info.avnd->info.msg;

  if (AVND_EVT_AVA_COMP_REG == evt_type) {
    if (AVND_LED_STATE_GREEN != cb->led_state) {
      /* AvND Not Ready, so send TRY AGAIN to sender AvND */
      AVSV_AMF_API_INFO *api_info = &evt->info.ava.msg->info.api_info;
      AVSV_AMF_COMP_REG_PARAM *reg = &api_info->param.reg;
      AVND_COMP *comp = 0;
      SaAisErrorT amf_rc = SA_AIS_OK;
      bool msg_from_avnd = true;

      comp = avnd_compdb_rec_get(cb->compdb, Amf::to_string(&reg->comp_name));
      if (nullptr == comp) return NCSCC_RC_FAILURE;

      /* send the response back to AvA */
      amf_rc = SA_AIS_ERR_TRY_AGAIN;
      res =
          avnd_amf_resp_send(cb, AVSV_AMF_COMP_REG, amf_rc, 0, &api_info->dest,
                             &evt->mds_ctxt, comp, msg_from_avnd);
      return res;
    } /* if(AVND_LED_STATE_GREEN != cb->led_state) */
  }

  if ((evt_type >= AVND_EVT_AVA_FINALIZE) && (evt_type < AVND_EVT_AVA_MAX)) {
    res = g_avnd_func_list[evt_type](cb, evt);
  } else {
    res = NCSCC_RC_FAILURE;
    goto done;
  }

done:
  TRACE_LEAVE2("%u", res);
  return res;
}

/******************************************************************************
  Name          : avnd_evt_avnd_avnd_api_resp_msg_hdl

  Description   : This routine handles API response message from proxied AvND.
                  It just forwards the message to AvA.

  Arguments     : cb  - ptr to the AvND control block.
                  evt - ptr to the AvND event.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None
******************************************************************************/
uint32_t avnd_evt_avnd_avnd_api_resp_msg_hdl(AVND_CB *cb, AVND_EVT *evt) {
  uint32_t res = NCSCC_RC_SUCCESS;
  AVSV_ND2ND_AVND_MSG *avnd_msg = evt->info.avnd;
  AVSV_AMF_API_RESP_INFO *resp_info = &avnd_msg->info.msg->info.api_resp_info;
  SaAmfHAStateT *ha_state = nullptr;
  MDS_DEST reg_dest = 0;
  const std::string comp_name = Amf::to_string(&avnd_msg->comp_name);

  TRACE_ENTER2("%s: Type =%u and rc = %u", comp_name.c_str(), resp_info->type,
               resp_info->rc);

  AVND_COMP *o_comp =
      m_AVND_INT_EXT_COMPDB_REC_GET(cb->internode_avail_comp_db, comp_name);
  if (nullptr == o_comp) {
    TRACE("Couldn't find comp in Inter/Ext Comp DB");
    // Check if it is a internode responses like error report, etc.
    o_comp = avnd_cb->compdb_internode.find(comp_name);
    if (nullptr == o_comp) {
      LOG_ER("Couldn't find comp in internode comp DB");
      res = NCSCC_RC_FAILURE;
      goto done;
    } else {
      TRACE("Comp '%s' found in internode comp DB", o_comp->name.c_str());
    }
  }
  reg_dest = o_comp->reg_dest;
  /*****************************************************************************
   *   Some processing for registration and unregistration starts
   *****************************************************************************/
  if (AVSV_AMF_COMP_REG == resp_info->type) {
    if (SA_AIS_OK != resp_info->rc) {
      /* We got comp reg failure. We need to delete the component.  */
      o_comp =
          m_AVND_INT_EXT_COMPDB_REC_GET(cb->internode_avail_comp_db, comp_name);
      if (nullptr == o_comp) {
        LOG_ER("Couldn't find comp in Inter/Ext Comp DB");
        res = NCSCC_RC_FAILURE;
        goto done;
      }
      reg_dest = o_comp->reg_dest;
      res = avnd_internode_comp_del(cb, comp_name);
    } else {
      o_comp =
          m_AVND_INT_EXT_COMPDB_REC_GET(cb->internode_avail_comp_db, comp_name);
      if (nullptr == o_comp) {
        LOG_ER("Couldn't find comp in Inter/Ext Comp DB");
        res = NCSCC_RC_FAILURE;
        goto done;
      }
      reg_dest = o_comp->reg_dest;
      /* Reg reg SUCCESS. Add proxied information to proxy */
      o_comp->reg_resp_pending = false;
      m_AVND_COMP_REG_SET(o_comp);
      res = avnd_comp_proxied_add(cb, o_comp, o_comp->pxy_comp, false);
    }
  }

  if (AVSV_AMF_COMP_UNREG == resp_info->type) {
    if (SA_AIS_OK != resp_info->rc) {
      /* We got comp unreg failure. We need to forward the resp to the user. */
    } else {
      /* Unreg SUCCESS. We need to delete the component as well as proxy-proxied
         relation */
      o_comp =
          m_AVND_INT_EXT_COMPDB_REC_GET(cb->internode_avail_comp_db, comp_name);
      if (nullptr == o_comp) {
        LOG_ER("Couldn't find comp in Inter/Ext Comp DB");
        res = NCSCC_RC_FAILURE;
        goto done;
      }
      reg_dest = o_comp->reg_dest;
      res = avnd_comp_proxied_del(cb, o_comp, o_comp->pxy_comp, false, nullptr);
      if (NCSCC_RC_SUCCESS != res) {
        TRACE("%s: avnd_comp_proxied_del Failed: rc:%u", __FUNCTION__, res);
      }
      res = avnd_internode_comp_del(cb, o_comp->name);
      if (NCSCC_RC_SUCCESS != res) {
        TRACE("%s: avnd_internode_comp_del Failed: rc:%u", __FUNCTION__, res);
      }
    }
  }

  if (AVSV_AMF_HA_STATE_GET == resp_info->type) {
    ha_state = &resp_info->param.ha_get.ha;
  }

  /*****************************************************************************
   *   Some processing for registration and unregistration ends
   *****************************************************************************/

  /* We have to fprwrd this message to AvA.  */
  res = avnd_amf_resp_send(cb, resp_info->type, resp_info->rc,
                           reinterpret_cast<uint8_t *>(ha_state),
                           &o_comp->reg_dest, &avnd_msg->mds_ctxt,
                           nullptr, false);
  if (o_comp->comp_type == AVND_COMP_TYPE_INTER_NODE_NP) {
    TRACE("Comp '%s' deleted from internode comp DB", o_comp->name.c_str());
    cb->compdb_internode.erase(o_comp->name);
    delete o_comp;
  }
  if (NCSCC_RC_SUCCESS != res) {
    LOG_ER("%s: Msg Send to AvA Failed:Comp:%s ,Type: %u, rc:%u, Dest:%" PRIu64,
           __FUNCTION__, comp_name.c_str(), resp_info->type, resp_info->rc,
           reg_dest);
  }

done:
  if (NCSCC_RC_SUCCESS != res) {
    LOG_ER("%s: Msg Send to AvA Failed:Comp:%s ,Type: %u, rc:%u", __FUNCTION__,
           comp_name.c_str(), resp_info->type, resp_info->rc);
  }
  TRACE_LEAVE2("%u", res);
  return res;
}

/******************************************************************************
  Name          : avnd_evt_avnd_avnd_cbk_msg_hdl

  Description   : This routine handles callbacks from other AvNDs. It forwards
                  these requests to AvA.

  Arguments     : cb  - ptr to the AvND control block.
                  evt - ptr to the AvND event.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None
******************************************************************************/
uint32_t avnd_evt_avnd_avnd_cbk_msg_hdl(AVND_CB *cb, AVND_EVT *evt) {
  uint32_t rc = 0;
  AVSV_ND2ND_AVND_MSG *avnd_msg = evt->info.avnd;
  AVND_COMP *comp = nullptr;
  AVSV_AMF_CBK_INFO *cbk_info = avnd_msg->info.msg->info.cbk_info;
  AVSV_AMF_CBK_INFO *cbk_rec = nullptr;
  AVND_COMP_CBK *rec = nullptr;
  const std::string comp_name = Amf::to_string(&cbk_info->param.hc.comp_name);

  TRACE_ENTER2("Type:%u, Hdl:%llu, Inv:%llu", cbk_info->type, cbk_info->hdl,
               cbk_info->inv);

  /* Create a callback record for storing purpose. */
  rc = avsv_amf_cbk_copy(&cbk_rec, cbk_info);

  if (NCSCC_RC_SUCCESS != rc) goto done;

  /* Get the component pointer. */

  if ((comp = m_AVND_INT_EXT_COMPDB_REC_GET(cb->internode_avail_comp_db,
                                            comp_name)) == nullptr) {
    /*
       NOTE : The component name has been taken from health check callback
       structure, this will not make any difference to other types of callback
       structures, such as comp_term, csi_set, csi_rem, etc. We can take name
       from any one of them as the name is the same in all the structures. Like
       we can use cbk_info->param.csi_set.comp_name or
       cbk_info->param.pxied_comp_inst.comp_name.
     */
    rc = NCSCC_RC_FAILURE;
    LOG_ER("Couldn't find comp %s in Inter/Ext Comp DB", comp_name.c_str());
    /* free the callback info */
    if (cbk_rec) avsv_amf_cbk_free(cbk_rec);

    goto done;
  }

  /* add the record */
  if ((0 !=
       (rec = avnd_comp_cbq_rec_add(cb, comp, cbk_rec, &comp->reg_dest, 0)))) {
    /* assign inv & hdl values */
    /* Here we will preserve the original invocation handle cbk_rec->inv and
       use our created handle rec->opq_hdl. We will use orig_opq_hdl handle
       when this AvND will forward the callback response back to caller AvND */
    rec->orig_opq_hdl = cbk_rec->inv;
    rec->cbk_info->inv = rec->opq_hdl;
    rec->cbk_info->hdl = comp->reg_hdl;

    /* send the request if comp is not in orphaned state.
       in case of orphaned component we will send it later when
       comp moves back to instantiated. we will free it, if the comp
       doesnt move to instantiated */
    if (!m_AVND_COMP_PRES_STATE_IS_ORPHANED(comp)) {
      rc = avnd_comp_cbq_rec_send(cb, comp, rec, false);
    }

    if (NCSCC_RC_SUCCESS != rc) {
      LOG_ER("comp %s cbk rec send failed", comp->name.c_str());
    }
  } else {
    rc = NCSCC_RC_FAILURE;
    LOG_ER("%s Cbk Rec Add Failed:Dest: %" PRIu64, comp->name.c_str(),
           comp->reg_dest);
  }

  if (NCSCC_RC_SUCCESS != rc && rec) {
    /* pop & delete */
    uint32_t found;

    rec = avnd_comp_cbq_rec_pop(comp, rec->opq_hdl, found);
    rec->cbk_info = 0;
    if (found) avnd_comp_cbq_rec_del(cb, comp, rec);
  }

done:

  if (NCSCC_RC_SUCCESS != rc) {
    LOG_ER(
      "avnd_evt_avnd_avnd_cbk_msg_hdl():Failure:Type:%u Hdl:%llu and Inv:%llu",
      cbk_info->type, cbk_info->hdl, cbk_info->inv);
  }

  TRACE_LEAVE2("%u", rc);
  return rc;
}

/******************************************************************************
  Name          : avnd_evt_avd_reboot_evh

  Description   : This routine handles callbacks from other AvNDs. It forwards
                  these requests to AvA.

  Arguments     : cb  - ptr to the AvND control block.
                  evt - ptr to the AvND event.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None
******************************************************************************/
uint32_t avnd_evt_avd_reboot_evh(AVND_CB *cb, AVND_EVT *evt) {
  AVSV_D2N_REBOOT_MSG_INFO *info;

  info = &evt->info.avd->msg_info.d2n_reboot_info;
  TRACE_ENTER2("%x, %u", info->node_id, info->msg_id);

  osafassert(AVSV_D2N_REBOOT_MSG == evt->info.avd->msg_type);

  avnd_msgid_assert(info->msg_id);
  cb->rcv_msg_id = info->msg_id;

  /* Clear error report related alarms before reboot.
     TODO: This for loop can be removed if AVD remembers and checkpoints
     alarms sent due to error report.
   */
  for (AVND_COMP *comp = avnd_compdb_rec_get_next(avnd_cb->compdb, "");
       comp != nullptr;
       comp = avnd_compdb_rec_get_next(avnd_cb->compdb, comp->name)) {
    /* Skip OpenSAF and external components */
    if (comp->su->is_ncs || comp->su->su_is_external) continue;

    if (comp->error_report_sent == true) {
      avnd_di_uns32_upd_send(AVSV_SA_AMF_COMP, saAmfCompRecoveryOnError_ID,
                             comp->name, 0);
      comp->error_report_sent = false;
    }
  }

  LOG_NO("Received reboot order from %x, ordering reboot now!", info->node_id);
  opensaf_reboot(cb->node_info.nodeId,
                 osaf_extended_name_borrow(&cb->node_info.executionEnvironment),
                 "Received reboot order");

  TRACE_LEAVE();
  return NCSCC_RC_SUCCESS;
}
