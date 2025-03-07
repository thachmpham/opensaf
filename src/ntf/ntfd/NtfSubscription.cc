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
 * Author(s): Ericsson AB
 *
 */

/**
 *   This object handles information about NTF subscriptions.
 */

#include "ntf/ntfd/NtfSubscription.h"
#include "base/logtrace.h"
#include "ntf/common/ntfsv_mem.h"
#include "ntf/ntfd/NtfClient.h"
#include "ntfs.h"

#if DISCARDED_TEST
/* TODO REMOVE TEST */
extern int disc_test_cntr;
#endif

/**
 * This is the constructor.
 *
 * Initial variables are set and pointer to the filter object is stored.
 *
 * @param ntfsv_subscribe_req_t s
 *           struct received from subscribe request.
 * @param subscriptionId
 *               Client-wide unique id of the subscription.
 * @param filter Pointer to the filter object.
 */
NtfSubscription::NtfSubscription(ntfsv_subscribe_req_t* s)
    : subscriptionId_(s->subscriptionId), s_info_(*s) {
  TRACE_2("Subscription %u created for client_id %u", subscriptionId_,
          s->client_id);
  if (s->f_rec.alarm_filter) {
    NtfFilter* filter = new NtfAlarmFilter(s->f_rec.alarm_filter);
    filterMap[filter->type()] = filter;
    TRACE_2(
        "Filter type %#x p=%p, added to subscription %u, filterMap size is %u",
        filter->type(), filter, subscriptionId_,
        (unsigned int)filterMap.size());
  }
  if (s->f_rec.sec_al_filter) {
    NtfFilter* filter = new NtfSecurityAlarmFilter(s->f_rec.sec_al_filter);
    filterMap[filter->type()] = filter;
    TRACE_2("Filter type %#x added to subscription %u, filterMap size is %u",
            filter->type(), subscriptionId_, (unsigned int)filterMap.size());
  }
  if (s->f_rec.obj_cr_del_filter) {
    NtfFilter* filter =
        new NtfObjectCreateDeleteFilter(s->f_rec.obj_cr_del_filter);
    filterMap[filter->type()] = filter;
    TRACE_2("Filter type %#x added to subscription %u, filterMap size is %u",
            filter->type(), subscriptionId_, (unsigned int)filterMap.size());
  }
  if (s->f_rec.att_ch_filter) {
    NtfFilter* filter = new NtfAttributeChangeFilter(s->f_rec.att_ch_filter);
    filterMap[filter->type()] = filter;
    TRACE_2("Filter type %#x added to subscription %u, filterMap size is %u",
            filter->type(), subscriptionId_, (unsigned int)filterMap.size());
  }
  if (s->f_rec.sta_ch_filter) {
    NtfFilter* filter = new NtfStateChangeFilter(s->f_rec.sta_ch_filter);
    filterMap[filter->type()] = filter;
    TRACE_2("Filter type %#x added to subscription %u, filterMap size is %u",
            filter->type(), subscriptionId_, (unsigned int)filterMap.size());
  }
  TRACE_2("Num discarded: %u", s_info_.d_info.numberDiscarded);
  if (s_info_.d_info.numberDiscarded) {
    for (unsigned int i = 0; i < s_info_.d_info.numberDiscarded; i++) {
      discardedAdd(s_info_.d_info.discardedNotificationIdentifiers[i]);
    }
    s_info_.d_info.numberDiscarded = 0;
    free(s_info_.d_info.discardedNotificationIdentifiers);
    s_info_.d_info.discardedNotificationIdentifiers = NULL;
  }
}

/**
 * This is the destructor.
 *
 * The filter object is deleted.
 */
NtfSubscription::~NtfSubscription() {
  TRACE_ENTER();
  TRACE_2("Subscription %u deleted", subscriptionId_);
  // delete all filters
  FilterMap::iterator posN = filterMap.begin();
  while (posN != filterMap.end()) {
    NtfFilter* filter = posN->second;
    TRACE_2("Delete filter type %#x", (int)filter->type());
    delete filter;
    filterMap.erase(posN++);
  }
  TRACE_LEAVE();
}

/**
 * This method is called to get the id of the subscription.
 *
 * @return Id of the subscription.
 */
SaNtfSubscriptionIdT NtfSubscription::getSubscriptionId() const {
  return (subscriptionId_);
}
/**
 * This method is called to get the subscriptin info struct of
 * the subscription.
 *
 * @return pointer to the subscription info struct.
 */
ntfsv_subscribe_req_t* NtfSubscription::getSubscriptionInfo() {
  return (&s_info_);
}

/**
 * This method is called to check if the subscription matches the received
 * notification.
 *
 * The appropriate filter that matches the type of the received
 * notification is checked.
 *
 * @param notification
 *               Pointer to the received notifiaction object.
 *
 * @return true if the subscription matches the notification
 *         false if the subscription does not match the notification
 */
bool NtfSubscription::checkSubscription(NtfSmartPtr& notification) {
  bool rv = false;

  FilterMap::iterator pos = filterMap.find(notification->getNotificationType());
  if (pos != filterMap.end()) {
    NtfFilter* filter = pos->second;
    osafassert(filter);
    rv = filter->checkFilter(notification);
  }
  return (rv);
}

/**
 * Add a notification Id to the discarded list
 *
 * @param n_id
 *                      Unique notification id.
 */
void NtfSubscription::discardedAdd(SaNtfIdentifierT n_id) {
  discardedNotificationIdList.push_back(n_id);
  TRACE_2("add discarded list size is %u, not_id %llu",
          (unsigned int)discardedNotificationIdList.size(), n_id);
}

/**
 *  Clear the discarded list.
 */
void NtfSubscription::discardedClear() {
  TRACE_ENTER();
  discardedNotificationIdList.clear();
  TRACE_LEAVE();
}

void NtfSubscription::syncRequest(NCS_UBAID* uba) {
  s_info_.d_info.notificationType = SA_NTF_TYPE_ALARM; /* not used */
  s_info_.d_info.numberDiscarded = discardedNotificationIdList.size();
  if (s_info_.d_info.numberDiscarded) {
    s_info_.d_info.discardedNotificationIdentifiers = (SaNtfIdentifierT*)malloc(
        sizeof(SaNtfIdentifierT) * s_info_.d_info.numberDiscarded);
    if (!s_info_.d_info.discardedNotificationIdentifiers) {
      LOG_WA("malloc failed");
      osafassert(0);
    }
    DiscardedNotificationIdList::iterator pos;
    int i = 0;
    pos = discardedNotificationIdList.begin();
    while (pos != discardedNotificationIdList.end()) {
      s_info_.d_info.discardedNotificationIdentifiers[i] = *pos;
      i++;
      pos++;
    }
  }
  if (0 == sendNewSubscription(&s_info_, uba)) {
    LOG_ER("syncRequest send subscription failed");
    osafassert(0);
  }
  free(s_info_.d_info.discardedNotificationIdentifiers);
  s_info_.d_info.discardedNotificationIdentifiers = NULL;
}

/**
 * This method is called when a notification should be sent to the client.
 *
 * If there are no notifications in the discarded notification list, then
 * we try to send it out. If it does not succeed, we put the newly
 * received notification in the list.
 *
 * If there are notifications in the discarded notification list, then
 * we try to send them out first. If it succeeds, we try to send the newly
 * received notifiaction. If it does not succeed, we put the newly
 * received notification to the end of the list.
 *
 * @param notification
 *               Pointer to the notification object.
 */
void NtfSubscription::sendNotification(NtfSmartPtr& notification,
                                       NtfClient* client) {
  TRACE_ENTER();
  // store the matching subscriptionId in the notification
  notification->getNotInfo()->subscriptionId = getSubscriptionId();
#if DISCARDED_TEST
  /* TODO REMOVE TEST */
  disc_test_cntr++;
#endif
  // check if there are discarded notifications
  if (discardedNotificationIdList.empty()) {
    // send notification
    TRACE_3("send_notification_lib called, client %u, notification %llu",
            client->getClientId(), notification->getNotificationId());
    if (send_notification_lib(notification->getNotInfo(), client->getClientId(),
                              client->getMdsDest()) != NCSCC_RC_SUCCESS) {
      // send failed, put notification id in discard list
      discardedAdd(notification->getNotificationId());
    }
  } else {
    // there are already discarded notifications in the queue, send them first
    ntfsv_discarded_info_t d_info;
    d_info.notificationType =
        (SaNtfNotificationTypeT)notification->getNotificationType();
    d_info.numberDiscarded = discardedNotificationIdList.size();
    d_info.discardedNotificationIdentifiers = (SaNtfIdentifierT*)malloc(
        sizeof(SaNtfIdentifierT) * d_info.numberDiscarded);
    if (!d_info.discardedNotificationIdentifiers) {
      LOG_ER("malloc failed");
      discardedAdd(notification->getNotificationId());
      /* The notification can be confirmed since it is put into discarded
       * list.*/
      notificationSentConfirmed(client->getClientId(), getSubscriptionId(),
                                notification->getNotificationId(), 1);
      TRACE_LEAVE();
      return;
    }
    DiscardedNotificationIdList::iterator pos;
    int i = 0;
    pos = discardedNotificationIdList.begin();
    while (pos != discardedNotificationIdList.end()) {
      d_info.discardedNotificationIdentifiers[i] = *pos;
      i++;
      pos++;
    }
    // first try to send discarded notifications
    TRACE_3("send_discard notifications called, [%u]", d_info.numberDiscarded);
    if (send_discard_notification_lib(
            &d_info, client->getClientId(), getSubscriptionId(),
            client->getMdsDest()) == NCSCC_RC_SUCCESS) {
      // sending discarded notifications was successful, empty list
      discardedNotificationIdList.clear();
      TRACE_3("send_discard_notification_lib succeeded, dl size is %u",
              (unsigned int)discardedNotificationIdList.size());

      // try to send the new notification
      if (send_notification_lib(notification->getNotInfo(),
                                client->getClientId(),
                                client->getMdsDest()) != NCSCC_RC_SUCCESS) {
        discardedAdd(notification->getNotificationId());
      }
    } else {
      discardedAdd(notification->getNotificationId());
      /* The notification can be confirmed since it is put into discarded
       * list.*/
      notificationSentConfirmed(client->getClientId(), getSubscriptionId(),
                                notification->getNotificationId(), 1);
      { // Start
        // Generate Discarded Ntf Clean up as
        // we are not able to send second time.
        ntfsv_ntfs_evt_t *evt = NULL;
        if (NULL == (evt = reinterpret_cast<ntfsv_ntfs_evt_t *>
              (calloc(1, sizeof(ntfsv_ntfs_evt_t))))) {
          LOG_WA("mem alloc FAILURE");
          goto done;
        }
        evt->evt_type = NTFSV_NTFS_EVT_NTFA_DOWN;
        /** Initialize the Event Header **/
        // Filling non-zero to determine an internal event
        evt->internal_event = true;
        evt->fr_node_id =
          m_NTFS_GET_NODE_ID_FROM_ADEST(client->getMdsDest());
        evt->fr_dest = client->getMdsDest();

        /** Initialize the MDS portion of the header **/
        evt->info.mds_info.node_id =
          m_NTFS_GET_NODE_ID_FROM_ADEST(client->getMdsDest());
        evt->info.mds_info.mds_dest_id = client->getMdsDest();
        TRACE("Nodeid: %u, MdsDest: %" PRIu64, evt->info.mds_info.node_id,
            evt->info.mds_info.mds_dest_id);
        TRACE("mdsDest: %" PRIu64, evt->info.mds_info.mds_dest_id);

        /* Push the event and we are done */
        if (m_NCS_IPC_SEND(&ntfs_cb->mbx, evt, NCS_IPC_PRIORITY_HIGH) !=
            NCSCC_RC_SUCCESS) {
          LOG_WA("ipc send failed");
          free(evt);
          goto done;
        }
        LOG_ER(
            "Down event missed for app with mdsdest: %" PRIu64 " on node: %u",
            evt->info.mds_info.mds_dest_id, evt->info.mds_info.node_id);
      }  // End
  }
    free(d_info.discardedNotificationIdentifiers);
  }
done:
  TRACE_LEAVE();
}

void NtfSubscription::printInfo() {
  TRACE("Subscription information");
  TRACE("  subscriptionId %u", subscriptionId_);
}
/**
 *  Returns size of discarded list.
 */
unsigned int NtfSubscription::discardedListSize() {
  return discardedNotificationIdList.size();
}
