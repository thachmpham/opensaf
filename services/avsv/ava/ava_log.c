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
..............................................................................



..............................................................................

  DESCRIPTION:

  This file contains AvA logging utility routines.
..............................................................................

  FUNCTIONS INCLUDED in this module:
  

******************************************************************************
*/

#include "ava.h"


#if ( NCS_AVA_LOG == 1 )
/****************************************************************************
  Name          : ava_log_seapi
 
  Description   : This routine logs the SE-API info.
                  
  Arguments     : op     - se-api operation
                  status - status of se-api operation execution
                  sev    - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_seapi (AVSV_LOG_SEAPI op, AVSV_LOG_SEAPI status, uns8 sev)
{
   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_SEAPI, AVA_FC_SEAPI, 
              NCSFL_LC_HEADLINE, sev, NCSFL_TYPE_TII, op, status);

   return;
}


/****************************************************************************
  Name          : ava_log_mds
 
  Description   : This routine logs the MDS info.
                  
  Arguments     : op     - mds operation
                  status - status of mds operation execution
                  sev    - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_mds (AVSV_LOG_MDS op, AVSV_LOG_MDS status, uns8 sev)
{
   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_MDS, AVA_FC_MDS, 
              NCSFL_LC_HEADLINE, sev, NCSFL_TYPE_TII, op, status);

   return;
}



/****************************************************************************
  Name          : ava_log_edu
 
  Description   : This routine logs the EDU info.
                  
  Arguments     : op     - edu operation
                  status - status of edu operation execution
                  sev    - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_edu (AVSV_LOG_EDU op, AVSV_LOG_EDU status, uns8 sev)
{
   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_EDU, AVA_FC_EDU, 
              NCSFL_LC_HEADLINE, sev, NCSFL_TYPE_TII, op, status);

   return;
}



/****************************************************************************
  Name          : ava_log_lock
 
  Description   : This routine logs the LOCK info.
                  
  Arguments     : op     - lock operation
                  status - status of lock operation execution
                  sev    - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_lock (AVSV_LOG_LOCK op, AVSV_LOG_LOCK status, uns8 sev)
{
   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_LOCK, AVA_FC_LOCK, 
              NCSFL_LC_LOCKS, sev, NCSFL_TYPE_TII, op, status);

   return;
}



/****************************************************************************
  Name          : ava_log_cb
 
  Description   : This routine logs the control block info.
                  
  Arguments     : op     - cb operation
                  status - status of cb operation execution
                  sev    - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_cb (AVSV_LOG_CB op, AVSV_LOG_CB status, uns8 sev)
{
   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_CB, AVA_FC_CB, 
              NCSFL_LC_HEADLINE, sev, NCSFL_TYPE_TII, op, status);

   return;
}



/****************************************************************************
  Name          : ava_log_cbk
 
  Description   : This routine logs the callback info.
                  
  Arguments     : type      - cbk type
                  comp_name - ptr to the comp-name
                  sev       - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_cbk (AVSV_LOG_AMF_CBK type, SaNameT *comp_name, uns8 sev)
{
   uns8 name[SA_MAX_NAME_LENGTH];

   memset(name, '\0', SA_MAX_NAME_LENGTH);

   /* convert comp-name into string format */
   if (comp_name) 
      m_NCS_STRNCPY(name, comp_name->value, m_NCS_OS_NTOHS(comp_name->length));

   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_CBK, AVA_FC_CBK, 
              NCSFL_LC_API, sev, NCSFL_TYPE_TIC, type - 1, name);

   return;
}



/****************************************************************************
  Name          : ava_log_sel_obj
 
  Description   : This routine logs the selection object info.
                  
  Arguments     : op     - sel-obj operation
                  status - status of sel-obj operation execution
                  sev    - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_sel_obj (AVA_LOG_SEL_OBJ op, AVA_LOG_SEL_OBJ status, uns8 sev)
{
   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_SEL_OBJ, AVA_FC_SEL_OBJ, 
              NCSFL_LC_HEADLINE, sev, NCSFL_TYPE_TII, op, status);

   return;
}


/****************************************************************************
  Name          : ava_log_api
 
  Description   : This routine logs the AMF API info.
                  
  Arguments     : type      - API type
                  status    - status of API execution
                  comp_name - ptr to the comp-name
                  sev       - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_api (AVA_LOG_API   type, 
                  AVA_LOG_API   status, 
                  const SaNameT *comp_name, 
                  uns8          sev)
{
   uns8 name[SA_MAX_NAME_LENGTH];

   memset(name, '\0', SA_MAX_NAME_LENGTH);

   /* convert comp-name into string format */
   if (comp_name) 
      m_NCS_STRNCPY(name, comp_name->value, comp_name->length);

   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_API, AVA_FC_API, 
              NCSFL_LC_API, sev, NCSFL_TYPE_TIIC, type-1, status, name);

   return;
}


/****************************************************************************
  Name          : ava_log_hdl_db
 
  Description   : This routine logs the handle database info.
                  
  Arguments     : op     - hdl-db operation
                  status - status of hdl-db operation execution
                  hdl    - amf-hdl
                  sev    - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_hdl_db (AVA_LOG_HDL_DB op, 
                     AVA_LOG_HDL_DB status, 
                     uns32          hdl, 
                     uns8           sev)
{
   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_HDL_DB, AVA_FC_HDL_DB, 
              NCSFL_LC_HEADLINE, sev, NCSFL_TYPE_TIIL, op, status, hdl);

   return;
}


/****************************************************************************
  Name          : ava_log_misc
 
  Description   : This routine logs miscellaneous info.
                  
  Arguments     : op  - misc operation
                  sev - severity
 
  Return Values : None
 
  Notes         : None.
 *****************************************************************************/
void ava_log_misc (AVA_LOG_MISC op, uns8 sev)
{
   ncs_logmsg(NCS_SERVICE_ID_AVA, AVA_LID_MISC, AVA_FC_MISC, 
              NCSFL_LC_HEADLINE, sev, NCSFL_TYPE_TI, op);

   return;
}


/****************************************************************************
  Name          : ava_log_reg
 
  Description   : This routine registers AvA with flex log service.
                  
  Arguments     : None.
 
  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE
 
  Notes         : None.
 *****************************************************************************/
uns32 ava_log_reg (void)
{
   NCS_DTSV_RQ reg;
   uns32       rc = NCSCC_RC_SUCCESS;

   memset(&reg,0,sizeof(NCS_DTSV_RQ));

   reg.i_op = NCS_DTSV_OP_BIND;
   reg.info.bind_svc.svc_id = NCS_SERVICE_ID_AVA;
   reg.info.bind_svc.version = AVSV_LOG_VERSION;
   strcpy(reg.info.bind_svc.svc_name, "AvSv"); 

   rc = ncs_dtsv_su_req(&reg);

   return rc;
}


/****************************************************************************
  Name          : ava_log_unreg
 
  Description   : This routine unregisters AvA with flex log service.
                  
  Arguments     : None.
 
  Return Values : NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE
 
  Notes         : None.
 *****************************************************************************/
uns32 ava_log_unreg (void)
{
   NCS_DTSV_RQ reg;
   uns32       rc = NCSCC_RC_SUCCESS;

   memset(&reg,0,sizeof(NCS_DTSV_RQ));

   reg.i_op = NCS_DTSV_OP_UNBIND;
   reg.info.bind_svc.svc_id = NCS_SERVICE_ID_AVA;

   rc = ncs_dtsv_su_req(&reg);

   return rc;
}
#endif /* NCS_AVA_LOG == 1 */


