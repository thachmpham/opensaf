/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2008 The OpenSAF Foundation
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

  This file contains routines to manage the pending-callback list.

..............................................................................

  FUNCTIONS INCLUDED in this module:


******************************************************************************
*/
#include "amf/amfnd/avnd.h"

/*** static function declarations */

AVND_COMP_CBK *avnd_comp_cbq_rec_add(AVND_CB *, AVND_COMP *,
                                     AVSV_AMF_CBK_INFO *, MDS_DEST *, SaTimeT);

/****************************************************************************
  Name          : avnd_evt_ava_csi_quiescing_compl

  Description   : This routine processes the AMF CSI Quiescing Complete
                  message from AvA.

  Arguments     : cb  - ptr to the AvND control block
                  evt - ptr to the AvND event

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_evt_ava_csi_quiescing_compl_evh(AVND_CB *cb, AVND_EVT *evt) {
  AVSV_AMF_API_INFO *api_info = &evt->info.ava.msg->info.api_info;
  AVSV_AMF_CSI_QUIESCING_COMPL_PARAM *qsc = &api_info->param.csiq_compl;
  AVND_COMP *comp = 0;
  AVND_COMP_CBK *cbk_rec = 0;
  AVND_COMP_CSI_REC *csi = 0;
  AVND_ERR_INFO err_info;
  uint32_t rc = NCSCC_RC_SUCCESS;
  bool msg_from_avnd = false, int_ext_comp = false;
  SaAisErrorT amf_rc = SA_AIS_OK;

  TRACE_ENTER();

  if (AVND_EVT_AVND_AVND_MSG == evt->type) {
    /* This means that the message has come from proxy AvND to this AvND. */
    msg_from_avnd = true;
  }

  if (false == msg_from_avnd) {
    /* Check for internode or external coomponent first
       If it is, then forward it to the respective AvND. */
    rc = avnd_int_ext_comp_hdlr(cb, api_info, &evt->mds_ctxt, &amf_rc,
                                &int_ext_comp);
    if (true == int_ext_comp) {
      goto done;
    }
  }

  /*
   * Get the comp. As comp-name is derived from the AvA lib, it's
   * non-existence is a fatal error.
   */
  comp = avnd_compdb_rec_get(cb->compdb, Amf::to_string(&qsc->comp_name));
  osafassert(comp);

  /* Stop the qscing complete timer if started any */
  if (m_AVND_TMR_IS_ACTIVE(comp->qscing_tmr)) {
    m_AVND_TMR_COMP_QSCING_CMPL_STOP(cb, comp);
  }

  /* npi comps dont interact with amf */
  if (!m_AVND_COMP_TYPE_IS_PREINSTANTIABLE(comp)) goto done;

  /* get the matching entry from the cbk list */
  m_AVND_COMP_CBQ_INV_GET(comp, qsc->inv, cbk_rec);

  /* it can be a responce from a proxied, see any proxied comp are there */
  if (!cbk_rec && comp->pxied_list.n_nodes != 0) {
    AVND_COMP_PXIED_REC *rec = 0;

    /* parse thru all proxied comp, look in the list of inv handle of each of
     * them */
    for (rec =
             (AVND_COMP_PXIED_REC *)m_NCS_DBLIST_FIND_FIRST(&comp->pxied_list);
         rec; rec = (AVND_COMP_PXIED_REC *)m_NCS_DBLIST_FIND_NEXT(
                  &rec->comp_dll_node)) {
      m_AVND_COMP_CBQ_INV_GET(rec->pxied_comp, qsc->inv, cbk_rec);
      if (cbk_rec) break;
    }

    /* its a responce from proxied, update the comp ptr */
    if (cbk_rec) {
      comp = rec->pxied_comp;

      /*************************   Section  1 Starts Here
       * **************************/

      /* We have got the component name, but we are not sure this is a local
         component or internode/ext component as the proxy can register the
         proxied component with its own name i.e. proxy may use the same amf
         handle for registering the proxied component. The proxied component can
         be a local/intenode/ext component. This type of situation where the
         name is common for a proxy and proxied callback response will not be
         caught above in avnd_int_ext_comp_hdlr, bz the proxy is a local
         component, so the name cann't be found in internode_avail_comp_db.*/
      if (m_AVND_COMP_TYPE_IS_INTER_NODE(comp)) {
        /* This is an external component, so we need to forward this to another
         * AvND */

        /* We got the callback record, so before deleting it, replace the
           invocation handle in the response with the original one. Check
           function avnd_evt_avnd_avnd_cbk_msg_hdl()'s comments */
        qsc->inv = cbk_rec->orig_opq_hdl;
        avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk_rec->opq_hdl, false);

        /* We need to forward this req to other AvND */
        /* Before sending api_info, we need to overwrite the component name as
           right now, api_info->param.resp.comp_name has proxy comp name. */
        osaf_extended_name_free(&qsc->comp_name);
        osaf_extended_name_alloc(comp->name.c_str(), &qsc->comp_name);
        rc = avnd_avnd_msg_send(cb, (uint8_t *)api_info, api_info->type,
                                &evt->mds_ctxt, comp->node_id);
        if (NCSCC_RC_SUCCESS != rc) {
          LOG_ER("%s,AvND Send Failure:%s,Type=%u,Hdl=%llu,Inv:%llu, Err:%u",
                 __FUNCTION__, comp->name.c_str(), api_info->type, qsc->hdl,
                 qsc->inv, qsc->err);
        }
        goto done;
      }

      /*************************   Section 1 Ends Here
       * **************************/

    } /* if(cbk_rec) */
  }
  /* if (!cbk_rec && comp->pxied_list.n_nodes != 0) */
  if (!cbk_rec) return rc;

  if (SA_AIS_OK != qsc->err) {
    /* process comp-failure */
    err_info.src = AVND_ERR_SRC_CBK_CSI_SET_FAILED;
    err_info.rec_rcvr.avsv_ext =
        static_cast<AVSV_ERR_RCVR>(comp->err_info.def_rec);
    rc = avnd_err_process(cb, comp, &err_info);
  } else {
    if (!m_AVSV_SA_NAME_IS_NULL(
            cbk_rec->cbk_info->param.csi_set.csi_desc.csiName)) {
      csi = m_AVND_COMPDB_REC_CSI_GET(
          *comp,
          Amf::to_string(&cbk_rec->cbk_info->param.csi_set.csi_desc.csiName)
              .c_str());
      osafassert(csi);
    }

    /* indicate that this csi-assignment is over */
    rc = avnd_comp_csi_assign_done(cb, comp, csi);
    if (NCSCC_RC_SUCCESS != rc) goto done;
  }

done:
  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_evt_tmr_qscing_cmpl_evh

  Description   : This routine processes the qscing complete timer expiry.

  Arguments     : cb  - ptr to the AvND control block
                  evt - ptr to the AvND event

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_evt_tmr_qscing_cmpl_evh(struct avnd_cb_tag *cb,
                                      struct avnd_evt_tag *evt) {
  AVND_TMR_EVT *tmr = &evt->info.tmr;
  AVND_COMP *comp = 0;
  AVND_ERR_INFO err_info;
  uint32_t rc = NCSCC_RC_SUCCESS;

  TRACE_ENTER();

  /* retrieve comp record */
  comp = (AVND_COMP *)ncshm_take_hdl(NCS_SERVICE_ID_AVND, tmr->opq_hdl);
  if (!comp) {
    TRACE("COMP NULL For qscing complete tmr expiry");
    goto done;
  }
  ncshm_give_hdl(tmr->opq_hdl);
  err_info.src = AVND_ERR_SRC_QSCING_COMPL_TIMEOUT;
  err_info.rec_rcvr.avsv_ext =
      static_cast<AVSV_ERR_RCVR>(comp->err_info.def_rec);

  rc = avnd_err_process(cb, comp, &err_info);

done:
  TRACE_LEAVE();
  return rc;
}
/****************************************************************************
  Name          : avnd_evt_ava_resp

  Description   : This routine processes the AMF Response message from AvA.

  Arguments     : cb  - ptr to the AvND control block
                  evt - ptr to the AvND event

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_evt_ava_resp_evh(AVND_CB *cb, AVND_EVT *evt) {
  AVSV_AMF_API_INFO *api_info = &evt->info.ava.msg->info.api_info;
  AVSV_AMF_RESP_PARAM *resp = &api_info->param.resp;
  AVND_COMP *comp = 0;
  AVND_COMP_CBK *cbk_rec = 0;
  AVND_COMP_HC_REC *hc_rec = 0;
  AVND_COMP_CSI_REC *csi = 0;
  AVND_ERR_INFO err_info;
  uint32_t rc = NCSCC_RC_SUCCESS;
  bool msg_from_avnd = false, int_ext_comp = false;
  SaAisErrorT amf_rc = SA_AIS_OK;
  uint32_t cbk_rec_deleted = 0;

  TRACE_ENTER();

  if (AVND_EVT_AVND_AVND_MSG == evt->type) {
    /* This means that the message has come from proxy AvND to this AvND. */
    msg_from_avnd = true;
  }

  if (false == msg_from_avnd) {
    /* Check for internode or external coomponent first
       If it is, then forward it to the respective AvND. */
    rc = avnd_int_ext_comp_hdlr(cb, api_info, &evt->mds_ctxt, &amf_rc,
                                &int_ext_comp);
    if (true == int_ext_comp) {
      goto done;
    }
  }

  /*
   * Get the comp. As comp-name is derived from the AvA lib, it's
   * non-existence is a fatal error.
   */
  comp = avnd_compdb_rec_get(cb->compdb, Amf::to_string(&resp->comp_name));
  osafassert(comp);

  /* npi comps except for proxied dont interact with amf */
  if (!m_AVND_COMP_TYPE_IS_PREINSTANTIABLE(comp) &&
      !m_AVND_COMP_TYPE_IS_PROXIED(comp))
    goto done;

  /* get the matching entry from the cbk list */
  m_AVND_COMP_CBQ_INV_GET(comp, resp->inv, cbk_rec);

  /* it can be a responce from a proxied, see any proxied comp are there */
  if (!cbk_rec && comp->pxied_list.n_nodes != 0) {
    AVND_COMP_PXIED_REC *rec = 0;

    /* parse thru all proxied comp, look in the list of inv handle of each of
     * them */
    for (rec =
             (AVND_COMP_PXIED_REC *)m_NCS_DBLIST_FIND_FIRST(&comp->pxied_list);
         rec; rec = (AVND_COMP_PXIED_REC *)m_NCS_DBLIST_FIND_NEXT(
                  &rec->comp_dll_node)) {
      m_AVND_COMP_CBQ_INV_GET(rec->pxied_comp, resp->inv, cbk_rec);
      if (cbk_rec) break;
    }

    /* its a responce from proxied, update the comp ptr */
    if (cbk_rec) {
      comp = rec->pxied_comp;

      /*************************   Section  1 Starts Here
       * **************************/

      /* We have got the component name, but we are not sure this is a local
         component or internode/ext component as the proxy can register the
         proxied component with its own name i.e. proxy may use the same amf
         handle for registering the proxied component. The proxied component can
         be a local/intenode/ext component. This type of situation where the
         name is common for a proxy and proxied callback response will not be
         caught above in avnd_int_ext_comp_hdlr, bz the proxy is a local
         component, so the name cann't be found in internode_avail_comp_db.*/
      if (m_AVND_COMP_TYPE_IS_INTER_NODE(comp)) {
        /* This is an external component, so we need to forward this to another
         * AvND */

        /* We got the callback record, so before deleting it, replace the
           invocation handle in the response with the original one. Check
           function avnd_evt_avnd_avnd_cbk_msg_hdl()'s comments */
        resp->inv = cbk_rec->orig_opq_hdl;

        /* We cann't delete callback record for CSI set resp for
           "SA_AMF_HA_QUIESCING". It will be deleted when QUIESCING_COMPL
           comes. */

        if ((AVSV_AMF_CSI_SET == cbk_rec->cbk_info->type) &&
            (SA_AMF_HA_QUIESCING == cbk_rec->cbk_info->param.csi_set.ha)) {
          /* Don't delete the callback. */
        } else {
          avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk_rec->opq_hdl, false);
        }

        /* We need to forward this req to other AvND */
        /* Before sending api_info, we need to overwrite the component name as
           right now, api_info->param.resp.comp_name has proxy comp name. */
        osaf_extended_name_alloc(comp->name.c_str(), &resp->comp_name);
        rc = avnd_avnd_msg_send(cb, (uint8_t *)api_info, api_info->type,
                                &evt->mds_ctxt, comp->node_id);
        if (NCSCC_RC_SUCCESS != rc) {
          LOG_ER("%s,AvND Send Failure:%s,Type:%u,Hdl:%llu,Inv:%llu, Err:%u",
                 __FUNCTION__, comp->name.c_str(), api_info->type, resp->hdl,
                 resp->inv, resp->err);
        }
        goto done;
      }

      /*************************   Section 1 Ends Here
       * **************************/

    } /* if(cbk_rec) */
  }
  /* if (!cbk_rec && comp->pxied_list.n_nodes != 0)  */
  if (!cbk_rec) {
    TRACE_LEAVE2("Empty comp callback record comp=%s, callback type=%llx",
                 comp->name.c_str(), resp->inv);
    return rc;
  }

  switch (cbk_rec->cbk_info->type) {
    case AVSV_AMF_HC: {
      AVND_COMP_HC_REC tmp_hc_rec;
      tmp_hc_rec.key = cbk_rec->cbk_info->param.hc.hc_key;
      tmp_hc_rec.req_hdl = cbk_rec->cbk_info->hdl;
      hc_rec = m_AVND_COMPDB_REC_HC_GET(*comp, tmp_hc_rec);
      if (SA_AIS_OK != resp->err) {
        /* process comp-failure */
        if (hc_rec) {
          err_info.rec_rcvr.raw = hc_rec->rec_rcvr.raw;
          err_info.src = AVND_ERR_SRC_CBK_HC_FAILED;
          rc = avnd_err_process(cb, comp, &err_info);
        }
      } else {
        /* comp is healthy.. remove the cbk record */
        avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk_rec->opq_hdl, false);

        if (hc_rec) {
          if (hc_rec->status == AVND_COMP_HC_STATUS_SND_TMR_EXPD) {
            /* send another hc cbk */
            hc_rec->status = AVND_COMP_HC_STATUS_STABLE;
            avnd_comp_hc_rec_start(cb, comp, hc_rec);
          } else
            hc_rec->status = AVND_COMP_HC_STATUS_STABLE;
        }
      }
      break;
    }

    case AVSV_AMF_COMP_TERM:
      /* trigger comp-fsm & delete the record */
      if (SA_AIS_OK != resp->err) {
        LOG_NO("TerminateCallback failed for '%s' aisError:%u",
               comp->name.c_str(), resp->err);

        m_AVND_COMP_TERM_FAIL_SET(comp);
      }
      /* For successful response from (1.) Sa-Aware PI component, wait
         for down event to come, don't run clc here, run clc when down
         event comes. (2.) proxied PI, no down event
         expected, so run clc here. In case of failure response from
         any body, follow the clc. No need to check for PI along with
         proxied, because in case of PI only, term response will come
         back from component. */

      // XXX TODO_70: container and contained can be same process or different process.
      if ((SA_AIS_OK == resp->err) && (!m_AVND_COMP_TYPE_IS_PROXIED(comp) &&
          (comp->contained() == false))) {
        /* Save invocation value to delete cbq record when
           down event comes. */
        comp->term_cbq_inv_value = resp->inv;
      } else {
        uint32_t opq_hdl(cbk_rec->opq_hdl);
        rc = avnd_comp_clc_fsm_run(cb, comp,
                                   (SA_AIS_OK == resp->err)
                                       ? AVND_COMP_CLC_PRES_FSM_EV_TERM_SUCC
                                       : AVND_COMP_CLC_PRES_FSM_EV_CLEANUP);
        avnd_comp_cbq_rec_pop_and_del(cb, comp, opq_hdl, false);
      }

      // if all OK send a response to the client
      if ((rc == NCSCC_RC_SUCCESS) && (resp->err == SA_AIS_OK)) {
        rc = avnd_amf_resp_send(cb, AVSV_AMF_COMP_TERM_RSP, SA_AIS_OK, nullptr,
                                &api_info->dest, &evt->mds_ctxt, comp, false);
      }
      break;

    case AVSV_AMF_CSI_ATTR_CHANGE:
      csi = m_AVND_COMPDB_REC_CSI_GET(
          *comp,
          Amf::to_string(&cbk_rec->cbk_info->param.csi_attr_change.csi_name)
              .c_str());

      if (!csi)
        LOG_ER("'%s', not found",
               osaf_extended_name_borrow(
                   &cbk_rec->cbk_info->param.csi_attr_change.csi_name));
      cbk_rec_deleted =
          avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk_rec->opq_hdl, false);
      if (!cbk_rec_deleted) {
        if (m_AVND_TMR_IS_ACTIVE(cbk_rec->resp_tmr)) {
          m_AVND_TMR_COMP_CBK_RESP_STOP(cb, *cbk_rec)
        }
      }
      if (SA_AIS_OK != resp->err) {
        // generate a failure report.
        err_info.src = AVND_ERR_SRC_CBK_CSI_ATTR_CHANGE_FAILED;
        err_info.rec_rcvr.avsv_ext =
            static_cast<AVSV_ERR_RCVR>(comp->err_info.def_rec);
        rc = avnd_err_process(cb, comp, &err_info);
      }

      break;
    case AVSV_AMF_CSI_SET:

      if (m_AVND_COMP_TYPE_IS_PROXIED(comp) &&
          !m_AVND_COMP_TYPE_IS_PREINSTANTIABLE(comp)) {
        /* trigger comp-fsm & delete the record */
        uint32_t opq_hdl(cbk_rec->opq_hdl);
        rc = avnd_comp_clc_fsm_run(cb, comp,
                                   (SA_AIS_OK == resp->err)
                                       ? AVND_COMP_CLC_PRES_FSM_EV_INST_SUCC
                                       : AVND_COMP_CLC_PRES_FSM_EV_INST_FAIL);
        avnd_comp_cbq_rec_pop_and_del(cb, comp, opq_hdl, false);
        if (NCSCC_RC_SUCCESS != rc) goto done;
        break;
      }

      /* Validate this assignment response */

      /* get csi rec */
      if (!m_AVSV_SA_NAME_IS_NULL(
              cbk_rec->cbk_info->param.csi_set.csi_desc.csiName)) {
        csi = m_AVND_COMPDB_REC_CSI_GET(
            *comp,
            Amf::to_string(&cbk_rec->cbk_info->param.csi_set.csi_desc.csiName)
                .c_str());
        osafassert(csi);
      }

      /* check, if the older assignment was overriden by new one, if so trash
       * this resp */
      if (!csi) {
        AVND_COMP_CSI_REC *temp_csi = m_AVND_COMPDB_REC_CSI_GET_FIRST(*comp);
        osafassert(temp_csi != nullptr);

        if (cbk_rec->cbk_info->param.csi_set.ha != temp_csi->si->curr_state) {
          avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk_rec->opq_hdl, false);
          break;
        }
      } else if (cbk_rec->cbk_info->param.csi_set.ha != csi->si->curr_state) {
        avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk_rec->opq_hdl, false);
        break;
      } else if (m_AVND_COMP_IS_ALL_CSI(comp)) {
        avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk_rec->opq_hdl, false);
        break;
      }

      if (SA_AIS_OK != resp->err) {
        /* process comp-failure */
        err_info.src = AVND_ERR_SRC_CBK_CSI_SET_FAILED;
        err_info.rec_rcvr.avsv_ext =
            static_cast<AVSV_ERR_RCVR>(comp->err_info.def_rec);
        rc = avnd_err_process(cb, comp, &err_info);
      } else {
        if (SA_AMF_HA_QUIESCING != cbk_rec->cbk_info->param.csi_set.ha) {
          /* indicate that this csi-assignment is over */
          rc = avnd_comp_csi_assign_done(cb, comp, csi);
          if (NCSCC_RC_SUCCESS != rc) goto done;
        } else {
          /* Just stop the callback timer, Quiescing complete will come after
           * some time */
          if (m_AVND_TMR_IS_ACTIVE(cbk_rec->resp_tmr)) {
            m_AVND_TMR_COMP_CBK_RESP_STOP(cb, *cbk_rec)
          }

          /* Now Start the QSCING complete timer */
          comp->qscing_tmr.type = AVND_TMR_QSCING_CMPL_RESP;
          comp->qscing_tmr.opq_hdl = comp->comp_hdl;
          m_AVND_TMR_COMP_QSCING_CMPL_START(cb, comp);
        }
      }
      break;

    case AVSV_AMF_CSI_REM:

      if (m_AVND_COMP_TYPE_IS_PROXIED(comp) &&
          !m_AVND_COMP_TYPE_IS_PREINSTANTIABLE(comp)) {
        /* trigger comp-fsm & delete the record */
        uint32_t opq_hdl(cbk_rec->opq_hdl);
        rc = avnd_comp_clc_fsm_run(cb, comp,
                                   (SA_AIS_OK == resp->err)
                                       ? AVND_COMP_CLC_PRES_FSM_EV_TERM_SUCC
                                       : AVND_COMP_CLC_PRES_FSM_EV_CLEANUP);
        avnd_comp_cbq_rec_pop_and_del(cb, comp, opq_hdl, false);
        break;
      }

      if (!m_AVSV_SA_NAME_IS_NULL(cbk_rec->cbk_info->param.csi_rem.csi_name)) {
        csi = m_AVND_COMPDB_REC_CSI_GET(
            *comp,
            Amf::to_string(&cbk_rec->cbk_info->param.csi_rem.csi_name).c_str());
        osafassert(csi);
      }

      /* perform err prc if resp fails */
      if (SA_AIS_OK != resp->err) {
        err_info.src = AVND_ERR_SRC_CBK_CSI_REM_FAILED;
        err_info.rec_rcvr.avsv_ext =
            static_cast<AVSV_ERR_RCVR>(comp->err_info.def_rec);
        rc = avnd_err_process(cb, comp, &err_info);
      }

      /* indicate that this csi-removal is over */
      if (comp->csi_list.n_nodes) {
        rc = avnd_comp_csi_remove_done(cb, comp, csi);
        if (NCSCC_RC_SUCCESS != rc) goto done;
      }

      /*  cancel qscing_tmr in case this is this called
      during an admin shutdown etc.. */
      if (m_AVND_TMR_IS_ACTIVE(comp->qscing_tmr)) {
        m_AVND_TMR_COMP_QSCING_CMPL_STOP(cb, comp);
      }

      break;

    case AVSV_AMF_PXIED_COMP_INST: {
        /* trigger comp-fsm & delete the record */
        uint32_t opq_hdl(cbk_rec->opq_hdl);
        rc = avnd_comp_clc_fsm_run(cb, comp,
                                   (SA_AIS_OK == resp->err)
                                       ? AVND_COMP_CLC_PRES_FSM_EV_INST_SUCC
                                       : AVND_COMP_CLC_PRES_FSM_EV_INST_FAIL);
        avnd_comp_cbq_rec_pop_and_del(cb, comp, opq_hdl, false);
      }
      break;

    case AVSV_AMF_PXIED_COMP_CLEAN: {
        /* trigger comp-fsm & delete the record */
        uint32_t opq_hdl(cbk_rec->opq_hdl);
        rc = avnd_comp_clc_fsm_run(cb, comp,
                                   (SA_AIS_OK == resp->err)
                                       ? AVND_COMP_CLC_PRES_FSM_EV_CLEANUP_SUCC
                                       : AVND_COMP_CLC_PRES_FSM_EV_CLEANUP_FAIL);
        avnd_comp_cbq_rec_pop_and_del(cb, comp, opq_hdl, false);
      }
      break;

    case AVSV_AMF_CONTAINED_COMP_INST: {
      AVND_COMP_CLC_PRES_FSM_EV event(AVND_COMP_CLC_PRES_FSM_EV_INST_SUCC);

      uint32_t opq_hdl(cbk_rec->opq_hdl);
      if (resp->err == SA_AIS_ERR_TRY_AGAIN)
        event = AVND_COMP_CLC_PRES_FSM_EV_INST_TRY_AGAIN;
      else if (resp->err != SA_AIS_OK)
        event = AVND_COMP_CLC_PRES_FSM_EV_INST_FAIL;
      else
        m_AVND_COMP_INST_CMD_SUCC_SET(comp);

      // contained component is sa-aware so we need to wait for registration
      if (event != AVND_COMP_CLC_PRES_FSM_EV_INST_SUCC ||
          m_AVND_COMP_IS_REG(comp)) {
        rc = avnd_comp_clc_fsm_run(cb, comp, event);
      } else
        m_AVND_TMR_COMP_REG_START(cb, *comp, rc);

      avnd_comp_cbq_rec_pop_and_del(cb, comp, opq_hdl, false);
      break;
    }

    case AVSV_AMF_CONTAINED_COMP_CLEAN: {
      uint32_t opq_hdl(cbk_rec->opq_hdl);
      rc = avnd_comp_clc_fsm_run(cb, comp,
                                 (SA_AIS_OK == resp->err)
                                     ? AVND_COMP_CLC_PRES_FSM_EV_CLEANUP_SUCC
                                     : AVND_COMP_CLC_PRES_FSM_EV_CLEANUP_FAIL);
      avnd_comp_cbq_rec_pop_and_del(cb, comp, opq_hdl, false);
      break;
    }

    case AVSV_AMF_PG_TRACK:
    default:
      osafassert(0);
  } /* switch */

done:
  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_evt_tmr_cbk_resp

  Description   : This routine processes the callback response timer expiry.

  Arguments     : cb  - ptr to the AvND control block
                  evt - ptr to the AvND event

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE

  Notes         : None.
******************************************************************************/
uint32_t avnd_evt_tmr_cbk_resp_evh(AVND_CB *cb, AVND_EVT *evt) {
  AVND_TMR_EVT *tmr = &evt->info.tmr;
  AVND_COMP_CBK *rec = 0;
  AVND_COMP_HC_REC *hc_rec = 0;
  AVND_ERR_INFO err_info;
  uint32_t rc = NCSCC_RC_SUCCESS;

  TRACE_ENTER();

  /* retrieve callback record */
  rec = (AVND_COMP_CBK *)ncshm_take_hdl(NCS_SERVICE_ID_AVND, tmr->opq_hdl);
  if (!rec) goto done;

  if (NCSCC_RC_SUCCESS ==
      m_AVND_CHECK_FOR_STDBY_FOR_EXT_COMP(cb, rec->comp->su->su_is_external))
    goto done;

  /*
   * the record may be deleted as a part of the expiry processing.
   * hence returning the record to the hdl mngr.
   */
  ncshm_give_hdl(tmr->opq_hdl);

  if (AVSV_AMF_PXIED_COMP_INST == rec->cbk_info->type) {
    rc = avnd_comp_clc_fsm_run(cb, rec->comp,
                               AVND_COMP_CLC_PRES_FSM_EV_INST_FAIL);
  } else if (AVSV_AMF_PXIED_COMP_CLEAN == rec->cbk_info->type) {
    rc = avnd_comp_clc_fsm_run(cb, rec->comp,
                               AVND_COMP_CLC_PRES_FSM_EV_CLEANUP_FAIL);
  } else if (AVSV_AMF_COMP_TERM == rec->cbk_info->type) {
    m_AVND_COMP_TERM_FAIL_SET(rec->comp);
    rc =
        avnd_comp_clc_fsm_run(cb, rec->comp, AVND_COMP_CLC_PRES_FSM_EV_CLEANUP);
    if (rec->comp->term_cbq_inv_value != 0) {
      AVND_COMP_CBK *cbk_rec;
      /* Since, the cbq timer has expired and no down event
         came, get the matching entry from the cbk list and
         delete the cbq. */
      m_AVND_COMP_CBQ_INV_GET(rec->comp, rec->comp->term_cbq_inv_value,
                              cbk_rec);
      rec->comp->term_cbq_inv_value = 0;
      if (cbk_rec) {
        avnd_comp_cbq_rec_pop_and_del(cb, rec->comp, cbk_rec->opq_hdl, false);
      }
    }
  } else {
    switch (rec->cbk_info->type) {
      case AVSV_AMF_HC:
        err_info.src = AVND_ERR_SRC_CBK_HC_TIMEOUT;
        break;
      case AVSV_AMF_CSI_SET:
        err_info.src = AVND_ERR_SRC_CBK_CSI_SET_TIMEOUT;
        break;
      case AVSV_AMF_CSI_REM:
        err_info.src = AVND_ERR_SRC_CBK_CSI_REM_TIMEOUT;
        break;
      case AVSV_AMF_CSI_ATTR_CHANGE:
        err_info.src = AVND_ERR_SRC_CBK_CSI_ATTR_CHANGE_TIMEOUT;
        break;
      default:
        LOG_ER("%s,%u,type=%u", __FUNCTION__, __LINE__, rec->cbk_info->type);
        err_info.src = AVND_ERR_SRC_CBK_CSI_SET_TIMEOUT;
        break;
    }
    /* treat it as comp failure (determine the recommended recovery) */
    if (AVSV_AMF_HC == rec->cbk_info->type) {
      AVND_COMP_HC_REC tmp_hc_rec = {};
      tmp_hc_rec.key = rec->cbk_info->param.hc.hc_key;
      tmp_hc_rec.req_hdl = rec->cbk_info->hdl;
      hc_rec = m_AVND_COMPDB_REC_HC_GET(*(rec->comp), tmp_hc_rec);
      if (!hc_rec) goto done;
      err_info.rec_rcvr.raw = hc_rec->rec_rcvr.raw;
    } else
      err_info.rec_rcvr.avsv_ext =
          static_cast<AVSV_ERR_RCVR>(rec->comp->err_info.def_rec);

    rc = avnd_err_process(cb, rec->comp, &err_info);
  }

done:
  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_comp_cbq_send

  Description   : This routine sends AMF callback parameters to the component.

  Arguments     : cb        - ptr to the AvND control block
                  comp      - ptr to the component
                  dest      - ptr to the mds-dest of the prc to which the msg
                              is sent. If 0, the msg is sent to the
                              registering process.
                  hdl       - AMF handle (0 if reg hdl)
                  cbk_info  - ptr to the callback parameter
                  timeout   - time period within which the appl should
                              respond.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.

  Notes         : The calling process allocates memory for callback parameter.
                  It is used in the pending callback list & freed when the
                  corresponding callback record is freed. However, if this
                  routine returns failure, then the calling routine frees the
                  memory.
******************************************************************************/
uint32_t avnd_comp_cbq_send(AVND_CB *cb, AVND_COMP *comp, MDS_DEST *dest,
                            SaAmfHandleT hdl, AVSV_AMF_CBK_INFO *cbk_info,
                            SaTimeT timeout) {
  AVND_COMP_CBK *rec = 0;
  MDS_DEST mds_dest;
  uint32_t rc = NCSCC_RC_SUCCESS;

  TRACE_ENTER();
  /* determine the mds-dest */
  mds_dest = (dest) ? *dest : comp->reg_dest;

  /* add & send the new record */
  if ((0 !=
       (rec = avnd_comp_cbq_rec_add(cb, comp, cbk_info, &mds_dest, timeout)))) {
    /* assign inv & hdl values */
    rec->cbk_info->inv = rec->opq_hdl;
    rec->cbk_info->hdl = ((!hdl) ? comp->reg_hdl : hdl);

    /* send the request if comp is not in orphaned state.
       in case of orphaned component we will send it later when
       comp moves back to instantiated. we will free it, if the comp
       doesnt move to instantiated */
    if (!m_AVND_COMP_PRES_STATE_IS_ORPHANED(comp))
      rc = avnd_comp_cbq_rec_send(cb, comp, rec, true);
  } else
    rc = NCSCC_RC_FAILURE;

  if (NCSCC_RC_SUCCESS != rc && rec) {
    /* pop & delete */
    uint32_t found;

    rec = avnd_comp_cbq_rec_pop(comp, rec->opq_hdl, found);
    rec->cbk_info = 0;
    if (found) avnd_comp_cbq_rec_del(cb, comp, rec);
  }

  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_comp_cbq_rec_send

  Description   : This routine sends AMF callback parameters to the
                  appropriate process in the component. It allocates the AvA
                  message & callback info. The callback parameters are
                  copied from the callbk record & then sent to AvA.

  Arguments     : cb   - ptr to the AvND control block
                  comp - ptr to the component
                  rec  - ptr to the callback record
                     timer_start - Whether we need to start the callback
                     timer or not. We need not start timer
                     in the case when the callbk has come
                   from AvND and we need to forward this
                   to AvA.
                  timer_start - There is no need to start the timer in case
                         when the callback has come from pxd AvND. We need to
                         forward the callback to pxy and get the response and
                         send it back to pxd AvND. So, if it is true then we
                         understand that this message has to be forwarded to
                         local only as comp's nodeid with match this AvND's.

  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.

  Notes         : None
******************************************************************************/
uint32_t avnd_comp_cbq_rec_send(AVND_CB *cb, AVND_COMP *comp,
                                AVND_COMP_CBK *rec, bool timer_start) {
  AVND_MSG msg;
  AVSV_ND2ND_AVND_MSG *avnd_msg = nullptr;
  AVSV_NDA_AVA_MSG *temp_ptr = nullptr;

  TRACE_ENTER();
  memset(&msg, 0, sizeof(AVND_MSG));

  /* allocate ava message */
  // zero-initialized (new T() POD types are value-initialized, c++98, 03 and
  // 11)
  msg.info.ava =
      static_cast<AVSV_NDA_AVA_MSG *>(calloc(1, sizeof(AVSV_NDA_AVA_MSG)));

  /* populate the msg */
  msg.type = AVND_MSG_AVA;
  msg.info.ava->type = AVSV_AVND_AMF_CBK_MSG;
  uint32_t rc = avsv_amf_cbk_copy(&msg.info.ava->info.cbk_info, rec->cbk_info);
  if (NCSCC_RC_SUCCESS != rc) goto done;

  /* Check wether we need to send this to local AvA or another AvND.
     Since proxy can be at another AvND, so we need to send to that AvND */
  if (((cb->node_info.nodeId != m_NCS_NODE_ID_FROM_MDS_DEST(rec->dest) &&
        !comp->contained()) ||
       (m_AVND_COMP_TYPE_IS_EXT_CLUSTER(comp))) &&
      (true == timer_start)) {
    /* Since the node id of the message to be sent differs, so send it to
       another      AvND. Or this may be a case of cluster component at
       controller proxying to an external component, in this case, node id will
       match, but since we need to treat "ctrl proxy - external component
       proxied" as internode scenario, so we need to put a check of external
       component also. */
    NODE_ID node_id = 0;
    MDS_DEST i_to_dest = 0;
    avnd_msg = static_cast<AVSV_ND2ND_AVND_MSG *>(
        calloc(1, sizeof(AVSV_ND2ND_AVND_MSG)));

    osaf_extended_name_alloc(comp->name.c_str(), &avnd_msg->comp_name);
    temp_ptr = msg.info.ava;
    msg.info.avnd = avnd_msg;
    /* Hijack the message of AvA and make it a part of AvND. */
    msg.info.avnd->info.msg = temp_ptr;
    msg.info.avnd->type = AVND_AVND_AVA_MSG;
    msg.type = AVND_MSG_AVND;
    /* Send it to AvND */
    node_id = m_NCS_NODE_ID_FROM_MDS_DEST(rec->dest);
    i_to_dest = avnd_get_mds_dest_from_nodeid(cb, node_id);
    rc = avnd_avnd_mds_send(cb, i_to_dest, &msg);
    if (NCSCC_RC_SUCCESS != rc) {
      LOG_ER("avnd_comp_cbq_rec_send:Msg Send to AvND Failed:%s, %x",
             comp->name.c_str(), node_id);
    }
  } else {
    /* send the message to AvA */
    rc = avnd_mds_send(cb, &msg, &rec->dest, 0);
  }

  if (NCSCC_RC_SUCCESS == rc) {
    /* start the callback response timer */
    if (true == timer_start) {
      m_AVND_TMR_COMP_CBK_RESP_START(cb, *rec, rec->timeout, rc);
      /* Not sure why someone has written the following line. */
      msg.info.ava = 0;
    }
  }

done:
  /* free the contents of avnd message */
  avnd_msg_content_free(cb, &msg);
  TRACE_LEAVE();
  return rc;
}

/****************************************************************************
  Name          : avnd_comp_cbq_del

  Description   : This routine clears the pending callback list.

  Arguments     : cb   - ptr to the AvND control block
                  comp - ptr to the component
                  send_del_cbk - true if the callback is tobe deleted and
                                 an event can be sent to another AvND.

  Return Values : None.

  Notes         : None.
******************************************************************************/
void avnd_comp_cbq_del(AVND_CB *cb, AVND_COMP *comp, bool send_del_cbk) {
  AVND_COMP_CBK *rec = 0;
  NODE_ID dest_node_id = 0;
  TRACE_ENTER2("Comp '%s', send_del_cbk '%u'", comp->name.c_str(),
               send_del_cbk);

  do {
    /* pop the record */
    m_AVND_COMP_CBQ_START_POP(comp, rec);
    if (!rec) break;

    /* Check if del cbk is to be sent. */
    if (true == send_del_cbk) {
      /* Check that the callback was sent to another AvND. */
      dest_node_id = m_NCS_NODE_ID_FROM_MDS_DEST(rec->dest);
      if (cb->node_info.nodeId != dest_node_id) {
        /* This means that the callback was given to another AvND.
           So, this means that we need to send a del message to the
           same AvND as this may remain stale in case we don't sent
           and there is no response from AvA. */
        avnd_avnd_cbk_del_send(cb, comp->name, &rec->opq_hdl, &dest_node_id);

      } /* if(cb->node_info.nodeId != dest_node_id) */
    }

    /* if(true == send_del_cbk) */
    /* delete the record */
    avnd_comp_cbq_rec_del(cb, comp, rec);
  } while (1);
  TRACE_LEAVE();
  return;
}

/****************************************************************************
  Name          : avnd_comp_cbq_rec_pop_and_del

  Description   : This routine pops & deletes a record from the pending
                  callback list.

  Arguments     : cb   - ptr to the AvND control block
                  comp - ptr to the component
                  rec  - ptr to the callback record that is to be popped &
                         deleted.
                  send_del_cbk - true if the callback is tobe deleted and
                                 an event can be sent to another AvND.

  Return Values : true if found and delete the record
                  false if not found the record.

  Notes         : None.
******************************************************************************/
uint32_t avnd_comp_cbq_rec_pop_and_del(AVND_CB *cb, AVND_COMP *comp,
                                   uint32_t opq_hdl, bool send_del_cbk) {
  uint32_t found;
  NODE_ID dest_node_id = 0;

  /* pop the record */
  AVND_COMP_CBK *rec(avnd_comp_cbq_rec_pop(comp, opq_hdl, found));

  if (found) {
    /* Check if del cbk is to be sent. */
    if (true == send_del_cbk) {
      /* Check that the callback was sent to another AvND. */
      dest_node_id = m_NCS_NODE_ID_FROM_MDS_DEST(rec->dest);
      if (cb->node_info.nodeId != dest_node_id) {
        /* This means that the callback was given to another AvND.
           So, this means that we need to send a del message to the
           same AvND as this may remain stale in case we don't sent
           and there is no response from AvA. */
        (void)avnd_avnd_cbk_del_send(cb, comp->name, &rec->opq_hdl,
                                     &dest_node_id);

      } /* if(cb->node_info.nodeId != dest_node_id) */
    }   /* if(true == send_del_cbk) */
    avnd_comp_cbq_rec_del(cb, comp, rec);
  } /* if(found) */
  return found;
}

/****************************************************************************
  Name          : avnd_comp_cbq_rec_add

  Description   : This routine adds a record to the pending callback list.

  Arguments     : cb   - ptr to the AvND control block
                  comp - ptr to the component
                  cbk_info - ptr to the callback info
                  dest     - ptr to the mds-dest of the process to which
                             callback params are sent
                  timeout  - timeout value

  Return Values : ptr to the newly added callback record

  Notes         : None.
******************************************************************************/
AVND_COMP_CBK *avnd_comp_cbq_rec_add(AVND_CB *cb, AVND_COMP *comp,
                                     AVSV_AMF_CBK_INFO *cbk_info,
                                     MDS_DEST *dest, SaTimeT timeout) {
  AVND_COMP_CBK *rec = 0;

  rec = new AVND_COMP_CBK();

  /* create the association with hdl-mngr */
  if ((0 == (rec->opq_hdl = ncshm_create_hdl(cb->pool_id, NCS_SERVICE_ID_AVND,
                                             (NCSCONTEXT)rec))))
    goto error;

  /* assign the record params */
  rec->comp = comp;
  rec->cbk_info = cbk_info;
  rec->dest = *dest;
  rec->timeout = timeout;
  rec->comp_name = comp->name;

  /* push the record to the pending callback list */
  m_AVND_COMP_CBQ_START_PUSH(comp, rec);

  return rec;

error:
  if (rec) avnd_comp_cbq_rec_del(cb, comp, rec);

  return 0;
}

/****************************************************************************
  Name          : avnd_comp_cbq_rec_del

  Description   : This routine clears the record in the pending callback list.

  Arguments     : cb   - ptr to the AvND control block
                  comp - ptr to the component
                  rec  - ptr to the callback record

  Return Values : None.

  Notes         : None.
******************************************************************************/
void avnd_comp_cbq_rec_del(AVND_CB *cb, AVND_COMP *comp, AVND_COMP_CBK *rec) {
  /* remove the association with hdl-mngr */
  if (rec->opq_hdl) {
    if (rec->red_opq_hdl) {
      /* This is non-zero, this means that this data has been checkpointed
         from ACT AvND to STDBY AvND, so we need to destroy the handle
         generated at STDBY AvND and not the handle generated at ACT AvND. */
      ncshm_destroy_hdl(NCS_SERVICE_ID_AVND, rec->red_opq_hdl);
    } else
      ncshm_destroy_hdl(NCS_SERVICE_ID_AVND, rec->opq_hdl);
  }

  /* stop the callback response timer */
  if (m_AVND_TMR_IS_ACTIVE(rec->resp_tmr))
    m_AVND_TMR_COMP_CBK_RESP_STOP(cb, *rec);

  /* free the callback info */
  if (rec->cbk_info) avsv_amf_cbk_free(rec->cbk_info);

  /* free the record */
  delete rec;

  return;
}

/****************************************************************************
  Name          : avnd_comp_cbq_finalize

  Description   : This routine removes all the component callback records
                  that share the specified AMF handle & the mds-dest. It is
                  invoked when the application invokes saAmfFinalize for a
                  certain handle.

  Arguments     : cb   - ptr to the AvND control block
                  comp - ptr the the component
                  hdl  - amf-handle
                  dest - ptr to mds-dest (of the prc that finalize)

  Return Values : None.

  Notes         : None.
******************************************************************************/
void avnd_comp_cbq_finalize(AVND_CB *cb, AVND_COMP *comp, SaAmfHandleT hdl,
                            MDS_DEST *dest) {
  AVND_COMP_CBK *curr = (comp)->cbk_list, *prv = 0;

  /* scan the entire comp-cbk list & delete the matching records */
  while (curr) {
    if ((curr->cbk_info->hdl == hdl) &&
        !memcmp(&curr->dest, dest, sizeof(MDS_DEST))) {
      if (curr->cbk_info && (curr->cbk_info->type == AVSV_AMF_COMP_TERM) &&
          (!m_AVND_COMP_TYPE_IS_PROXIED(comp))) {
        m_AVND_COMP_TERM_FAIL_SET(comp);
        avnd_comp_clc_fsm_run(cb, comp, AVND_COMP_CLC_PRES_FSM_EV_CLEANUP);
      }

      avnd_comp_cbq_rec_pop_and_del(cb, comp, curr->opq_hdl, true);
      curr = (prv) ? prv->next : comp->cbk_list;
    } else {
      prv = curr;
      curr = curr->next;
    }
  } /* while */

  return;
}

/****************************************************************************
  Name          : avnd_comp_cbq_csi_rec_del

  Description   : This routine removes a csi-set / csi-remove callback record
                  for the specified CSI.

  Arguments     : cb           - ptr to the AvND control block
                  comp         - ptr the the component
                  csi_name     - ptr to the csi-name

  Return Values : None.

  Notes         : None.
******************************************************************************/
void avnd_comp_cbq_csi_rec_del(AVND_CB *cb, AVND_COMP *comp,
                               const std::string &csi_name) {
  AVND_COMP_CBK *curr = comp->cbk_list, *prv = 0;
  AVSV_AMF_CBK_INFO *info = 0;
  bool to_del = false;

  TRACE_ENTER2("'%s', '%s'", comp->name.c_str(), csi_name.c_str());

  /* scan the entire comp-cbk list & delete the matching records */
  while (curr) {
    info = curr->cbk_info;

    if (csi_name.empty()) {
      /* => remove all the csi-set & csi-rem cbks */
      if ((AVSV_AMF_CSI_SET == info->type) || (AVSV_AMF_CSI_REM == info->type))
        to_del = true;
    } else {
      /* => remove only the matching csi-set & csi-rem cbk */
      if (((AVSV_AMF_CSI_SET == info->type) &&
           (0 == csi_name.compare(
                     Amf::to_string(&info->param.csi_set.csi_desc.csiName)))) ||
          ((AVSV_AMF_CSI_REM == info->type) &&
           (0 ==
            csi_name.compare(Amf::to_string(&info->param.csi_rem.csi_name)))) ||
          ((AVSV_AMF_CSI_ATTR_CHANGE == info->type) &&
           (0 == csi_name.compare(
                     Amf::to_string(&info->param.csi_attr_change.csi_name)))))

        to_del = true;
    }

    if (true == to_del) {
      avnd_comp_cbq_rec_pop_and_del(cb, comp, curr->opq_hdl, true);
      curr = (prv) ? prv->next : comp->cbk_list;
    } else {
      prv = curr;
      curr = curr->next;
    }
    to_del = false;
  }

  TRACE_LEAVE();
}

/****************************************************************************
  Name          : avnd_comp_unreg_cbk_process

  Description   : This routine deletes
                  uses .

  Arguments     : cb   - ptr to the AvND control block
                  comp - ptr to the proxy of reg comp

  Return Values : None.

  Notes         : None.
******************************************************************************/

void avnd_comp_unreg_cbk_process(AVND_CB *cb, AVND_COMP *comp) {
  AVND_COMP_CBK *cbk = 0, *temp_cbk_list = 0, *head = 0;
  AVND_COMP_CSI_REC *csi = 0;

  while ((comp->cbk_list != nullptr) && (comp->cbk_list != cbk)) {
    cbk = comp->cbk_list;

    switch (cbk->cbk_info->type) {
      case AVSV_AMF_HC:
      case AVSV_AMF_COMP_TERM: {
        uint32_t found = false;

        avnd_comp_cbq_rec_pop(comp, cbk->opq_hdl, found);

        if (!found)
          LOG_NO("%s - '%s' type:%u", __FUNCTION__, comp->name.c_str(),
                 cbk->cbk_info->type);

        cbk->next = nullptr;

        /*  add this rec on to temp_cbk_list */
        {
          if (head == 0) {
            head = cbk;
            temp_cbk_list = cbk;
          } else {
            temp_cbk_list->next = cbk;
            temp_cbk_list = cbk;
          }
        }

      } break;

      case AVSV_AMF_CSI_SET: {
        csi = m_AVND_COMPDB_REC_CSI_GET(
            *comp,
            Amf::to_string(&cbk->cbk_info->param.csi_set.csi_desc.csiName)
                .c_str());

        /* check, if the older assignment was overriden by new one, if so trash
         * this resp */
        if (!csi) {
          AVND_COMP_CSI_REC *temp_csi = m_AVND_COMPDB_REC_CSI_GET_FIRST(*comp);
          osafassert(temp_csi != nullptr);

          if (cbk->cbk_info->param.csi_set.ha != temp_csi->si->curr_state) {
            avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk->opq_hdl, true);
            break;
          }
        } else if (cbk->cbk_info->param.csi_set.ha != csi->si->curr_state) {
          avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk->opq_hdl, true);
          break;
        } else if (m_AVND_COMP_IS_ALL_CSI(comp)) {
          avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk->opq_hdl, true);
          break;
        }

        (void)avnd_comp_csi_assign_done(cb, comp, csi);
      } break;

      case AVSV_AMF_CSI_REM: {
        csi = m_AVND_COMPDB_REC_CSI_GET(
            *comp,
            Amf::to_string(&cbk->cbk_info->param.csi_rem.csi_name).c_str());
        if (comp->csi_list.n_nodes) {
          (void)avnd_comp_csi_remove_done(cb, comp, csi);
        } else {
          avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk->opq_hdl, true);
        }

      } break;

      case AVSV_AMF_PG_TRACK:
      case AVSV_AMF_PXIED_COMP_INST:
      case AVSV_AMF_PXIED_COMP_CLEAN:

      default: {
        /* pop and delete this records */
        avnd_comp_cbq_rec_pop_and_del(cb, comp, cbk->opq_hdl, true);
      } break;
    }

  } /* while */

  /* copy the health check callback's back to cbk_list */
  comp->cbk_list = head;
}
