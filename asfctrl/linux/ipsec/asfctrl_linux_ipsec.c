/* Copyright (C) 2010 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * File:	asfctrl_linux_ipsec.c
 *
 * Added Support for ipsec configuration information offloading
 * from Linux to ASF.
 *
 * Authors:	Hemant Agrawal <hemant@freescale.com>
 *		Sandeep Malik <b02416@freescale.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the  GNU General Public License along
 * with this program; if not, write  to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
/****************************************************************************
Revision History:
*  Version	Date		Author		Change Description
*  0.1	29/07/2010    Hemant Agrawal	Initial Development
*  1.0	29/09/2010    Sandeep Malik		Linux Integration
***************************************************************************/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/if_arp.h>
#include <gianfar.h>
#include <net/ip.h>
#include <net/dst.h>
#include <net/route.h>
#include <net/xfrm.h>

#include "../../../asfipsec/driver/ipsfpapi.h"
#include "../ffp/asfctrl.h"
#include "asfctrl_linux_ipsec_hooks.h"

#define ASFCTRL_LINUX_IPSEC_VERSION	"1.0.0"
#define ASFCTRL_LINUX_IPSEC_DESC 	"ASF Linux-IPsec Integration Driver"

/** \brief	Driver's license
 *  \details	GPL
 *  \ingroup	Linux_module
 */
MODULE_LICENSE("GPL");
/** \brief	Module author
 *  \ingroup	Linux_module
 */
MODULE_AUTHOR("Freescale Semiconductor, Inc");
/** \brief	Module description
 *  \ingroup	Linux_module
 */
MODULE_DESCRIPTION(ASFCTRL_LINUX_IPSEC_DESC);

/* Global Variables */
ASFIPSecCap_t g_ipsec_cap;
uint32_t asfctrl_vsg_ipsec_cont_magic_id;
uint32_t asfctrl_max_sas = SECFP_MAX_SAS;
uint32_t asfctrl_max_policy_cont = ASFCTRL_MAX_SPD_CONTAINERS;

struct asf_ipsec_callbackfn_s asf_sec_fns = {
		asfctrl_xfrm_enc_hook,
		asfctrl_xfrm_dec_hook,
		NULL,
		asfctrl_xfrm_encrypt_n_send
};
/* function_prototypes */


ASF_void_t asfctrl_ipsec_fn_NoInSA(ASF_uint32_t ulVsgId,
				ASFBuffer_t Buffer,
				genericFreeFn_f pFreeFn,
				ASF_void_t *freeArg,
				ASF_uint32_t ulCommonInterfaceId)
{
	struct sk_buff  *skb;
	int bVal = in_softirq();
	ASFCTRL_FUNC_TRACE;
	if (!bVal)
		local_bh_disable();

	skb = AsfBuf2Skb(Buffer);

	ASFCTRL_INFO("Sending packet UP ");
	/* Send it to for normal path handling */
	netif_receive_skb(skb);

	if (!bVal)
		local_bh_enable();
}

ASF_void_t asfctrl_ipsec_fn_NoOutSA(ASF_uint32_t  ulVsgId,
				ASFFFPFlowTuple_t *tuple,
				ASFBuffer_t Buffer,
				genericFreeFn_f pFreeFn,
				ASF_void_t   *freeArg,
				ASF_uchar8_t bSPDContainerPresent,
				ASF_uchar8_t bRevalidate)
{
	struct sk_buff  *skb;
	int bVal = in_softirq();
	ASFCTRL_FUNC_TRACE;
	if (!bVal)
		local_bh_disable();

	skb = AsfBuf2Skb(Buffer);

	/* Send the pacekt up for normal path IPsec processing. (after the NAT)
		has to be special function */
#if 0
	if (NET_RX_DROP == ip_forward_asf_packet(skb))
			pFreeFn(Buffer.nativeBuffer);
#endif
	ASFCTRL_ERR("\n NO OUT SA Found Drop packet\n");
	pFreeFn(Buffer.nativeBuffer);

	/*
	if (bRevalidate) Revalidate the SPD OUT */

	if (!bVal)
		local_bh_enable();

	return;
}

ASF_void_t asfctrl_ipsec_fn_VerifySPD(ASF_uint32_t ulVSGId,
					ASF_uint32_t ulInSPDContainerIndex,
					ASF_uint32_t ulMagicNumber,
					ASF_uint32_t ulSPI,
					ASF_uint8_t usProtocol,
					ASF_IPAddr_t DestAddr,
					ASFBuffer_t Buffer,
					genericFreeFn_f pFreeFn,
					ASF_void_t    *freeArg,
					ASF_uchar8_t bRevalidate,
					ASF_uint32_t ulCommonInterfaceId)
{
	struct sk_buff  *skb;
	struct xfrm_state *x;
	struct net *net;
	xfrm_address_t daddr;
	unsigned short family;
	int bVal = in_softirq();
	if (!bVal)
		local_bh_disable();

	skb = AsfBuf2Skb(Buffer);
	ASFCTRL_DBG("\n DestAddr %x protocol %x SPI %x",
			DestAddr.ipv4addr, usProtocol, ulSPI);

	/*1.  find the SA (xfrm pointer) on the basis of SPI,
	 * protcol, dest Addr */
	net = dev_net(skb->dev);
	daddr.a4 = (DestAddr.ipv4addr);
	family = AF_INET;
	x = xfrm_state_lookup(net, &daddr, ulSPI, usProtocol, family);
	if (x == NULL) {
		ASFCTRL_WARN("Unable to get the match SPD"\
			"for the decrypted packet");
		pFreeFn(Buffer.nativeBuffer);
		goto fnexit;
	}
	/*2. Set the sec_path security context into the skb */
	/* Allocate new secpath or COW existing one. */
	if (!skb->sp || atomic_read(&skb->sp->refcnt) != 1) {
		struct sec_path *sp;

		sp = secpath_dup(skb->sp);
		if (!sp) {
			/* Drop the packet */
			pFreeFn(Buffer.nativeBuffer);
			goto fnexit;
		}
		if (skb->sp)
			secpath_put(skb->sp);
		skb->sp = sp;
	}

	/*fill the details of secpath */
	skb->sp->xvec[skb->sp->len++] = x;

	/*3. send the packet to slow path */
	netif_receive_skb(skb);

	/*
	4.if (bRevalidate) Revalidate the SPD IN */

fnexit:
	if (!bVal)
		local_bh_enable();
	return;
}

ASF_void_t asfctrl_ipsec_fn_SeqNoOverFlow(ASF_uint32_t ulVSGId,
					ASF_uint32_t ulTunnelId,
					ASF_uint32_t ulSPI,
					ASF_uint16_t usProtocol,
					ASF_IPAddr_t  DestAddr)
{
	ASFCTRL_FUNC_TRACE;
	return;
}

ASF_void_t asfctrl_ipsec_fn_PeerGatewayChange(ASF_uint32_t ulVSGId,
					ASF_uint32_t ulInSPDContainerIndex,
					ASF_uint32_t ulSPI,
					ASF_uint8_t  usProtocol,
					ASF_IPAddr_t OldDstAddr,
					ASF_IPAddr_t NewDstAddr,
					ASF_uint16_t usOldPort,
					ASF_uint16_t usNewPort)
{
	ASFCTRL_FUNC_TRACE;
	return;
}

ASF_void_t asfctrl_ipsec_fn_audit_log(ASFLogInfo_t *pIPSecV4Info)
{
	int bVal = in_softirq();
	if (!bVal)
		local_bh_disable();

	/* Filling the Loging message fields, IPSec module specific fileds  */
	ASFCTRL_FUNC_TRACE;

	ASFCTRL_TRACE("%s-SA, SPI=%d, Proto=%d, "\
			"Dst IPAddr= %d,  Src IPAddr= %d PathMTU=%d,",
		XFRM_DIR(pIPSecV4Info->u.IPSecInfo.ucDirection),
		pIPSecV4Info->u.IPSecInfo.ulSPI,
		pIPSecV4Info->u.IPSecInfo.ucProtocol,
		pIPSecV4Info->u.IPSecInfo.Address.dstIP.ipv4addr,
		pIPSecV4Info->u.IPSecInfo.Address.srcIP.ipv4addr,
		pIPSecV4Info->u.IPSecInfo.ulPathMTU);

	ASFCTRL_TRACE("Msg (%d)= %s", pIPSecV4Info->ulMsgId,
		pIPSecV4Info->aMsg ? pIPSecV4Info->aMsg : "null");

	/*pIPSecV4Info->u.IPSecInfo.ulSeqNumber*/
	ASFCTRL_TRACE("Num of Pkts = %d\nNumof Bytes = %d",
		pIPSecV4Info->u.IPSecInfo.ulNumOfPktsProcessed,
		pIPSecV4Info->u.IPSecInfo.ulNumOfBytesProcessed);

	if (!bVal)
		local_bh_enable();
	return;
}

/*If the policy offload fails, need to reset the cookie in the
 * linux do we need it for the sync-SMP mode of ASF-Linux */
ASF_void_t asfctrl_ipsec_fn_Config(ASF_uint32_t ulVSGId,
				ASF_uint32_t Cmd,
				ASF_uint32_t Response,
				ASF_void_t  *pRequestIdentifier,
				ASF_uint32_t ulRequestIdentifierLen,
				ASF_uint32_t ulResult)
{
	int bVal = in_softirq();
	struct xfrm_policy *xp = (struct xfrm_policy *)pRequestIdentifier;

	ASFCTRL_FUNC_TRACE;
	if (!bVal)
		local_bh_disable();

	if (Response != T_SUCCESS) {
		if (Cmd == ASF_IPSEC_CONFIG_ADD_OUTSPDCONTAINER) {
			free_container_index(xp->asf_cookie,
				ASF_OUT_CONTANER_ID);
			xp->asf_cookie = 0;
		} else if (Cmd == ASF_IPSEC_CONFIG_ADD_INSPDCONTAINER) {
			free_container_index(xp->asf_cookie,
				ASF_IN_CONTANER_ID);
			xp->asf_cookie = 0;
		};
	}
	if (!bVal)
		local_bh_enable();
	return;
}

ASF_void_t asfctrl_ipsec_fn_RefreshL2Blob(ASF_uint32_t ulVSGId,
				ASF_uint32_t ultunnelId,
				ASF_uint32_t ulOutSPDContainerIndex,
				ASF_uint32_t ulOutSPDmagicNumber,
				ASF_IPSecTunEndAddr_t *address,
				ASF_uint32_t ulSPI,
				ASF_uint8_t  ucProtocol)
{
	struct sk_buff *skb;
	int bVal = in_softirq();
	ASFCTRL_FUNC_TRACE;
	if (!bVal)
		local_bh_disable();
	/* Generate Dummy packet */
	skb = alloc_skb((4*1024), GFP_ATOMIC);
	if (skb) {
		struct iphdr *iph;
		ASF_uint32_t *pData;
		ASFIPSecRuntimeModOutSAArgs_t *pSAData;

		struct rtable *rt;
		struct flowi fl = {
				.nl_u = {
				.ip4_u = {
					.daddr = address->dstIP.ipv4addr,
					.saddr = address->srcIP.ipv4addr,
						},
						},
				.proto = IPPROTO_ICMP };

		if (ip_route_output_key(&init_net, &rt, &fl)) {
			ASFCTRL_DBG("\n Route not found for dst %x"\
			"local host: %d", address->dstIP.ipv4addr,
			(skb_rtable(skb)->rt_flags & RTCF_LOCAL) ? 1 : 0);
			kfree_skb(skb);
			return ;
		}

		skb_dst_set(skb, dst_clone(&rt->u.dst));

		ASFCTRL_DBG("\n Route found for dst %x ",
					address->dstIP.ipv4addr);
		skb->dev = skb_dst(skb)->dev;
		ASFCTRL_DBG("\ndevname is skb->devname: %s", skb->dev->name);
		skb_reserve(skb, LL_RESERVED_SPACE(skb->dev));
		skb_reset_network_header(skb);
		skb_put(skb, sizeof(struct iphdr));
		iph = ip_hdr(skb);
		iph->version = 5;
		iph->ihl = 5;
		iph->ttl = 1;
		iph->saddr = (address->srcIP.ipv4addr);
		iph->daddr = (address->dstIP.ipv4addr);
		iph->protocol = ASFCTRL_IPPROTO_DUMMY_IPSEC_L2BLOB;

		pData = (ASF_uint32_t *)skb_put(skb,
				sizeof(ASFIPSecRuntimeModOutSAArgs_t));
		*pData = ulVSGId;
		pSAData = (ASFIPSecRuntimeModOutSAArgs_t *)(pData + 1);
		pSAData->ulTunnelId = ultunnelId;
		pSAData->DestAddr = address->dstIP;
		pSAData->ulSPDContainerIndex =  ulOutSPDContainerIndex;
		pSAData->ulSPDContainerMagicNumber = ulOutSPDmagicNumber;
		pSAData->ucProtocol = ucProtocol;
		pSAData->ulSPI = ulSPI;
		pSAData->ucChangeType = 2;
		pSAData->u.ulMtu  = skb->dev->mtu;
		skb->protocol = htons(ETH_P_IP);
		asfctrl_skb_mark_dummy(skb);
		asf_ip_send(skb);
	}
	if (!bVal)
		local_bh_enable();
	return;
}

ASF_void_t asfctrl_ipsec_fn_DPDAlive(ASF_uint32_t ulVSGId,
				ASF_uint32_t ulTunnelId,
				ASF_uint32_t ulSPI,
				ASF_uint16_t usProtocol,
				ASF_IPAddr_t DestAddr,
				ASF_uint32_t ulSPDContainerIndex)
{
	ASFCTRL_FUNC_TRACE;
	return;
}


ASF_void_t asfctrl_ipsec_fn_NoOutFlowFound(ASF_uint32_t ulVSGId,
					ASF_IPAddr_t srcAddr,
					ASF_IPAddr_t destAddr,
					ASF_uint8_t  ucProtocol,
					ASF_uint16_t srcPort,
					ASF_uint16_t destPort,
					ASFBuffer_t Buffer,
					genericFreeFn_f pFreeFn,
					ASF_void_t *freeArg)
{
	ASFCTRL_FUNC_TRACE;
	return;
}

ASF_void_t asfctrl_ipsec_fn_VSGMappingNotFound(
				ASF_uint32_t ulCommonInterfaceid,
				ASFFFPFlowTuple_t tuple,
				ASFBuffer_t Buffer,
				genericFreeFn_f pFreeFn,
				ASF_void_t *freeArg)
{
	ASFCTRL_FUNC_TRACE;
	return;
}

ASF_void_t  asfctrl_ipsec_fn_InterfaceInfoNotFound(ASFFFPFlowTuple_t tuple,
						ASFBuffer_t Buffer,
						genericFreeFn_f pFreeFn,
						ASF_void_t *freeArg)
{
	ASFCTRL_FUNC_TRACE;
	return;
}


ASF_void_t asfctrl_ipsec_fn_Runtime(ASF_uint32_t ulVSGId,
				ASF_uint32_t Cmd,
				ASF_void_t  *pRequestIdentifier,
				ASF_uint32_t ulRequestIdentifierLen,
				ASF_void_t  *pResult,
				ASF_uint32_t ulResultLen)
{
	ASFCTRL_FUNC_TRACE;
	return;
}

ASF_void_t asfctrl_ipsec_l2blob_update_fn(struct sk_buff *skb,
					ASF_uint32_t hh_len,
					T_UINT16 ulDeviceID)
{
	ASFIPSecRuntimeModOutSAArgs_t *pSAData;
	ASF_uint32_t ulVSGId;
	ASF_void_t *pData;
	struct iphdr *iph;

	ASFCTRL_FUNC_TRACE;

	iph = (struct iphdr *)(skb->data + hh_len);

	pData = skb->data + hh_len + (iph->ihl * 4);

	ulVSGId = *(ASF_uint32_t *)pData;

	pSAData = (ASFIPSecRuntimeModOutSAArgs_t *)((T_UCHAR8 *)pData + 4);

	if (pSAData->ucChangeType == 2) {
		ASFIPSecRuntime(ulVSGId, ASF_IPSEC_RUNTIME_MOD_OUTSA, pSAData,
			sizeof(ASFIPSecRuntimeModOutSAArgs_t), NULL, 0);
	}

	pSAData->ucChangeType = 3;
	if (hh_len > ASF_MAX_L2BLOB_LEN)
		return;
	pSAData->u.l2blob.ulDeviceID = ulDeviceID;
	pSAData->u.l2blob.ulL2BlobLen =  hh_len;
	memcpy(&pSAData->u.l2blob.l2blob, skb->data,
			pSAData->u.l2blob.ulL2BlobLen);
	ASFIPSecRuntime(ulVSGId, ASF_IPSEC_RUNTIME_MOD_OUTSA, pSAData,
			sizeof(ASFIPSecRuntimeModOutSAArgs_t), NULL, 0);
	return;
}


void asfctrl_ipsec_update_vsg_magic_number(void)
{
	ASFIPSecUpdateVSGMagicNumber_t VSGMagicInfo;
	VSGMagicInfo.ulVSGId = ASF_DEF_VSG;
	VSGMagicInfo.ulVSGMagicNumber = asfctrl_vsg_config_id;
	ASFIPSecUpdateVSGMagicNumber(&VSGMagicInfo);
	return ;
}

int asfctrl_ipsec_get_flow_info_fn(bool *ipsec_in, bool *ipsec_out,
				ASFFFPIpsecInfo_t *ipsecInInfo,
				struct net *net,
				struct flowi fl)
{
	struct xfrm_policy *pol_out = 0, *pol_in = 0;
	ASFFFPIpsecContainerInfo_t *outInfo;
	ASFFFPIpsecContainerInfo_t *inInfo;

	*ipsec_in = ASF_FALSE;
	*ipsec_out = ASF_FALSE;

	pol_out = xfrm_policy_check_flow(net, &fl, AF_INET, FLOW_DIR_OUT);
	pol_in = xfrm_policy_check_flow(net, &fl, AF_INET, FLOW_DIR_IN);

	if (pol_out) {
		outInfo = &(ipsecInInfo->outContainerInfo);
		*ipsec_in = ASF_FALSE;
		*ipsec_out = ASF_TRUE;
		outInfo->ulTunnelId = ASF_DEF_IPSEC_TUNNEL_ID;
		outInfo->ulSPDMagicNumber = asfctrl_vsg_ipsec_cont_magic_id;
		outInfo->ulSPDContainerId = pol_out->asf_cookie;
		outInfo->configIdentity.ulVSGConfigMagicNumber =
					asfctrl_vsg_config_id;
		outInfo->configIdentity.ulTunnelConfigMagicNumber =
					ASF_DEF_IPSEC_TUNNEL_MAGIC_NUM;
		ASFCTRL_INFO("\n magicnum %x contId %d",
				outInfo->ulSPDMagicNumber,
				outInfo->ulSPDContainerId);
	}
	if (pol_in) {
		inInfo = &(ipsecInInfo->inContainerInfo);
		*ipsec_out = ASF_FALSE;
		*ipsec_in = ASF_TRUE;
		inInfo->ulTunnelId = ASF_DEF_IPSEC_TUNNEL_ID;
		inInfo->ulSPDMagicNumber = asfctrl_vsg_ipsec_cont_magic_id;
		inInfo->ulSPDContainerId = pol_in->asf_cookie;
		inInfo->configIdentity.ulVSGConfigMagicNumber =
					asfctrl_vsg_config_id;
		inInfo->configIdentity.ulTunnelConfigMagicNumber =
					ASF_DEF_IPSEC_TUNNEL_MAGIC_NUM;
		ASFCTRL_INFO("\n magicnum %x contId %d",
				inInfo->ulSPDMagicNumber,
				inInfo->ulSPDContainerId);

	}
	return 1;
}



static int __init asfctrl_linux_ipsec_init(void)
{
	ASFIPSecCbFn_t Fnptr;
	ASFCap_t  asf_cap;
	ASFIPSecInitConfigIdentity_t  confId;
	unsigned int *ulVSGMagicNumber;
	unsigned int **ulTunnelMagicNumber;
	int i, j;

	ASFGetCapabilities(&asf_cap);
	if (!asf_cap.func.bIPsec) {
		ASFCTRL_ERR("IPSEC Not supported in ASF");
		return -EPERM;
	}

	ASFIPSecSetNotifyPreference(ASF_ASYNC_RESPONSE);

	ASFIPSecGetCapabilities(&g_ipsec_cap);

	if (!g_ipsec_cap.bBufferHomogenous) {
		/* Hetrogenous */
		ASFCTRL_ERR("Hetrogeneous buffers not supported\r\n");
		return -EINVAL;
	}
	ulVSGMagicNumber = kzalloc(sizeof(unsigned int *) * ASF_MAX_NUM_VSG,
				GFP_KERNEL);
	ulTunnelMagicNumber = kzalloc(sizeof(unsigned int *) * ASF_MAX_NUM_VSG,
				GFP_KERNEL);
	for (i = 0; i < ASF_MAX_NUM_VSG; i++)
		ulTunnelMagicNumber[i] = kzalloc(sizeof(unsigned int) *
			ASF_MAX_TUNNEL, GFP_KERNEL);
	/* If ASF supports less than what our arrays are designed for */
	if (g_ipsec_cap.ulMaxSupportedIPSecSAs < SECFP_MAX_SAS)
		asfctrl_max_sas = g_ipsec_cap.ulMaxSupportedIPSecSAs;

	if (g_ipsec_cap.ulMaxSPDContainers < ASFCTRL_MAX_SPD_CONTAINERS)
		asfctrl_max_policy_cont = g_ipsec_cap.ulMaxSupportedIPSecSAs;

	asfctrl_vsg_ipsec_cont_magic_id = jiffies;
	/* Updating the existing Config ID in ASF IPSEC */
	confId.ulMaxVSGs = ASF_MAX_NUM_VSG;
	confId.ulMaxTunnels = ASF_MAX_TUNNEL;
	for (i = 0; i < ASF_MAX_NUM_VSG; i++) {
		ulVSGMagicNumber[i] = asfctrl_vsg_config_id;
		for (j = 0; j < ASF_MAX_TUNNEL; j++)
			ulTunnelMagicNumber[i][j] =
				ASF_DEF_IPSEC_TUNNEL_MAGIC_NUM;
	}
	confId.pulVSGMagicNumber = ulVSGMagicNumber;
	confId.pulTunnelMagicNumber = ulTunnelMagicNumber;

	ASFIPSecInitConfigIdentity(&confId);

	kfree(ulVSGMagicNumber);
	for (i = 0; i < ASF_MAX_NUM_VSG; i++)
		kfree(ulTunnelMagicNumber[i]);
	kfree(ulTunnelMagicNumber);

	register_ipsec_offload_hook(&asf_sec_fns);
	asfctrl_ipsec_km_register();

	Fnptr.pFnNoInSA  = asfctrl_ipsec_fn_NoInSA;
	Fnptr.pFnNoOutSA = asfctrl_ipsec_fn_NoOutSA;
	Fnptr.pFnVerifySPD = asfctrl_ipsec_fn_VerifySPD;
	Fnptr.pFnRefreshL2Blob = asfctrl_ipsec_fn_RefreshL2Blob;
	Fnptr.pFnDPDAlive = asfctrl_ipsec_fn_DPDAlive;
	Fnptr.pFnSeqNoOverFlow = asfctrl_ipsec_fn_SeqNoOverFlow;
	Fnptr.pFnPeerChange = asfctrl_ipsec_fn_PeerGatewayChange;
	Fnptr.pFnAuditLog = asfctrl_ipsec_fn_audit_log;
	Fnptr.pFnNoOutFlow = asfctrl_ipsec_fn_NoOutFlowFound;
	Fnptr.pFnConfig = asfctrl_ipsec_fn_Config;
	Fnptr.pFnRuntime = asfctrl_ipsec_fn_Runtime;
	Fnptr.pFnVSGMap = asfctrl_ipsec_fn_VSGMappingNotFound;
	Fnptr.pFnIfaceNotFound = asfctrl_ipsec_fn_InterfaceInfoNotFound;
	ASFIPSecRegisterCallbacks(&Fnptr);

	asfctrl_register_ipsec_func(asfctrl_ipsec_get_flow_info_fn,
				asfctrl_ipsec_l2blob_update_fn,
				asfctrl_ipsec_update_vsg_magic_number);
	init_container_indexes();
	init_sa_indexes();
	ASFCTRL_DBG("ASF Control Module - IPsec Loaded\n");
	return 0;
}


static void __exit asfctrl_linux_ipsec__exit(void)
{
	ASFIPSecCbFn_t Fnptr;
	memset(&Fnptr, 0, sizeof(ASFIPSecCbFn_t));
	ASFIPSecRegisterCallbacks(&Fnptr);
	asfctrl_register_ipsec_func(NULL, NULL, NULL);
	asfctrl_ipsec_km_unregister();
	unregister_ipsec_offload_hook();
	ASFCTRL_DBG("ASF Control Module - IPsec Unloaded \n");
}

module_init(asfctrl_linux_ipsec_init);
module_exit(asfctrl_linux_ipsec__exit);
