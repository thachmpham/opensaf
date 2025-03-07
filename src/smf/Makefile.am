#      -*- OpenSAF  -*-
#
# (C) Copyright 2016 The OpenSAF Foundation
# Copyright Ericsson AB 2017 - All Rights Reserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. This file and program are licensed
# under the GNU Lesser General Public License Version 2.1, February 1999.
# The complete license can be accessed from the following location:
# http://opensource.org/licenses/lgpl-license.php
# See the Copying file included with the OpenSAF distribution for full
# licensing terms.
#
# Author(s): Ericsson AB
#

EXTRA_DIST += \
	src/smf/README \
	src/smf/README.doxygen \
	src/smf/README.schema

if ENABLE_AIS_SMF

osaf_lib_LTLIBRARIES += lib/libsmfsv_common.la

lib_libsmfsv_common_la_CFLAGS = $(OSAF_LIB_FLAGS) $(AM_CFLAGS)

lib_libsmfsv_common_la_CXXFLAGS = $(OSAF_LIB_FLAGS) $(AM_CXXFLAGS)

lib_libsmfsv_common_la_CPPFLAGS = \
	-DSA_EXTENDED_NAME_SOURCE \
	$(AM_CPPFLAGS)

lib_libsmfsv_common_la_LDFLAGS = \
	$(AM_LDFLAGS)

lib_libsmfsv_common_la_SOURCES = \
	src/smf/common/smfsv_evt.c

nodist_EXTRA_lib_libsmfsv_common_la_SOURCES = dummy.cc

lib_LTLIBRARIES += lib/libSaSmf.la

lib_libSaSmf_la_SOURCES = \
	src/smf/agent/smfa_api.c \
	src/smf/agent/smfa_init.c \
	src/smf/agent/smfa_mds.c \
	src/smf/agent/smfa_utils.c

nodist_EXTRA_lib_libSaSmf_la_SOURCES = dummy.cc

lib_libSaSmf_la_CFLAGS = $(OSAF_LIB_FLAGS) $(AM_CFLAGS)

lib_libSaSmf_la_CXXFLAGS = $(OSAF_LIB_FLAGS) $(AM_CXXFLAGS)

lib_libSaSmf_la_CPPFLAGS = \
	-DSA_EXTENDED_NAME_SOURCE \
	$(AM_CPPFLAGS)

lib_libSaSmf_la_LDFLAGS = \
	$(AM_LDFLAGS)

if HAVE_LD_VERSION_SCRIPT
lib_libSaSmf_la_LDFLAGS += \
	-Wl,-version-script=$(top_srcdir)/src/ais/lib/libSaSmf.map
endif

lib_libSaSmf_la_LDFLAGS += \
	-version-number 0:0:0

lib_libSaSmf_la_LIBADD = \
	lib/libsmfsv_common.la \
	lib/libais.la \
	lib/libopensaf_core.la

noinst_HEADERS += \
	src/smf/agent/smfa.h \
	src/smf/common/smfsv_defs.h \
	src/smf/common/smfsv_evt.h \
	src/smf/smfd/SmfAdminState.h \
	src/smf/smfd/SmfCallback.h \
	src/smf/smfd/SmfCampState.h \
	src/smf/smfd/SmfCampaign.h \
	src/smf/smfd/SmfCampaignInit.h \
	src/smf/smfd/SmfCampaignThread.h \
	src/smf/smfd/SmfCampaignWrapup.h \
	src/smf/smfd/SmfCampaignXmlParser.h \
	src/smf/smfd/SmfCbkUtil.h \
	src/smf/smfd/SmfExecControl.h \
	src/smf/smfd/SmfExecControlHdl.h \
	src/smf/smfd/SmfImmApplierHdl.h \
	src/smf/smfd/SmfImmOperation.h \
	src/smf/smfd/SmfLongDnApplier.h \
	src/smf/smfd/SmfProcState.h \
	src/smf/smfd/SmfProcedureThread.h \
	src/smf/smfd/SmfRollback.h \
	src/smf/smfd/SmfStepState.h \
	src/smf/smfd/SmfStepTypes.h \
	src/smf/smfd/SmfTargetTemplate.h \
	src/smf/smfd/SmfUpgradeAction.h \
	src/smf/smfd/SmfUpgradeCampaign.h \
	src/smf/smfd/SmfUpgradeMethod.h \
	src/smf/smfd/SmfUpgradeProcedure.h \
	src/smf/smfd/SmfUpgradeStep.h \
	src/smf/smfd/SmfUtils.h \
	src/smf/smfd/SmfUtils_ObjExist.h \
	src/smf/smfd/smfd.h \
	src/smf/smfd/smfd_cb.h \
	src/smf/smfd/smfd_evt.h \
	src/smf/smfd/smfd_long_dn.h \
	src/smf/smfd/smfd_smfnd.h \
	src/smf/smfnd/smfnd.h \
	src/smf/smfnd/smfnd_evt.h

osaf_execbin_PROGRAMS += bin/osafsmfd bin/osafsmfnd

nodist_pkgclccli_SCRIPTS += \
	src/smf/smfnd/osaf-smfnd \
	src/smf/smfd/osaf-smfd

dist_osaf_execbin_SCRIPTS += \
	src/smf/scripts/smf-backup-create \
	src/smf/scripts/smf-bundle-check \
	src/smf/scripts/smf-cluster-reboot \
	src/smf/scripts/smf-node-check \
	src/smf/scripts/smf-repository-check

dist_pkgsysconf_DATA += \
	src/smf/smfnd/smfnd.conf \
	src/smf/smfd/smfd.conf

if ENABLE_IMMXML

dist_pkgimmxml_svc_DATA += \
	src/smf/config/smfsv_classes.xml \
	src/smf/config/smfsv_objects.xml \
	src/smf/config/smfsv_pl_template.xml \
	src/smf/config/smfsv_sc_template.xml

endif

dist_bin_SCRIPTS += \
	src/smf/tools/smf-adm \
	src/smf/tools/smf-find \
	src/smf/tools/smf-state

bin_osafsmfd_CXXFLAGS = $(AM_CXXFLAGS) @XML2_CFLAGS@

bin_osafsmfd_CPPFLAGS = \
	-DSA_CLM_B01=1 -DSA_EXTENDED_NAME_SOURCE \
	$(AM_CPPFLAGS)

bin_osafsmfd_SOURCES = \
	src/smf/smfd/SmfAdminState.cc \
	src/smf/smfd/SmfCallback.cc \
	src/smf/smfd/SmfCampState.cc \
	src/smf/smfd/SmfCampaign.cc \
	src/smf/smfd/SmfCampaignInit.cc \
	src/smf/smfd/SmfCampaignThread.cc \
	src/smf/smfd/SmfCampaignWrapup.cc \
	src/smf/smfd/SmfCampaignXmlParser.cc \
	src/smf/smfd/SmfCbkUtil.cc \
	src/smf/smfd/SmfExecControl.cc \
	src/smf/smfd/SmfExecControlHdl.cc \
	src/smf/smfd/SmfImmApplierHdl.cc \
	src/smf/smfd/SmfImmOperation.cc \
	src/smf/smfd/SmfLongDnApplier.cc \
	src/smf/smfd/SmfProcState.cc \
	src/smf/smfd/SmfProcedureThread.cc \
	src/smf/smfd/SmfRollback.cc \
	src/smf/smfd/SmfStepState.cc \
	src/smf/smfd/SmfStepTypes.cc \
	src/smf/smfd/SmfTargetTemplate.cc \
	src/smf/smfd/SmfUpgradeAction.cc \
	src/smf/smfd/SmfUpgradeCampaign.cc \
	src/smf/smfd/SmfUpgradeMethod.cc \
	src/smf/smfd/SmfUpgradeProcedure.cc \
	src/smf/smfd/SmfUpgradeStep.cc \
	src/smf/smfd/SmfUtils.cc \
	src/smf/smfd/SmfUtils_ObjExist.cc \
	src/smf/smfd/smfd_amf.cc \
	src/smf/smfd/smfd_campaign_oi.cc \
	src/smf/smfd/smfd_evt.c \
	src/smf/smfd/smfd_main.cc \
	src/smf/smfd/smfd_mds.c \
	src/smf/smfd/smfd_smfnd.c

bin_osafsmfd_LDFLAGS = \
	$(AM_LDFLAGS) \
	@XML2_LIBS@

bin_osafsmfd_LDADD = \
	lib/libsmfsv_common.la \
	lib/libosaf_common.la \
	lib/libosaf_common.la \
	lib/libSaSmf.la \
	lib/libSaAmf.la \
	lib/libSaClm.la \
	lib/libSaImmOi.la \
	lib/libSaImmOm.la \
	lib/libSaLog.la \
	lib/libSaNtf.la \
	lib/libopensaf_core.la

bin_osafsmfnd_CPPFLAGS = \
	-DSA_CLM_B01=1 -DSA_EXTENDED_NAME_SOURCE \
	$(AM_CPPFLAGS)

bin_osafsmfnd_SOURCES = \
	src/smf/smfnd/smfnd_amf.c \
	src/smf/smfnd/smfnd_evt.c \
	src/smf/smfnd/smfnd_main.c \
	src/smf/smfnd/smfnd_mds.c

bin_osafsmfnd_LDADD = \
	lib/libsmfsv_common.la \
	lib/libSaAmf.la \
	lib/libopensaf_core.la

endif
