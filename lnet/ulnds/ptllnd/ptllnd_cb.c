/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2005 Cluster File Systems, Inc. All rights reserved.
 *   Author: Eric Barton <eeb@bartonsoftware.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   This file is confidential source code owned by Cluster File Systems.
 *   No viewing, modification, compilation, redistribution, or any other
 *   form of use is permitted except through a signed license agreement.
 *
 *   If you have not signed such an agreement, then you have no rights to
 *   this file.  Please destroy it immediately and contact CFS.
 *
 */

#include "ptllnd.h"

char *
ptllnd_ptlid2str(ptl_process_id_t id)
{
        static char strs[32][16];
        static int  idx = 0;

        snprintf(strs[idx], sizeof(strs[0]),
                 "%d-"LPD64, id.pid, (__u64)id.nid);

        return strs[idx++];
}

void
ptllnd_destroy_peer(ptllnd_peer_t *peer)
{
        lnet_ni_t         *ni = peer->plp_ni;
        ptllnd_ni_t       *plni = ni->ni_data;

        LASSERT (peer->plp_closing);
        LASSERT (plni->plni_npeers > 0);
        LASSERT (list_empty(&peer->plp_txq));
        plni->plni_npeers--;
        LIBCFS_FREE(peer, sizeof(*peer));
}

void
ptllnd_close_peer(ptllnd_peer_t *peer)
{
        lnet_ni_t   *ni = peer->plp_ni;
        ptllnd_ni_t *plni = ni->ni_data;

        if (peer->plp_closing)
                return;

        peer->plp_closing = 1;

        while (!list_empty(&peer->plp_txq)) {
                ptllnd_tx_t *tx = list_entry(peer->plp_txq.next,
                                             ptllnd_tx_t, tx_list);
                tx->tx_status = -ESHUTDOWN;
                list_del(&tx->tx_list);
                list_add_tail(&tx->tx_list, &plni->plni_zombie_txs);
        }

        list_del(&peer->plp_list);
        ptllnd_peer_decref(peer);
}

ptllnd_peer_t *
ptllnd_find_peer(lnet_ni_t *ni, lnet_nid_t nid, int create)
{
        ptllnd_ni_t       *plni = ni->ni_data;
        unsigned int       hash = LNET_NIDADDR(nid) % plni->plni_peer_hash_size;
        struct list_head  *tmp;
        ptllnd_peer_t     *plp;
        ptllnd_tx_t       *tx;
        int                rc;

        PJK_UT_MSG(">>> nid=" LPX64 "\n",nid);

        LASSERT (LNET_NIDNET(nid) == LNET_NIDNET(ni->ni_nid));

        list_for_each(tmp, &plni->plni_peer_hash[hash]) {
                plp = list_entry(tmp, ptllnd_peer_t, plp_list);

                if (plp->plp_nid == nid) {
                        ptllnd_peer_addref(plp);
                        PJK_UT_MSG("<<< peer=%p FOUND\n",plp);
                        return plp;
                }
        }

        if (!create)
                return NULL;

        /* New peer: check first for enough posted buffers */
        plni->plni_npeers++;
        rc = ptllnd_grow_buffers(ni);
        if (rc != 0) {
                plni->plni_npeers--;
                return NULL;
        }

        LIBCFS_ALLOC(plp, sizeof(*plp));
        if (plp == NULL) {
                CERROR("Can't allocate new peer %s\n",
                       libcfs_nid2str(nid));
                plni->plni_npeers--;
                return NULL;
        }

        PJK_UT_MSG("new peer=%p\n",plp);

        plp->plp_ni = ni;
        plp->plp_nid = nid;
        plp->plp_ptlid.nid = LNET_NIDADDR(nid);
        plp->plp_ptlid.pid = plni->plni_pid;
        plp->plp_max_credits =
        plp->plp_credits = 1; /* add more later when she gives me credits */
        plp->plp_max_msg_size = plni->plni_max_msg_size; /* until I hear from her */
        plp->plp_outstanding_credits = plni->plni_peer_credits - 1;
        plp->plp_match = 0;
        plp->plp_recvd_hello = 0;
        plp->plp_closing = 0;
        plp->plp_refcount = 1;
        plp->plp_seq = 0;
        CFS_INIT_LIST_HEAD(&plp->plp_list);
        CFS_INIT_LIST_HEAD(&plp->plp_txq);

        ptllnd_peer_addref(plp);
        list_add_tail(&plp->plp_list, &plni->plni_peer_hash[hash]);

        tx = ptllnd_new_tx(plp, PTLLND_MSG_TYPE_HELLO, 0);
        if (tx == NULL) {
                CERROR("Can't send HELLO to %s\n", libcfs_nid2str(nid));
                ptllnd_close_peer(plp);
                ptllnd_peer_decref(plp);
                return NULL;
        }

        tx->tx_msg.ptlm_u.hello.kptlhm_matchbits = PTL_RESERVED_MATCHBITS;
        tx->tx_msg.ptlm_u.hello.kptlhm_max_msg_size = plni->plni_max_msg_size;

        ptllnd_post_tx(tx);

        PJK_UT_MSG("<<< peer=%p NEW\n",plp);
        return plp;
}

ptllnd_tx_t *
ptllnd_new_tx(ptllnd_peer_t *peer, int type, int payload_nob)
{
        lnet_ni_t   *ni = peer->plp_ni;
        ptllnd_ni_t *plni = ni->ni_data;
        ptllnd_tx_t *tx;
        int          msgsize;

        PJK_UT_MSG("peer=%p type=%d payload=%d\n",peer,type,payload_nob);

        switch (type) {
        default:
                LBUG();

        case PTLLND_RDMA_WRITE:
        case PTLLND_RDMA_READ:
                LASSERT (payload_nob == 0);
                msgsize = 0;
                break;

        case PTLLND_MSG_TYPE_PUT:
        case PTLLND_MSG_TYPE_GET:
                LASSERT (payload_nob == 0);
                msgsize = offsetof(kptl_msg_t, ptlm_u) +
                          sizeof(kptl_request_msg_t);
                break;

        case PTLLND_MSG_TYPE_IMMEDIATE:
                msgsize = offsetof(kptl_msg_t,
                                   ptlm_u.immediate.kptlim_payload[payload_nob]);
                break;

        case PTLLND_MSG_TYPE_NOOP:
                LASSERT (payload_nob == 0);
                msgsize = offsetof(kptl_msg_t, ptlm_u);
                break;

        case PTLLND_MSG_TYPE_HELLO:
                LASSERT (payload_nob == 0);
                msgsize = offsetof(kptl_msg_t, ptlm_u) +
                          sizeof(kptl_hello_msg_t);
                break;
        }

        LASSERT (msgsize <= peer->plp_max_msg_size);

        PJK_UT_MSG("msgsize=%d\n",msgsize);

        LIBCFS_ALLOC(tx, offsetof(ptllnd_tx_t, tx_msg) + msgsize);

        if (tx == NULL) {
                CERROR("Can't allocate msg type %d for %s\n",
                       type, libcfs_nid2str(peer->plp_nid));
                return NULL;
        }

        CFS_INIT_LIST_HEAD(&tx->tx_list);
        tx->tx_peer = peer;
        tx->tx_type = type;
        tx->tx_lnetmsg = tx->tx_lnetreplymsg = NULL;
        tx->tx_niov = 0;
        tx->tx_iov = NULL;
        tx->tx_reqmdh = PTL_INVALID_HANDLE;
        tx->tx_bulkmdh = PTL_INVALID_HANDLE;
        tx->tx_msgsize = msgsize;
        tx->tx_completing = 0;
        tx->tx_status = 0;

        if (msgsize != 0) {
                tx->tx_msg.ptlm_magic = PTLLND_MSG_MAGIC;
                tx->tx_msg.ptlm_version = PTLLND_MSG_VERSION;
                tx->tx_msg.ptlm_type = type;
                tx->tx_msg.ptlm_credits = 0;
                tx->tx_msg.ptlm_nob = msgsize;
                tx->tx_msg.ptlm_cksum = 0;
                tx->tx_msg.ptlm_srcnid = ni->ni_nid;
                tx->tx_msg.ptlm_srcstamp = plni->plni_stamp;
                tx->tx_msg.ptlm_dstnid = peer->plp_nid;
                tx->tx_msg.ptlm_dststamp = peer->plp_stamp;
                tx->tx_msg.ptlm_seq = peer->plp_seq++;
        }

        ptllnd_peer_addref(peer);
        plni->plni_ntxs++;

        PJK_UT_MSG("tx=%p\n",tx);

        return tx;
}

void
ptllnd_abort_tx(ptllnd_tx_t *tx, ptl_handle_md_t *mdh)
{
        ptllnd_peer_t   *peer = tx->tx_peer;
        lnet_ni_t       *ni = peer->plp_ni;
        int              rc;

        while (!PtlHandleIsEqual(*mdh, PTL_INVALID_HANDLE)) {
                rc = PtlMDUnlink(*mdh);
#ifndef LUSTRE_PORTALS_UNLINK_SEMANTICS
                if (rc == PTL_OK) /* unlink successful => no unlinked event */
                        return;
                LASSERT (rc == PTL_MD_IN_USE);
#endif
                /* Wait for ptllnd_tx_event() to invalidate */
                ptllnd_wait(ni, -1);
        }
}

void
ptllnd_tx_done(ptllnd_tx_t *tx)
{
        ptllnd_peer_t   *peer = tx->tx_peer;
        lnet_ni_t       *ni = peer->plp_ni;
        ptllnd_ni_t     *plni = ni->ni_data;

        /* CAVEAT EMPTOR: If this tx is being aborted, I'll continue to get
         * events for this tx until it's unlinked.  So I set tx_completing to
         * flag the tx is getting handled */

        PJK_UT_MSG(">>> tx=%p peer=%p\n",tx,peer);
        PJK_UT_MSG("completing=%d\n",tx->tx_completing);
        PJK_UT_MSG("status=%d\n",tx->tx_status);
        PJK_UT_MSG("niov=%d\n",tx->tx_niov);
        PJK_UT_MSG("lnetreplymsg=%p\n",tx->tx_lnetreplymsg);
        PJK_UT_MSG("lnetmsg=%p\n",tx->tx_lnetmsg);

        if (tx->tx_completing)
                return;

        tx->tx_completing = 1;

        if (!list_empty(&tx->tx_list))
                list_del_init(&tx->tx_list);

        if (tx->tx_status != 0)
                ptllnd_close_peer(peer);

        ptllnd_abort_tx(tx, &tx->tx_reqmdh);
        ptllnd_abort_tx(tx, &tx->tx_bulkmdh);

        if (tx->tx_niov > 0) {
                LIBCFS_FREE(tx->tx_iov, tx->tx_niov * sizeof(*tx->tx_iov));
                tx->tx_niov = 0;
        }

        if (tx->tx_lnetreplymsg != NULL) {
                LASSERT (tx->tx_type == PTLLND_MSG_TYPE_GET);
                LASSERT (tx->tx_lnetmsg != NULL);
                /* Simulate GET success always  */
                lnet_finalize(ni, tx->tx_lnetmsg, 0);
                PJK_UT_MSG("lnet_finalize(tx_lnetreplymsg=%p)\n",tx->tx_lnetreplymsg);
                lnet_finalize(ni, tx->tx_lnetreplymsg, tx->tx_status);
        } else if (tx->tx_lnetmsg != NULL) {
                lnet_finalize(ni, tx->tx_lnetmsg, tx->tx_status);
        }

        ptllnd_peer_decref(peer);

        LASSERT (plni->plni_ntxs > 0);
        plni->plni_ntxs--;
        LIBCFS_FREE(tx, offsetof(ptllnd_tx_t, tx_msg) + tx->tx_msgsize);

        PJK_UT_MSG("<<< tx=%p\n",tx);
}

void
ptllnd_abort_txs(lnet_ni_t *ni)
{
        ptllnd_ni_t   *plni = ni->ni_data;

        while (!list_empty(&plni->plni_active_txs)) {
                ptllnd_tx_t *tx = list_entry(plni->plni_active_txs.next,
                                             ptllnd_tx_t, tx_list);
                tx->tx_status = -ESHUTDOWN;
                ptllnd_tx_done(tx);
        }
}

int
ptllnd_set_txiov(ptllnd_tx_t *tx,
                 unsigned int niov, struct iovec *iov,
                 unsigned int offset, unsigned int len)
{
        ptl_md_iovec_t *piov;
        int             npiov;

        if (len == 0) {
                tx->tx_niov = 0;
                return 0;
        }

        for (;;) {
                LASSERT (niov > 0);
                if (offset < iov->iov_len)
                        break;
                offset -= iov->iov_len;
                niov--;
                iov++;
        }

        for (;;) {
                LIBCFS_ALLOC(piov, niov * sizeof(*piov));
                if (piov == NULL)
                        return -ENOMEM;

                for (npiov = 0;; npiov++) {
                        LASSERT (npiov < niov);
                        LASSERT (iov->iov_len >= offset);

                        piov[npiov].iov_base = iov[npiov].iov_base + offset;
                        piov[npiov].iov_len = iov[npiov].iov_len - offset;

                        if (piov[npiov].iov_len >= len) {
                                piov[npiov].iov_len = len;
                                npiov++;
                                break;
                        }
                        iov++;
                        offset = 0;
                }

                if (npiov == niov) {
                        tx->tx_niov = niov;
                        tx->tx_iov = piov;
                        return 0;
                }

                /* Dang! The piov I allocated was too big and it's a drag to
                 * have to maintain separate 'allocated' and 'used' sizes, so
                 * I'll just do it again; NB this doesn't happen normally... */
                LIBCFS_FREE(piov, niov * sizeof(*piov));
                niov = npiov;
        }
}

void
ptllnd_set_md_buffer(ptl_md_t *md, ptllnd_tx_t *tx)
{
        unsigned int    niov = tx->tx_niov;
        ptl_md_iovec_t *iov = tx->tx_iov;

        LASSERT ((md->options & PTL_MD_IOVEC) == 0);

        if (niov == 0) {
                md->start = NULL;
                md->length = 0;
        } else if (niov == 1) {
                md->start = iov[0].iov_base;
                md->length = iov[0].iov_len;
        } else {
                md->start = iov;
                md->length = niov;
                md->options |= PTL_MD_IOVEC;
        }
}

int
ptllnd_post_buffer(ptllnd_buffer_t *buf)
{
        lnet_ni_t        *ni = buf->plb_ni;
        ptllnd_ni_t      *plni = ni->ni_data;
        ptl_process_id_t  anyid = {
                .nid       = PTL_NID_ANY,
                .pid       = PTL_PID_ANY};
        ptl_md_t          md = {
                .start     = buf->plb_buffer,
                .length    = plni->plni_buffer_size,
                .threshold = PTL_MD_THRESH_INF,
                .max_size  = plni->plni_max_msg_size,
                .options   = (PTLLND_MD_OPTIONS |
                              PTL_MD_OP_PUT | PTL_MD_MAX_SIZE),
                .user_ptr  = ptllnd_obj2eventarg(buf, PTLLND_EVENTARG_TYPE_BUF),
                .eq_handle = plni->plni_eqh};
        ptl_handle_me_t meh;
        int             rc;

        LASSERT (!buf->plb_posted);

        rc = PtlMEAttach(plni->plni_nih, plni->plni_portal,
                         anyid, LNET_MSG_MATCHBITS, 0,
                         PTL_UNLINK, PTL_INS_AFTER, &meh);
        if (rc != PTL_OK) {
                CERROR("PtlMEAttach failed: %d\n", rc);
                return -ENOMEM;
        }

        buf->plb_posted = 1;
        plni->plni_nposted_buffers++;

        rc = PtlMDAttach(meh, md, LNET_UNLINK, &buf->plb_md);
        if (rc == PTL_OK)
                return 0;

        CERROR("PtlMDAttach failed: %d\n", rc);

        buf->plb_posted = 0;
        plni->plni_nposted_buffers--;

        rc = PtlMEUnlink(meh);
        LASSERT (rc == PTL_OK);

        return -ENOMEM;
}

void
ptllnd_check_sends(ptllnd_peer_t *peer)
{
        lnet_ni_t      *ni = peer->plp_ni;
        ptllnd_ni_t    *plni = ni->ni_data;
        ptllnd_tx_t    *tx;
        ptl_md_t        md;
        ptl_handle_md_t mdh;
        int             rc;

        PJK_UT_MSG(">>> peer=%p\n",peer);
        PJK_UT_MSG("plp_outstanding_credits=%d\n",peer->plp_outstanding_credits);

        if (list_empty(&peer->plp_txq) &&
            peer->plp_outstanding_credits >=
            PTLLND_CREDIT_HIGHWATER(plni)) {

                tx = ptllnd_new_tx(peer, PTLLND_MSG_TYPE_NOOP, 0);
                PJK_UT_MSG("NOOP tx=%p\n",tx);
                if (tx == NULL) {
                        CERROR("Can't return credits to %s\n",
                               libcfs_nid2str(peer->plp_nid));
                } else {
                        list_add_tail(&tx->tx_list, &peer->plp_txq);
                }
        }

        while (!list_empty(&peer->plp_txq)) {
                tx = list_entry(peer->plp_txq.next, ptllnd_tx_t, tx_list);

                PJK_UT_MSG("Looking at TX=%p\n",tx);
                PJK_UT_MSG("plp_credits=%d\n",peer->plp_credits);
                PJK_UT_MSG("plp_outstanding_credits=%d\n",peer->plp_outstanding_credits);

                LASSERT (tx->tx_msgsize > 0);

                LASSERT (peer->plp_outstanding_credits >= 0);
                LASSERT (peer->plp_outstanding_credits <=
                         plni->plni_peer_credits);
                LASSERT (peer->plp_credits >= 0);
                LASSERT (peer->plp_credits <= peer->plp_max_credits);

                if (peer->plp_credits == 0)     /* no credits */
                        break;

                if (peer->plp_credits == 1 &&   /* last credit reserved for */
                    peer->plp_outstanding_credits == 0) /* returning credits */
                        break;

                list_del_init(&tx->tx_list);

                PJK_UT_MSG("Sending at TX=%p type=%d\n",tx,tx->tx_type);

                if (tx->tx_type == PTLLND_MSG_TYPE_NOOP &&
                    (!list_empty(&peer->plp_txq) ||
                     peer->plp_outstanding_credits <
                     PTLLND_CREDIT_HIGHWATER(plni))) {
                        /* redundant NOOP */
                        ptllnd_tx_done(tx);
                        continue;
                }

                /*
                 * We need to set the stamp here because a hello
                 * could have modified peer->plp_stamp, after it
                 * was set to into the TX
                 */
                tx->tx_msg.ptlm_dststamp = peer->plp_stamp;

                PJK_UT_MSG("Returning %d to peer\n",peer->plp_outstanding_credits);

                /*
                 * Return all the credits we have
                 */
                tx->tx_msg.ptlm_credits = peer->plp_outstanding_credits;
                peer->plp_outstanding_credits = 0;

                /*
                 * One less credit
                 */
                peer->plp_credits--;


                md.user_ptr = ptllnd_obj2eventarg(tx, PTLLND_EVENTARG_TYPE_TX);
                md.eq_handle = plni->plni_eqh;
                md.threshold = 1;
                md.options = PTLLND_MD_OPTIONS;
                md.start = &tx->tx_msg;
                md.length = tx->tx_msgsize;

                rc = PtlMDBind(plni->plni_nih, md, LNET_UNLINK, &mdh);
                if (rc != PTL_OK) {
                        CERROR("PtlMDBind for %s failed: %d\n",
                               libcfs_nid2str(peer->plp_nid), rc);
                        tx->tx_status = -EIO;
                        ptllnd_tx_done(tx);
                        break;
                }

                tx->tx_reqmdh = mdh;
                rc = PtlPut(mdh, PTL_NOACK_REQ, peer->plp_ptlid,
                            plni->plni_portal, 0, LNET_MSG_MATCHBITS, 0, 0);
                if (rc != PTL_OK) {
                        CERROR("PtlPut for %s failed: %d\n",
                               libcfs_nid2str(peer->plp_nid), rc);
                        tx->tx_status = -EIO;
                        ptllnd_tx_done(tx);
                        break;
                }

                list_add_tail(&tx->tx_list, &plni->plni_active_txs);
        }

        PJK_UT_MSG("<<< peer=%p\n",peer);
}

int
ptllnd_passive_rdma(ptllnd_peer_t *peer, int type, lnet_msg_t *msg,
                    unsigned int niov, struct iovec *iov,
                    unsigned int offset, unsigned int len)
{
        lnet_ni_t      *ni = peer->plp_ni;
        ptllnd_ni_t    *plni = ni->ni_data;
        ptllnd_tx_t    *tx = ptllnd_new_tx(peer, type, 0);
        __u64           matchbits;
        ptl_md_t        md;
        ptl_handle_md_t mdh;
        ptl_handle_me_t meh;
        int             rc;
        int             rc2;

        PJK_UT_MSG(">>> peer=%p type=%s(%d) tx=%p\n",peer,
                type == PTLLND_MSG_TYPE_GET ? "GET" : "PUT/REPLY",type,tx);
        PJK_UT_MSG("niov=%d offset=%d len=%d\n",niov,offset,len);

        LASSERT (type == PTLLND_MSG_TYPE_GET ||
                 type == PTLLND_MSG_TYPE_PUT);

        if (tx == NULL) {
                CERROR("Can't allocate %s tx for %s\n",
                       type == PTLLND_MSG_TYPE_GET ? "GET" : "PUT/REPLY",
                       libcfs_nid2str(peer->plp_nid));
                return -ENOMEM;
        }

        rc = ptllnd_set_txiov(tx, niov, iov, offset, len);
        if (rc != 0) {
                CERROR ("Can't allocate iov %d for %s\n",
                        niov, libcfs_nid2str(peer->plp_nid));
                rc = -ENOMEM;
                goto failed;
        }

        md.user_ptr = ptllnd_obj2eventarg(tx, PTLLND_EVENTARG_TYPE_TX);
        md.eq_handle = plni->plni_eqh;
        md.threshold = 1;
        md.max_size = 0;
        md.options = PTLLND_MD_OPTIONS;
        if(type == PTLLND_MSG_TYPE_GET)
                md.options |= PTL_MD_OP_PUT | PTL_MD_ACK_DISABLE;
        else
                md.options |= PTL_MD_OP_GET;
        ptllnd_set_md_buffer(&md, tx);

        while (!peer->plp_recvd_hello) {        /* wait to validate plp_match */
                PJK_UT_MSG("Wait For Hello\n");
                if (peer->plp_closing) {
                        rc = -EIO;
                        goto failed;
                }
                ptllnd_wait(ni, -1);
        }

        if(peer->plp_match < PTL_RESERVED_MATCHBITS)
                peer->plp_match = PTL_RESERVED_MATCHBITS;
        matchbits = peer->plp_match++;
        PJK_UT_MSG("matchbits " LPX64 "\n",matchbits);
        PJK_UT_MSG("nid " FMT_NID " pid=%d\n",peer->plp_ptlid.nid,peer->plp_ptlid.pid);

        rc = PtlMEAttach(plni->plni_nih, plni->plni_portal, peer->plp_ptlid,
                         matchbits, 0, PTL_UNLINK, PTL_INS_BEFORE, &meh);
        if (rc != PTL_OK) {
                CERROR("PtlMEAttach for %s failed: %d\n",
                       libcfs_nid2str(peer->plp_nid), rc);
                rc = -EIO;
                goto failed;
        }

/*
        PJK_UT_MSG("md.start=%p\n",md.start);
        PJK_UT_MSG("md.length=%d\n",md.length);
        PJK_UT_MSG("md.threshold=%d\n",md.threshold);
        PJK_UT_MSG("md.max_size=%d\n",md.max_size);
        PJK_UT_MSG("md.options=0x%x\n",md.options);
        PJK_UT_MSG("md.user_ptr=%p\n",md.user_ptr);
        PJK_UT_MSG("md.eq_handle=%p\n",md.eq_handle);
*/
        rc = PtlMDAttach(meh, md, LNET_UNLINK, &mdh);
        if (rc != PTL_OK) {
                CERROR("PtlMDAttach for %s failed: %d\n",
                       libcfs_nid2str(peer->plp_nid), rc);
                rc2 = PtlMEUnlink(meh);
                LASSERT (rc2 == PTL_OK);
                rc = -EIO;
                goto failed;
        }
        tx->tx_bulkmdh = mdh;

        /*
         * We need to set the stamp here because it
         * we could have received a HELLO above that set
         * peer->plp_stamp
         */
        tx->tx_msg.ptlm_dststamp = peer->plp_stamp;

        tx->tx_msg.ptlm_u.req.kptlrm_hdr = msg->msg_hdr;
        tx->tx_msg.ptlm_u.req.kptlrm_matchbits = matchbits;

        if (type == PTLLND_MSG_TYPE_GET) {
                tx->tx_lnetreplymsg = lnet_create_reply_msg(ni, msg);
                if (tx->tx_lnetreplymsg == NULL) {
                        CERROR("Can't create reply for GET to %s\n",
                               libcfs_id2str(msg->msg_target));
                        rc = -ENOMEM;
                        goto failed;
                }
        }

        tx->tx_lnetmsg = msg;
        ptllnd_post_tx(tx);
        PJK_UT_MSG("<<<\n");
        return 0;

 failed:
        ptllnd_tx_done(tx);
        PJK_UT_MSG("<<< rc=%d\n",rc);
        return rc;
}

int
ptllnd_active_rdma(ptllnd_peer_t *peer, int type,
                   lnet_msg_t *msg, __u64 matchbits,
                   unsigned int niov, struct iovec *iov,
                   unsigned int offset, unsigned int len)
{
        lnet_ni_t       *ni = peer->plp_ni;
        ptllnd_ni_t     *plni = ni->ni_data;
        ptllnd_tx_t     *tx = ptllnd_new_tx(peer, type, 0);
        ptl_md_t         md;
        ptl_handle_md_t  mdh;
        int              rc;

        PJK_UT_MSG(">>> peer=%p type=%d tx=%p\n",peer,type,tx);
        PJK_UT_MSG("niov=%d offset=%d len=%d\n",niov,offset,len);
        PJK_UT_MSG("matchbits " LPX64 "\n",matchbits);

        LASSERT (type == PTLLND_RDMA_READ ||
                 type == PTLLND_RDMA_WRITE);

        if (tx == NULL) {
                CERROR("Can't allocate tx for RDMA %s with %s\n",
                       (type == PTLLND_RDMA_WRITE) ? "write" : "read",
                       libcfs_nid2str(peer->plp_nid));
                ptllnd_close_peer(peer);
                return -ENOMEM;
        }

        rc = ptllnd_set_txiov(tx, niov, iov, offset, len);
        if (rc != 0) {
                CERROR ("Can't allocate iov %d for %s\n",
                        niov, libcfs_nid2str(peer->plp_nid));
                rc = -ENOMEM;
                goto failed;
        }

        md.user_ptr = ptllnd_obj2eventarg(tx, PTLLND_EVENTARG_TYPE_TX);
        md.eq_handle = plni->plni_eqh;
        md.threshold = 1;
        md.max_size = 0;
        md.options = PTLLND_MD_OPTIONS;
        if(type == PTLLND_RDMA_READ){
                md.options |= PTL_MD_OP_GET;
                md.threshold ++;
        }
        else{
                md.options |= PTL_MD_OP_PUT | PTL_MD_ACK_DISABLE;
        }
        ptllnd_set_md_buffer(&md, tx);

        rc = PtlMDBind(plni->plni_nih, md, LNET_UNLINK, &mdh);
        if (rc != PTL_OK) {
                CERROR("PtlMDBind for %s failed: %d\n",
                       libcfs_nid2str(peer->plp_nid), rc);
                rc = -EIO;
                goto failed;
        }

        tx->tx_bulkmdh = mdh;
        tx->tx_lnetmsg = msg;

        if (type == PTLLND_RDMA_READ)
                rc = PtlGet(mdh, peer->plp_ptlid,
                            plni->plni_portal, 0, matchbits, 0);
        else
                rc = PtlPut(mdh, PTL_NOACK_REQ, peer->plp_ptlid,
                            plni->plni_portal, 0, matchbits, 0, 0);
        if (rc == 0){
                PJK_UT_MSG("<<<\n");
                return 0;
        }

        tx->tx_lnetmsg = NULL;
 failed:
        tx->tx_status = rc;
        ptllnd_tx_done(tx);    /* this will close peer */
        PJK_UT_MSG("<<< rc=%d\n",rc);
        return rc;
}

int
ptllnd_send(lnet_ni_t *ni, void *private, lnet_msg_t *msg)
{
        ptllnd_ni_t    *plni = ni->ni_data;
        ptllnd_peer_t  *plp;
        ptllnd_tx_t    *tx;
        int             nob;
        int             rc;

        LASSERT (!msg->msg_routing);
        LASSERT (msg->msg_kiov == NULL);

        LASSERT (msg->msg_niov <= PTL_MD_MAX_IOV); /* !!! */

        PJK_UT_MSG("msg=%p nid=" LPX64 "\n",msg,msg->msg_target.nid);
        PJK_UT_MSG("is_target_router=%d\n",msg->msg_target_is_router);
        PJK_UT_MSG("msg_niov=%d\n",msg->msg_niov);
        PJK_UT_MSG("msg_offset=%d\n",msg->msg_offset);
        PJK_UT_MSG("msg_len=%d\n",msg->msg_len);

        plp = ptllnd_find_peer(ni, msg->msg_target.nid, 1);
        if (plp == NULL)
                return -ENOMEM;

        switch (msg->msg_type) {
        default:
                LBUG();

        case LNET_MSG_ACK:
                PJK_UT_MSG("LNET_MSG_ACK\n");

                LASSERT (msg->msg_len == 0);
                break;                          /* send IMMEDIATE */

        case LNET_MSG_GET:
                PJK_UT_MSG("LNET_MSG_GET nob=%d\n",msg->msg_md->md_length);

                if (msg->msg_target_is_router)
                        break;                  /* send IMMEDIATE */

                nob = msg->msg_md->md_length;
                nob = offsetof(kptl_msg_t, ptlm_u.immediate.kptlim_payload[nob]);
                if (nob <= plni->plni_max_msg_size)
                        break;

                LASSERT ((msg->msg_md->md_options & LNET_MD_KIOV) == 0);
                rc = ptllnd_passive_rdma(plp, PTLLND_MSG_TYPE_GET, msg,
                                         msg->msg_md->md_niov,
                                         msg->msg_md->md_iov.iov,
                                         0, msg->msg_md->md_length);
                ptllnd_peer_decref(plp);
                PJK_UT_MSG("<<< rc=%d\n",rc);
                return rc;

        case LNET_MSG_REPLY: {
                ptllnd_rx_t *rx = private;      /* incoming GET */
                LASSERT (rx != NULL);

                PJK_UT_MSG("LNET_MSG_REPLY rx=%p\n",rx);

                if (rx->rx_msg->ptlm_type == PTLLND_MSG_TYPE_GET) {
                        __u64        matchbits;

                        matchbits = rx->rx_msg->ptlm_u.req.kptlrm_matchbits;
                        PJK_UT_MSG("matchbits="LPX64"\n",matchbits);

                        LASSERT (!rx->rx_replied);
                        rc = ptllnd_active_rdma(plp, PTLLND_RDMA_WRITE, msg,
                                                matchbits,
                                                msg->msg_niov, msg->msg_iov,
                                                msg->msg_offset, msg->msg_len);
                        rx->rx_replied = (rc == 0);
                        ptllnd_peer_decref(plp);
                        PJK_UT_MSG("<<< rc=%d\n",rc);
                        return rc;
                }

                if (rx->rx_msg->ptlm_type != PTLLND_MSG_TYPE_IMMEDIATE) {
                        CERROR("Reply to %s bad msg type %x!!!\n",
                               libcfs_id2str(msg->msg_target),
                               rx->rx_msg->ptlm_type);
                        ptllnd_peer_decref(plp);
                        return -EPROTO;
                }

                /* fall through to handle like PUT */
        }

        case LNET_MSG_PUT:
                PJK_UT_MSG("LNET_MSG_PUT nob=%d\n",msg->msg_len);
                nob = msg->msg_len;
                nob = offsetof(kptl_msg_t, ptlm_u.immediate.kptlim_payload[nob]);
                PJK_UT_MSG("msg_size=%d max=%d\n",msg->msg_len,plp->plp_max_msg_size);
                if (nob <= plp->plp_max_msg_size)
                        break;                  /* send IMMEDIATE */

                rc = ptllnd_passive_rdma(plp, PTLLND_MSG_TYPE_PUT, msg,
                                         msg->msg_niov, msg->msg_iov,
                                         msg->msg_offset, msg->msg_len);
                ptllnd_peer_decref(plp);
                PJK_UT_MSG("<<< rc=%d\n",rc);
                return rc;
        }

        /* send IMMEDIATE
         * NB copy the payload so we don't have to do a fragmented send */

        PJK_UT_MSG("IMMEDIATE len=%d\n", msg->msg_len);
        tx = ptllnd_new_tx(plp, PTLLND_MSG_TYPE_IMMEDIATE, msg->msg_len);
        if (tx == NULL) {
                CERROR("Can't allocate tx for lnet type %d to %s\n",
                       msg->msg_type, libcfs_id2str(msg->msg_target));
                ptllnd_peer_decref(plp);
                return -ENOMEM;
        }

        lnet_copy_iov2flat(tx->tx_msgsize, &tx->tx_msg,
                           offsetof(kptl_msg_t, ptlm_u.immediate.kptlim_payload),
                           msg->msg_niov, msg->msg_iov, msg->msg_offset,
                           msg->msg_len);
        tx->tx_msg.ptlm_u.immediate.kptlim_hdr = msg->msg_hdr;

        tx->tx_lnetmsg = msg;
        ptllnd_post_tx(tx);
        ptllnd_peer_decref(plp);
        PJK_UT_MSG("<<<\n");
        return 0;
}

void
ptllnd_rx_done(ptllnd_rx_t *rx)
{
        ptllnd_peer_t *plp = rx->rx_peer;
        lnet_ni_t     *ni = plp->plp_ni;
        ptllnd_ni_t   *plni = ni->ni_data;

        PJK_UT_MSG("rx=%p\n", rx);

        plp->plp_outstanding_credits++;
        ptllnd_check_sends(rx->rx_peer);

        if (rx->rx_msg != (kptl_msg_t *)rx->rx_space)
                LIBCFS_FREE(rx, offsetof(ptllnd_rx_t, rx_space[rx->rx_nob]));

        LASSERT (plni->plni_nrxs > 0);
        plni->plni_nrxs--;
}

int
ptllnd_eager_recv(lnet_ni_t *ni, void *private, lnet_msg_t *msg,
                  void **new_privatep)
{
        ptllnd_rx_t *stackrx = private;
        ptllnd_rx_t *heaprx;

        PJK_UT_MSG("rx=%p (stack)\n", stackrx);

        /* Don't ++plni_nrxs: heaprx replaces stackrx */

        LASSERT (stackrx->rx_msg != (kptl_msg_t *)stackrx->rx_space);

        LIBCFS_ALLOC(heaprx, offsetof(ptllnd_rx_t, rx_space[stackrx->rx_nob]));
        if (heaprx == NULL)
                return -ENOMEM;

        PJK_UT_MSG("rx=%p (new heap)\n", stackrx);

        heaprx->rx_msg = (kptl_msg_t *)heaprx->rx_space;
        memcpy(&heaprx->rx_msg, stackrx->rx_msg, stackrx->rx_nob);

        return 0;
}

int
ptllnd_recv(lnet_ni_t *ni, void *private, lnet_msg_t *msg,
            int delayed, unsigned int niov,
            struct iovec *iov, lnet_kiov_t *kiov,
            unsigned int offset, unsigned int mlen, unsigned int rlen)
{
        ptllnd_rx_t    *rx = private;
        int             rc = 0;
        int             nob;

        LASSERT (kiov == NULL);
        LASSERT (niov <= PTL_MD_MAX_IOV);       /* !!! */

        PJK_UT_MSG(">>> msg=%p\n",msg);
        PJK_UT_MSG("rx=%p rx_nob=%d\n",rx,rx->rx_nob);
        PJK_UT_MSG("niov=%d\n",niov);
        PJK_UT_MSG("offset=%d\n",offset);
        PJK_UT_MSG("mlen=%d rlen=%d\n",mlen,rlen);

        switch (rx->rx_msg->ptlm_type) {
        default:
                LBUG();

        case PTLLND_MSG_TYPE_IMMEDIATE:
                nob = offsetof(kptl_msg_t, ptlm_u.immediate.kptlim_payload[mlen]);
                PJK_UT_MSG("PTLLND_MSG_TYPE_IMMEDIATE nob=%d\n",nob);
                if (nob > rx->rx_nob) {
                        CERROR("Immediate message from %s too big: %d(%d)\n",
                               libcfs_nid2str(rx->rx_peer->plp_nid),
                               nob, rx->rx_nob);
                        rc = -EPROTO;
                        break;
                }
                lnet_copy_flat2iov(niov, iov, offset,
                                   rx->rx_nob, rx->rx_msg,
                                   offsetof(kptl_msg_t, ptlm_u.immediate.kptlim_payload),
                                   mlen);
                lnet_finalize(ni, msg, 0);
                break;

        case PTLLND_MSG_TYPE_PUT:
                PJK_UT_MSG("PTLLND_MSG_TYPE_PUT offset=%d mlen=%d\n",offset,mlen);
                rc = ptllnd_active_rdma(rx->rx_peer, PTLLND_RDMA_READ, msg,
                                        rx->rx_msg->ptlm_u.req.kptlrm_matchbits,
                                        niov, iov, offset, mlen);
                break;

        case PTLLND_MSG_TYPE_GET:
                PJK_UT_MSG("PTLLND_MSG_TYPE_GET\n");
                LASSERT (msg == NULL);          /* no need to finalize */
                if (!rx->rx_replied)            /* peer will time out */
                        ptllnd_close_peer(rx->rx_peer);
                break;
        }

        ptllnd_rx_done(rx);
        PJK_UT_MSG("<<< rc=%d\n",rc);
        return rc;
}

void
ptllnd_parse_request(lnet_ni_t *ni, ptl_process_id_t initiator,
                     kptl_msg_t *msg, unsigned int nob)
{
        ptllnd_ni_t   *plni = ni->ni_data;
        const int      basenob = offsetof(kptl_msg_t, ptlm_u);
        ptllnd_rx_t    rx;
        int            flip;
        ptllnd_peer_t *plp;
        int            rc;


        PJK_UT_MSG(">>> initiator =" LPX64 " nob=%d\n",initiator.nid,nob);

        if (nob < basenob) {
                CERROR("Short receive from %s\n",
                       ptllnd_ptlid2str(initiator));
                return;
        }

        flip = msg->ptlm_magic == __swab32(PTLLND_MSG_MAGIC);
        if (!flip && msg->ptlm_magic != PTLLND_MSG_MAGIC) {
                CERROR("Bad magic %08x from %s\n", msg->ptlm_magic,
                       ptllnd_ptlid2str(initiator));
                return;
        }

        if (flip) {
                /* NB stamps are opaque cookies */
                __swab16s(&msg->ptlm_version);
                __swab32s(&msg->ptlm_nob);
                __swab32s(&msg->ptlm_cksum);
                __swab64s(&msg->ptlm_srcnid);
                __swab64s(&msg->ptlm_dstnid);
                __swab64s(&msg->ptlm_seq);
        }

        if (msg->ptlm_version != PTLLND_MSG_VERSION) {
                CERROR("Bad version %d from %s\n", (__u32)msg->ptlm_version,
                       ptllnd_ptlid2str(initiator));
                return;
        }

        if (msg->ptlm_dstnid != ni->ni_nid) {
                CERROR("Bad dstnid %s (%s expected) from %s\n",
                       libcfs_nid2str(msg->ptlm_dstnid),
                       libcfs_nid2str(ni->ni_nid),
                       libcfs_nid2str(msg->ptlm_srcnid));
                return;
        }

        if (msg->ptlm_dststamp != plni->plni_stamp) {
                CERROR("Bad dststamp "LPX64"("LPX64" expected) from %s\n",
                       msg->ptlm_dststamp, plni->plni_stamp,
                       libcfs_nid2str(msg->ptlm_srcnid));
                return;
        }

        switch (msg->ptlm_type) {
        case PTLLND_MSG_TYPE_PUT:
        case PTLLND_MSG_TYPE_GET:
                PJK_UT_MSG("PTLLND_MSG_TYPE_%s\n",
                        msg->ptlm_type==PTLLND_MSG_TYPE_PUT ? "PUT" : "GET");
                if (nob < basenob + sizeof(kptl_request_msg_t)) {
                        CERROR("Short rdma request from %s(%s)\n",
                               libcfs_nid2str(msg->ptlm_srcnid),
                               ptllnd_ptlid2str(initiator));
                        return;
                }
                if (flip)
                        __swab64s(&msg->ptlm_u.req.kptlrm_matchbits);
                break;

        case PTLLND_MSG_TYPE_IMMEDIATE:
                PJK_UT_MSG("PTLLND_MSG_TYPE_IMMEDIATE\n");
                if (nob < offsetof(kptl_msg_t,
                                   ptlm_u.immediate.kptlim_payload)) {
                        CERROR("Short immediate from %s(%s)\n",
                               libcfs_nid2str(msg->ptlm_srcnid),
                               ptllnd_ptlid2str(initiator));
                        return;
                }
                break;

        case PTLLND_MSG_TYPE_HELLO:
                PJK_UT_MSG("PTLLND_MSG_TYPE_HELLO\n");
                if (nob < basenob + sizeof(kptl_hello_msg_t)) {
                        CERROR("Short hello from %s(%s)\n",
                               libcfs_nid2str(msg->ptlm_srcnid),
                               ptllnd_ptlid2str(initiator));
                        return;
                }
                if(flip){
                        __swab64s(&msg->ptlm_u.hello.kptlhm_matchbits);
                        __swab32s(&msg->ptlm_u.hello.kptlhm_max_msg_size);
                }
                break;

        default:
                CERROR("Bad message type %d from %s(%s)\n", msg->ptlm_type,
                       libcfs_nid2str(msg->ptlm_srcnid),
                       ptllnd_ptlid2str(initiator));
                return;
        }

        plp = ptllnd_find_peer(ni, msg->ptlm_srcnid,
                               msg->ptlm_type == PTLLND_MSG_TYPE_HELLO);
        if (plp == NULL) {
                CERROR("Can't find peer %s\n",
                       libcfs_nid2str(msg->ptlm_srcnid));
                return;
        }

        if (msg->ptlm_type == PTLLND_MSG_TYPE_HELLO) {
                if (plp->plp_recvd_hello) {
                        CERROR("Unexpected HELLO from %s\n",
                               libcfs_nid2str(msg->ptlm_srcnid));
                        ptllnd_peer_decref(plp);
                        return;
                }

                PJK_UT_MSG("kptlhm_max_msg_size=%d\n",msg->ptlm_u.hello.kptlhm_max_msg_size);
                PJK_UT_MSG("kptlhm_matchbits="LPX64"\n",msg->ptlm_u.hello.kptlhm_matchbits);
                PJK_UT_MSG("ptlm_srcstamp="LPX64"\n",msg->ptlm_srcstamp);

                plp->plp_max_msg_size = MAX(plni->plni_max_msg_size,
                        msg->ptlm_u.hello.kptlhm_max_msg_size);
                plp->plp_match = msg->ptlm_u.hello.kptlhm_matchbits;
                plp->plp_stamp = msg->ptlm_srcstamp;
                plp->plp_max_credits += msg->ptlm_credits;
                plp->plp_recvd_hello = 1;

                PJK_UT_MSG("plp_max_msg_size=%d\n",plp->plp_max_msg_size);

        } else if (!plp->plp_recvd_hello) {

                CERROR("Bad message type %d (HELLO expected) from %s\n",
                       msg->ptlm_type, libcfs_nid2str(msg->ptlm_srcnid));
                ptllnd_peer_decref(plp);
                return;

        } else if (msg->ptlm_srcstamp != plp->plp_stamp) {

                CERROR("Bad srcstamp "LPX64"("LPX64" expected) from %s\n",
                       msg->ptlm_srcstamp, plp->plp_stamp,
                       libcfs_nid2str(msg->ptlm_srcnid));
                ptllnd_peer_decref(plp);
                return;
        }

        if (msg->ptlm_credits > 0) {
                PJK_UT_MSG("Getting back %d credits from peer\n",msg->ptlm_credits);
                if (plp->plp_credits + msg->ptlm_credits >
                    plp->plp_max_credits) {
                        CWARN("Too many credits from %s: %d + %d > %d\n",
                              libcfs_nid2str(msg->ptlm_srcnid),
                              plp->plp_credits, msg->ptlm_credits,
                              plp->plp_max_credits);
                        plp->plp_credits = plp->plp_max_credits;
                } else {
                        plp->plp_credits += msg->ptlm_credits;
                }
                ptllnd_check_sends(plp);
        }

        /* All OK so far; assume the message is good... */

        rx.rx_peer      = plp;
        rx.rx_msg       = msg;
        rx.rx_nob       = nob;
        rx.rx_replied   = 0;
        plni->plni_nrxs++;

        PJK_UT_MSG("rx=%p type=%d\n",&rx,msg->ptlm_type);

        switch (msg->ptlm_type) {
        default: /* message types have been checked already */
                ptllnd_rx_done(&rx);
                break;

        case PTLLND_MSG_TYPE_PUT:
        case PTLLND_MSG_TYPE_GET:
                PJK_UT_MSG("PTLLND_MSG_TYPE_%s\n",
                        msg->ptlm_type==PTLLND_MSG_TYPE_PUT ? "PUT" : "GET");
                rc = lnet_parse(ni, &msg->ptlm_u.req.kptlrm_hdr,
                                msg->ptlm_srcnid, &rx);
                PJK_UT_MSG("lnet_parse rc=%d\n",rc);
                if (rc < 0)
                        ptllnd_rx_done(&rx);
                break;

        case PTLLND_MSG_TYPE_IMMEDIATE:
                PJK_UT_MSG("PTLLND_MSG_TYPE_IMMEDIATE\n");
                rc = lnet_parse(ni, &msg->ptlm_u.immediate.kptlim_hdr,
                                msg->ptlm_srcnid, &rx);
                PJK_UT_MSG("lnet_parse rc=%d\n",rc);
                if (rc < 0)
                        ptllnd_rx_done(&rx);
                break;
        }

        ptllnd_peer_decref(plp);

        PJK_UT_MSG("<<<\n");
}

void
ptllnd_buf_event (lnet_ni_t *ni, ptl_event_t *event)
{
        ptllnd_buffer_t *buf = ptllnd_eventarg2obj(event->md.user_ptr);
        ptllnd_ni_t     *plni = ni->ni_data;
        char            *msg = &buf->plb_buffer[event->offset];
        int              repost;
        int              unlinked = event->type == PTL_EVENT_UNLINK;

        LASSERT (buf->plb_ni == ni);
        LASSERT (event->type == PTL_EVENT_PUT_END ||
                 event->type == PTL_EVENT_UNLINK);

        PJK_UT_MSG("buf=%p event=%d\n",buf,event->type);

        if (event->type == PTL_EVENT_PUT_END)
                ptllnd_parse_request(ni, event->initiator,
                                     (kptl_msg_t *)msg, event->mlength);

#ifdef LUSTRE_PORTALS_UNLINK_SEMANTICS
        /* UNLINK event only on explicit unlink */
        repost = (event->unlinked && event->type != PTL_EVENT_UNLINK);
        if(event->unlinked)
                unlinked = 1;
#else
        /* UNLINK event only on implicit unlink */
        repost = (event->type == PTL_EVENT_UNLINK);
#endif

        PJK_UT_MSG("repost=%d unlinked=%d\n",repost,unlinked);

        if(unlinked){
                LASSERT(buf->plb_posted);
                buf->plb_posted = 0;
                plni->plni_nposted_buffers--;
        }

        if (repost)
                (void) ptllnd_post_buffer(buf);
}

void
ptllnd_tx_event (lnet_ni_t *ni, ptl_event_t *event)
{
        ptllnd_ni_t *plni = ni->ni_data;
        ptllnd_tx_t *tx = ptllnd_eventarg2obj(event->md.user_ptr);
        int          error = (event->ni_fail_type != PTL_NI_OK);
        int          isreq;
        int          isbulk;
#ifdef LUSTRE_PORTALS_UNLINK_SEMANTICS
        int          unlinked = event->unlinked;
#else
        int          unlinked = (event->type == PTL_EVENT_UNLINK);
#endif

        LASSERT (!PtlHandleIsEqual(event->md_handle, PTL_INVALID_HANDLE));

        PJK_UT_MSG("tx=%p type=%s (%d)\n",tx,
                get_msg_type_string(tx->tx_type),tx->tx_type);
        PJK_UT_MSG("unlinked=%d\n",unlinked);
        PJK_UT_MSG("error=%d\n",error);

        isreq = PtlHandleIsEqual(event->md_handle, tx->tx_reqmdh);
        PJK_UT_MSG("isreq=%d\n",isreq);
        if (isreq) {
                LASSERT (event->md.start == (void *)&tx->tx_msg);
                if (unlinked)
                        tx->tx_reqmdh = PTL_INVALID_HANDLE;
        }


        isbulk = PtlHandleIsEqual(event->md_handle, tx->tx_bulkmdh);
        PJK_UT_MSG("isbulk=%d\n",isbulk);
        if (isbulk) {
                void *ptr;

                if ((event->md.options & PTL_MD_IOVEC) == 0)
                        ptr = tx->tx_iov[0].iov_base;
                else
                        ptr = tx->tx_iov;

                LASSERT (event->md.start == ptr);
                if (unlinked)
                        tx->tx_bulkmdh = PTL_INVALID_HANDLE;
        }

        LASSERT (!isreq != !isbulk);            /* always one and only 1 match */

        switch (tx->tx_type) {
        default:
                LBUG();

        case PTLLND_MSG_TYPE_NOOP:
        case PTLLND_MSG_TYPE_HELLO:
        case PTLLND_MSG_TYPE_IMMEDIATE:
                LASSERT (event->type == PTL_EVENT_UNLINK ||
                         event->type == PTL_EVENT_SEND_END);
                LASSERT (isreq);
                break;

        case PTLLND_MSG_TYPE_GET:
                LASSERT (event->type == PTL_EVENT_UNLINK ||
                         (isreq && event->type == PTL_EVENT_SEND_END) ||
                         (isbulk && event->type == PTL_EVENT_REPLY_END));

                if (isbulk && event->type == PTL_EVENT_REPLY_END) {
                        LASSERT (tx->tx_lnetreplymsg != NULL);
                        tx->tx_lnetreplymsg->msg_ev.mlength =
                                event->mlength;
                }
                break;

        case PTLLND_MSG_TYPE_PUT:
                LASSERT (event->type == PTL_EVENT_UNLINK ||
                         (isreq && event->type == PTL_EVENT_SEND_END) ||
                         (isbulk && event->type == PTL_EVENT_GET_END));
                break;

        case PTLLND_RDMA_READ:
                LASSERT (event->type == PTL_EVENT_UNLINK ||
                         event->type == PTL_EVENT_SEND_END ||
                         event->type == PTL_EVENT_REPLY_END);
                LASSERT (isbulk);
                break;

        case PTLLND_RDMA_WRITE:
                LASSERT (event->type == PTL_EVENT_UNLINK ||
                         event->type == PTL_EVENT_SEND_END);
                LASSERT (isbulk);
        }

        /* Schedule ptllnd_tx_done() on error last completion event */
        if (error ||
            (PtlHandleIsEqual(tx->tx_bulkmdh, PTL_INVALID_HANDLE) &&
             PtlHandleIsEqual(tx->tx_reqmdh, PTL_INVALID_HANDLE))) {
                if (error)
                        tx->tx_status = -EIO;
                list_del(&tx->tx_list);
                list_add_tail(&tx->tx_list, &plni->plni_zombie_txs);
                PJK_UT_MSG("tx=%p ONTO ZOMBIE LIST\n",tx);
        }
}

void
ptllnd_wait (lnet_ni_t *ni, int milliseconds)
{
        ptllnd_ni_t   *plni = ni->ni_data;
        ptllnd_tx_t   *tx;
        ptl_event_t    event;
        int            which;
        int            rc;
        int            blocked = 0;
        int            found = 0;
        int            timeout = 0;

        PJK_UT_MSG(">>> ms=%d\n",milliseconds);

        /* Handle any currently queued events, returning immediately if any.
         * Otherwise block for the timeout and handle all events queued
         * then. */

        for (;;) {
                rc = PtlEQPoll(&plni->plni_eqh, 1, timeout, &event, &which);
                timeout = 0;
                PJK_UT_MSG("PtlEQPoll rc=%d\n",rc);

                if (rc == PTL_EQ_EMPTY) {
                        if (found ||            /* handled some events */
                            milliseconds == 0 || /* just checking */
                            blocked){            /* blocked already */
                                PJK_UT_MSG("found=%d blocked=%d\n",found,blocked);
                                break;
                            }

                        blocked = 1;
                        timeout = milliseconds;
                        continue;
                }

                LASSERT (rc == PTL_OK || rc == PTL_EQ_DROPPED);

                if (rc == PTL_EQ_DROPPED)
                        CERROR("Event queue: size %d is too small\n",
                               plni->plni_eq_size);

                PJK_UT_MSG("event.type=%s(%d)\n",
                        get_ev_type_string(event.type),event.type);

                found = 1;
                switch (ptllnd_eventarg2type(event.md.user_ptr)) {
                default:
                        LBUG();

                case PTLLND_EVENTARG_TYPE_TX:
                        ptllnd_tx_event(ni, &event);
                        break;

                case PTLLND_EVENTARG_TYPE_BUF:
                        ptllnd_buf_event(ni, &event);
                        break;
                }
        }

        while (!list_empty(&plni->plni_zombie_txs)) {
                tx = list_entry(plni->plni_zombie_txs.next,
                                ptllnd_tx_t, tx_list);
                PJK_UT_MSG("Process ZOMBIE tx=%p\n",tx);
                ptllnd_tx_done(tx);
        }

        PJK_UT_MSG("<<<\n");
}
