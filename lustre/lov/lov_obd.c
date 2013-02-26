/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright (c) 2011, 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/lov/lov_obd.c
 *
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Mike Shaver <shaver@clusterfs.com>
 * Author: Nathan Rutman <nathan@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LOV
#ifdef __KERNEL__
#include <libcfs/libcfs.h>
#else
#include <liblustre.h>
#endif

#include <obd_support.h>
#include <lustre_lib.h>
#include <lustre_net.h>
#include <lustre/lustre_idl.h>
#include <lustre_dlm.h>
#include <lustre_mds.h>
#include <lustre_debug.h>
#include <obd_class.h>
#include <obd_lov.h>
#include <obd_ost.h>
#include <lprocfs_status.h>
#include <lustre_param.h>
#include <cl_object.h>
#include <lustre/ll_fiemap.h>

#include "lov_internal.h"

/* Keep a refcount of lov->tgt usage to prevent racing with addition/deletion.
   Any function that expects lov_tgts to remain stationary must take a ref. */
static void lov_getref(struct obd_device *obd)
{
        struct lov_obd *lov = &obd->u.lov;

        /* nobody gets through here until lov_putref is done */
        cfs_mutex_down(&lov->lov_lock);
        cfs_atomic_inc(&lov->lov_refcount);
        cfs_mutex_up(&lov->lov_lock);
        return;
}

static void __lov_del_obd(struct obd_device *obd, struct lov_tgt_desc *tgt);

static void lov_putref(struct obd_device *obd)
{
        struct lov_obd *lov = &obd->u.lov;

        cfs_mutex_down(&lov->lov_lock);
        /* ok to dec to 0 more than once -- ltd_exp's will be null */
        if (cfs_atomic_dec_and_test(&lov->lov_refcount) && lov->lov_death_row) {
                CFS_LIST_HEAD(kill);
                int i;
                struct lov_tgt_desc *tgt, *n;
                CDEBUG(D_CONFIG, "destroying %d lov targets\n",
                       lov->lov_death_row);
                for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                        tgt = lov->lov_tgts[i];

                        if (!tgt || !tgt->ltd_reap)
                                continue;
                        cfs_list_add(&tgt->ltd_kill, &kill);
                        /* XXX - right now there is a dependency on ld_tgt_count
                         * being the maximum tgt index for computing the
                         * mds_max_easize. So we can't shrink it. */
                        lov_ost_pool_remove(&lov->lov_packed, i);
                        lov->lov_tgts[i] = NULL;
                        lov->lov_death_row--;
                }
                cfs_mutex_up(&lov->lov_lock);

                cfs_list_for_each_entry_safe(tgt, n, &kill, ltd_kill) {
                        cfs_list_del(&tgt->ltd_kill);
                        /* Disconnect */
                        __lov_del_obd(obd, tgt);
                }
        } else {
                cfs_mutex_up(&lov->lov_lock);
        }
}

static int lov_set_osc_active(struct obd_device *obd, struct obd_uuid *uuid,
                              enum obd_notify_event ev);
static int lov_notify(struct obd_device *obd, struct obd_device *watched,
                      enum obd_notify_event ev, void *data);


#define MAX_STRING_SIZE 128
int lov_connect_obd(struct obd_device *obd, __u32 index, int activate,
                    struct obd_connect_data *data)
{
        struct lov_obd *lov = &obd->u.lov;
        struct obd_uuid *tgt_uuid;
        struct obd_device *tgt_obd;
        static struct obd_uuid lov_osc_uuid = { "LOV_OSC_UUID" };
        struct obd_import *imp;
#ifdef __KERNEL__
        cfs_proc_dir_entry_t *lov_proc_dir;
#endif
        int rc;
        ENTRY;

        if (!lov->lov_tgts[index])
                RETURN(-EINVAL);

        tgt_uuid = &lov->lov_tgts[index]->ltd_uuid;
        tgt_obd = lov->lov_tgts[index]->ltd_obd;

        if (!tgt_obd->obd_set_up) {
                CERROR("Target %s not set up\n", obd_uuid2str(tgt_uuid));
                RETURN(-EINVAL);
        }

        /* override the sp_me from lov */
        tgt_obd->u.cli.cl_sp_me = lov->lov_sp_me;

        if (data && (data->ocd_connect_flags & OBD_CONNECT_INDEX))
                data->ocd_index = index;

        /*
         * Divine LOV knows that OBDs under it are OSCs.
         */
        imp = tgt_obd->u.cli.cl_import;

        if (activate) {
                tgt_obd->obd_no_recov = 0;
                /* FIXME this is probably supposed to be
                   ptlrpc_set_import_active.  Horrible naming. */
                ptlrpc_activate_import(imp);
        }

        rc = obd_register_observer(tgt_obd, obd);
        if (rc) {
                CERROR("Target %s register_observer error %d\n",
                       obd_uuid2str(tgt_uuid), rc);
                RETURN(rc);
        }


        if (imp->imp_invalid) {
                CDEBUG(D_CONFIG, "not connecting OSC %s; administratively "
                       "disabled\n", obd_uuid2str(tgt_uuid));
                RETURN(0);
        }

        rc = obd_connect(NULL, &lov->lov_tgts[index]->ltd_exp, tgt_obd,
                         &lov_osc_uuid, data, NULL);
        if (rc || !lov->lov_tgts[index]->ltd_exp) {
                CERROR("Target %s connect error %d\n",
                       obd_uuid2str(tgt_uuid), rc);
                RETURN(-ENODEV);
        }

        lov->lov_tgts[index]->ltd_reap = 0;

        CDEBUG(D_CONFIG, "Connected tgt idx %d %s (%s) %sactive\n", index,
               obd_uuid2str(tgt_uuid), tgt_obd->obd_name, activate ? "":"in");

#ifdef __KERNEL__
        lov_proc_dir = lprocfs_srch(obd->obd_proc_entry, "target_obds");
        if (lov_proc_dir) {
                struct obd_device *osc_obd = lov->lov_tgts[index]->ltd_exp->exp_obd;
                cfs_proc_dir_entry_t *osc_symlink;

                LASSERT(osc_obd != NULL);
                LASSERT(osc_obd->obd_magic == OBD_DEVICE_MAGIC);
                LASSERT(osc_obd->obd_type->typ_name != NULL);

                osc_symlink = lprocfs_add_symlink(osc_obd->obd_name,
                                                  lov_proc_dir,
                                                  "../../../%s/%s",
                                                  osc_obd->obd_type->typ_name,
                                                  osc_obd->obd_name);
                if (osc_symlink == NULL) {
                        CERROR("could not register LOV target "
                                "/proc/fs/lustre/%s/%s/target_obds/%s.",
                                obd->obd_type->typ_name, obd->obd_name,
                                osc_obd->obd_name);
                        lprocfs_remove(&lov_proc_dir);
                }
        }
#endif

        rc = qos_add_tgt(obd, index);
        if (rc)
                CERROR("qos_add_tgt failed %d\n", rc);

        RETURN(0);
}

static int lov_connect(const struct lu_env *env,
                       struct obd_export **exp, struct obd_device *obd,
                       struct obd_uuid *cluuid, struct obd_connect_data *data,
                       void *localdata)
{
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *tgt;
        struct lustre_handle conn;
        int i, rc;
        ENTRY;

        CDEBUG(D_CONFIG, "connect #%d\n", lov->lov_connects);

        rc = class_connect(&conn, obd, cluuid);
        if (rc)
                RETURN(rc);

        *exp = class_conn2export(&conn);

        /* Why should there ever be more than 1 connect? */
        lov->lov_connects++;
        LASSERT(lov->lov_connects == 1);

        memset(&lov->lov_ocd, 0, sizeof(lov->lov_ocd));
        if (data)
                lov->lov_ocd = *data;

        obd_getref(obd);
        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                tgt = lov->lov_tgts[i];
                if (!tgt || obd_uuid_empty(&tgt->ltd_uuid))
                        continue;
                /* Flags will be lowest common denominator */
                rc = lov_connect_obd(obd, i, tgt->ltd_activate, &lov->lov_ocd);
                if (rc) {
                        CERROR("%s: lov connect tgt %d failed: %d\n",
                               obd->obd_name, i, rc);
                        continue;
                }
                /* connect to administrative disabled ost */
                if (!lov->lov_tgts[i]->ltd_exp)
                        continue;

                rc = lov_notify(obd, lov->lov_tgts[i]->ltd_exp->exp_obd,
                                OBD_NOTIFY_CONNECT, (void *)&i);
                if (rc) {
                        CERROR("%s error sending notify %d\n",
                               obd->obd_name, rc);
                }
        }
        obd_putref(obd);

        RETURN(0);
}

static int lov_disconnect_obd(struct obd_device *obd, struct lov_tgt_desc *tgt)
{
        cfs_proc_dir_entry_t *lov_proc_dir;
        struct lov_obd *lov = &obd->u.lov;
        struct obd_device *osc_obd;
        int rc;
        ENTRY;

        osc_obd = class_exp2obd(tgt->ltd_exp);
        CDEBUG(D_CONFIG, "%s: disconnecting target %s\n",
               obd->obd_name, osc_obd->obd_name);

        if (tgt->ltd_active) {
                tgt->ltd_active = 0;
                lov->desc.ld_active_tgt_count--;
                tgt->ltd_exp->exp_obd->obd_inactive = 1;
        }

        lov_proc_dir = lprocfs_srch(obd->obd_proc_entry, "target_obds");
        if (lov_proc_dir) {
                cfs_proc_dir_entry_t *osc_symlink;

                osc_symlink = lprocfs_srch(lov_proc_dir, osc_obd->obd_name);
                if (osc_symlink) {
                        lprocfs_remove(&osc_symlink);
                } else {
                        CERROR("/proc/fs/lustre/%s/%s/target_obds/%s missing.",
                               obd->obd_type->typ_name, obd->obd_name,
                               osc_obd->obd_name);
                }
        }

        if (osc_obd) {
                /* Pass it on to our clients.
                 * XXX This should be an argument to disconnect,
                 * XXX not a back-door flag on the OBD.  Ah well.
                 */
                osc_obd->obd_force = obd->obd_force;
                osc_obd->obd_fail = obd->obd_fail;
                osc_obd->obd_no_recov = obd->obd_no_recov;
        }

        obd_register_observer(osc_obd, NULL);

        rc = obd_disconnect(tgt->ltd_exp);
        if (rc) {
                CERROR("Target %s disconnect error %d\n",
                       tgt->ltd_uuid.uuid, rc);
                rc = 0;
        }

        qos_del_tgt(obd, tgt);

        tgt->ltd_exp = NULL;
        RETURN(0);
}

static int lov_disconnect(struct obd_export *exp)
{
        struct obd_device *obd = class_exp2obd(exp);
        struct lov_obd *lov = &obd->u.lov;
        int i, rc;
        ENTRY;

        if (!lov->lov_tgts)
                goto out;

        /* Only disconnect the underlying layers on the final disconnect. */
        lov->lov_connects--;
        if (lov->lov_connects != 0) {
                /* why should there be more than 1 connect? */
                CERROR("disconnect #%d\n", lov->lov_connects);
                goto out;
        }

        /* Let's hold another reference so lov_del_obd doesn't spin through
           putref every time */
        obd_getref(obd);

        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                if (lov->lov_tgts[i] && lov->lov_tgts[i]->ltd_exp) {
                        /* Disconnection is the last we know about an obd */
                        lov_del_target(obd, i, 0, lov->lov_tgts[i]->ltd_gen);
                }
        }
        obd_putref(obd);

out:
        rc = class_disconnect(exp); /* bz 9811 */
        RETURN(rc);
}

/* Error codes:
 *
 *  -EINVAL  : UUID can't be found in the LOV's target list
 *  -ENOTCONN: The UUID is found, but the target connection is bad (!)
 *  -EBADF   : The UUID is found, but the OBD is the wrong type (!)
 *  any >= 0 : is log target index
 */
static int lov_set_osc_active(struct obd_device *obd, struct obd_uuid *uuid,
                              enum obd_notify_event ev)
{
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *tgt;
        int index, activate, active;
        ENTRY;

        CDEBUG(D_INFO, "Searching in lov %p for uuid %s event(%d)\n",
               lov, uuid->uuid, ev);

        obd_getref(obd);
        for (index = 0; index < lov->desc.ld_tgt_count; index++) {
                tgt = lov->lov_tgts[index];
                if (!tgt || !tgt->ltd_exp)
                        continue;

                CDEBUG(D_INFO, "lov idx %d is %s conn "LPX64"\n",
                       index, obd_uuid2str(&tgt->ltd_uuid),
                       tgt->ltd_exp->exp_handle.h_cookie);
                if (obd_uuid_equals(uuid, &tgt->ltd_uuid))
                        break;
        }

        if (index == lov->desc.ld_tgt_count)
                GOTO(out, index = -EINVAL);

        if (ev == OBD_NOTIFY_DEACTIVATE || ev == OBD_NOTIFY_ACTIVATE) {
                activate = (ev == OBD_NOTIFY_ACTIVATE) ? 1 : 0;

                if (lov->lov_tgts[index]->ltd_activate == activate) {
                        CDEBUG(D_INFO, "OSC %s already %sactivate!\n",
                               uuid->uuid, activate ? "" : "de");
                } else {
                        lov->lov_tgts[index]->ltd_activate = activate;
                        CDEBUG(D_CONFIG, "%sactivate OSC %s\n",
                               activate ? "" : "de", obd_uuid2str(uuid));
                }

        } else if (ev == OBD_NOTIFY_INACTIVE || ev == OBD_NOTIFY_ACTIVE) {
                active = (ev == OBD_NOTIFY_ACTIVE) ? 1 : 0;

                if (lov->lov_tgts[index]->ltd_active == active) {
                        CDEBUG(D_INFO, "OSC %s already %sactive!\n",
                               uuid->uuid, active ? "" : "in");
                        GOTO(out, index);
                } else {
                        CDEBUG(D_CONFIG, "Marking OSC %s %sactive\n",
                               obd_uuid2str(uuid), active ? "" : "in");
                }

                lov->lov_tgts[index]->ltd_active = active;
                if (active) {
                        lov->desc.ld_active_tgt_count++;
                        lov->lov_tgts[index]->ltd_exp->exp_obd->obd_inactive = 0;
                } else {
                        lov->desc.ld_active_tgt_count--;
                        lov->lov_tgts[index]->ltd_exp->exp_obd->obd_inactive = 1;
                }
                /* remove any old qos penalty */
                lov->lov_tgts[index]->ltd_qos.ltq_penalty = 0;
        } else {
                CERROR("Unknown event(%d) for uuid %s", ev, uuid->uuid);
        }

 out:
        obd_putref(obd);
        RETURN(index);
}

static int lov_notify(struct obd_device *obd, struct obd_device *watched,
                      enum obd_notify_event ev, void *data)
{
        int rc = 0;
        struct lov_obd *lov = &obd->u.lov;
        ENTRY;

        cfs_down_read(&lov->lov_notify_lock);
        if (!lov->lov_connects) {
                cfs_up_read(&lov->lov_notify_lock);
                RETURN(rc);
        }

        if (ev == OBD_NOTIFY_ACTIVE || ev == OBD_NOTIFY_INACTIVE ||
            ev == OBD_NOTIFY_ACTIVATE || ev == OBD_NOTIFY_DEACTIVATE) {
                struct obd_uuid *uuid;

                LASSERT(watched);

                if (strcmp(watched->obd_type->typ_name, LUSTRE_OSC_NAME)) {
                        cfs_up_read(&lov->lov_notify_lock);
                        CERROR("unexpected notification of %s %s!\n",
                               watched->obd_type->typ_name,
                               watched->obd_name);
                        RETURN(-EINVAL);
                }
                uuid = &watched->u.cli.cl_target_uuid;

                /* Set OSC as active before notifying the observer, so the
                 * observer can use the OSC normally.
                 */
                rc = lov_set_osc_active(obd, uuid, ev);
                if (rc < 0) {
                        cfs_up_read(&lov->lov_notify_lock);
                        CERROR("event(%d) of %s failed: %d\n", ev,
                               obd_uuid2str(uuid), rc);
                        RETURN(rc);
                }
                /* active event should be pass lov target index as data */
                data = &rc;
        }

        /* Pass the notification up the chain. */
        if (watched) {
                rc = obd_notify_observer(obd, watched, ev, data);
        } else {
                /* NULL watched means all osc's in the lov (only for syncs) */
                /* sync event should be send lov idx as data */
                struct lov_obd *lov = &obd->u.lov;
                int i, is_sync;

                data = &i;
                is_sync = (ev == OBD_NOTIFY_SYNC) ||
                          (ev == OBD_NOTIFY_SYNC_NONBLOCK);

                obd_getref(obd);
                for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                        if (!lov->lov_tgts[i])
                                continue;

                        /* don't send sync event if target not
                         * connected/activated */
                        if (is_sync &&  !lov->lov_tgts[i]->ltd_active)
                                continue;

                        rc = obd_notify_observer(obd, lov->lov_tgts[i]->ltd_obd,
                                                 ev, data);
                        if (rc) {
                                CERROR("%s: notify %s of %s failed %d\n",
                                       obd->obd_name,
                                       obd->obd_observer->obd_name,
                                       lov->lov_tgts[i]->ltd_obd->obd_name,
                                       rc);
                        }
                }
                obd_putref(obd);
        }

        cfs_up_read(&lov->lov_notify_lock);
        RETURN(rc);
}

static int lov_add_target(struct obd_device *obd, struct obd_uuid *uuidp,
                          __u32 index, int gen, int active)
{
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *tgt;
        struct obd_device *tgt_obd;
        int rc;
        ENTRY;

        CDEBUG(D_CONFIG, "uuid:%s idx:%d gen:%d active:%d\n",
               uuidp->uuid, index, gen, active);

        if (gen <= 0) {
                CERROR("request to add OBD %s with invalid generation: %d\n",
                       uuidp->uuid, gen);
                RETURN(-EINVAL);
        }

        tgt_obd = class_find_client_obd(uuidp, LUSTRE_OSC_NAME,
                                        &obd->obd_uuid);
        if (tgt_obd == NULL)
                RETURN(-EINVAL);

        cfs_mutex_down(&lov->lov_lock);

        if ((index < lov->lov_tgt_size) && (lov->lov_tgts[index] != NULL)) {
                tgt = lov->lov_tgts[index];
                CERROR("UUID %s already assigned at LOV target index %d\n",
                       obd_uuid2str(&tgt->ltd_uuid), index);
                cfs_mutex_up(&lov->lov_lock);
                RETURN(-EEXIST);
        }

        if (index >= lov->lov_tgt_size) {
                /* We need to reallocate the lov target array. */
                struct lov_tgt_desc **newtgts, **old = NULL;
                __u32 newsize, oldsize = 0;

                newsize = max(lov->lov_tgt_size, (__u32)2);
                while (newsize < index + 1)
                        newsize = newsize << 1;
                OBD_ALLOC(newtgts, sizeof(*newtgts) * newsize);
                if (newtgts == NULL) {
                        cfs_mutex_up(&lov->lov_lock);
                        RETURN(-ENOMEM);
                }

                if (lov->lov_tgt_size) {
                        memcpy(newtgts, lov->lov_tgts, sizeof(*newtgts) *
                               lov->lov_tgt_size);
                        old = lov->lov_tgts;
                        oldsize = lov->lov_tgt_size;
                }

                lov->lov_tgts = newtgts;
                lov->lov_tgt_size = newsize;
#ifdef __KERNEL__
                smp_rmb();
#endif
                if (old)
                        OBD_FREE(old, sizeof(*old) * oldsize);

                CDEBUG(D_CONFIG, "tgts: %p size: %d\n",
                       lov->lov_tgts, lov->lov_tgt_size);
        }

        OBD_ALLOC_PTR(tgt);
        if (!tgt) {
                cfs_mutex_up(&lov->lov_lock);
                RETURN(-ENOMEM);
        }

        rc = lov_ost_pool_add(&lov->lov_packed, index, lov->lov_tgt_size);
        if (rc) {
                cfs_mutex_up(&lov->lov_lock);
                OBD_FREE_PTR(tgt);
                RETURN(rc);
        }

        tgt->ltd_uuid = *uuidp;
        tgt->ltd_obd = tgt_obd;
        /* XXX - add a sanity check on the generation number. */
        tgt->ltd_gen = gen;
        tgt->ltd_index = index;
        tgt->ltd_activate = active;
        lov->lov_tgts[index] = tgt;
        if (index >= lov->desc.ld_tgt_count)
                lov->desc.ld_tgt_count = index + 1;

        cfs_mutex_up(&lov->lov_lock);

        CDEBUG(D_CONFIG, "idx=%d ltd_gen=%d ld_tgt_count=%d\n",
                index, tgt->ltd_gen, lov->desc.ld_tgt_count);

        rc = obd_notify(obd, tgt_obd, OBD_NOTIFY_CREATE, &index);

        if (lov->lov_connects == 0) {
                /* lov_connect hasn't been called yet. We'll do the
                   lov_connect_obd on this target when that fn first runs,
                   because we don't know the connect flags yet. */
                RETURN(0);
        }

        obd_getref(obd);

        rc = lov_connect_obd(obd, index, active, &lov->lov_ocd);
        if (rc)
                GOTO(out, rc);

        /* connect to administrative disabled ost */
        if (!tgt->ltd_exp)
                GOTO(out, rc = 0);

        rc = lov_notify(obd, tgt->ltd_exp->exp_obd,
                        active ? OBD_NOTIFY_CONNECT : OBD_NOTIFY_INACTIVE,
                        (void *)&index);

out:
        if (rc) {
                CERROR("add failed (%d), deleting %s\n", rc,
                       obd_uuid2str(&tgt->ltd_uuid));
                lov_del_target(obd, index, 0, 0);
        }
        obd_putref(obd);
        RETURN(rc);
}

/* Schedule a target for deletion */
int lov_del_target(struct obd_device *obd, __u32 index,
                   struct obd_uuid *uuidp, int gen)
{
        struct lov_obd *lov = &obd->u.lov;
        int count = lov->desc.ld_tgt_count;
        int rc = 0;
        ENTRY;

        if (index >= count) {
                CERROR("LOV target index %d >= number of LOV OBDs %d.\n",
                       index, count);
                RETURN(-EINVAL);
        }

        /* to make sure there's no ongoing lov_notify() now */
        cfs_down_write(&lov->lov_notify_lock);
        obd_getref(obd);

        if (!lov->lov_tgts[index]) {
                CERROR("LOV target at index %d is not setup.\n", index);
                GOTO(out, rc = -EINVAL);
        }

        if (uuidp && !obd_uuid_equals(uuidp, &lov->lov_tgts[index]->ltd_uuid)) {
                CERROR("LOV target UUID %s at index %d doesn't match %s.\n",
                       lov_uuid2str(lov, index), index,
                       obd_uuid2str(uuidp));
                GOTO(out, rc = -EINVAL);
        }

        CDEBUG(D_CONFIG, "uuid: %s idx: %d gen: %d exp: %p active: %d\n",
               lov_uuid2str(lov, index), index,
               lov->lov_tgts[index]->ltd_gen, lov->lov_tgts[index]->ltd_exp,
               lov->lov_tgts[index]->ltd_active);

        lov->lov_tgts[index]->ltd_reap = 1;
        lov->lov_death_row++;
        /* we really delete it from obd_putref */
out:
        obd_putref(obd);
        cfs_up_write(&lov->lov_notify_lock);

        RETURN(rc);
}

static void __lov_del_obd(struct obd_device *obd, struct lov_tgt_desc *tgt)
{
        struct obd_device *osc_obd;

        LASSERT(tgt);
        LASSERT(tgt->ltd_reap);

        osc_obd = class_exp2obd(tgt->ltd_exp);

        CDEBUG(D_CONFIG, "Removing tgt %s : %s\n",
               tgt->ltd_uuid.uuid,
               osc_obd ? osc_obd->obd_name : "<no obd>");

        if (tgt->ltd_exp)
                lov_disconnect_obd(obd, tgt);

        OBD_FREE_PTR(tgt);

        /* Manual cleanup - no cleanup logs to clean up the osc's.  We must
           do it ourselves. And we can't do it from lov_cleanup,
           because we just lost our only reference to it. */
        if (osc_obd)
                class_manual_cleanup(osc_obd);
}

void lov_fix_desc_stripe_size(__u64 *val)
{
        if (*val < PTLRPC_MAX_BRW_SIZE) {
                LCONSOLE_WARN("Increasing default stripe size to min %u\n",
                              PTLRPC_MAX_BRW_SIZE);
                *val = PTLRPC_MAX_BRW_SIZE;
        } else if (*val & (LOV_MIN_STRIPE_SIZE - 1)) {
                *val &= ~(LOV_MIN_STRIPE_SIZE - 1);
                LCONSOLE_WARN("Changing default stripe size to "LPU64" (a "
                              "multiple of %u)\n",
                              *val, LOV_MIN_STRIPE_SIZE);
        }
}

void lov_fix_desc_stripe_count(__u32 *val)
{
        if (*val == 0)
                *val = 1;
}

void lov_fix_desc_pattern(__u32 *val)
{
        /* from lov_setstripe */
        if ((*val != 0) && (*val != LOV_PATTERN_RAID0)) {
                LCONSOLE_WARN("Unknown stripe pattern: %#x\n", *val);
                *val = 0;
        }
}

void lov_fix_desc_qos_maxage(__u32 *val)
{
        /* fix qos_maxage */
        if (*val == 0)
                *val = QOS_DEFAULT_MAXAGE;
}

void lov_fix_desc(struct lov_desc *desc)
{
        lov_fix_desc_stripe_size(&desc->ld_default_stripe_size);
        lov_fix_desc_stripe_count(&desc->ld_default_stripe_count);
        lov_fix_desc_pattern(&desc->ld_pattern);
        lov_fix_desc_qos_maxage(&desc->ld_qos_maxage);
}

int lov_setup(struct obd_device *obd, struct lustre_cfg *lcfg)
{
        struct lprocfs_static_vars lvars = { 0 };
        struct lov_desc *desc;
        struct lov_obd *lov = &obd->u.lov;
        int rc;
        ENTRY;

        if (LUSTRE_CFG_BUFLEN(lcfg, 1) < 1) {
                CERROR("LOV setup requires a descriptor\n");
                RETURN(-EINVAL);
        }

        desc = (struct lov_desc *)lustre_cfg_buf(lcfg, 1);

        if (sizeof(*desc) > LUSTRE_CFG_BUFLEN(lcfg, 1)) {
                CERROR("descriptor size wrong: %d > %d\n",
                       (int)sizeof(*desc), LUSTRE_CFG_BUFLEN(lcfg, 1));
                RETURN(-EINVAL);
        }

        if (desc->ld_magic != LOV_DESC_MAGIC) {
                if (desc->ld_magic == __swab32(LOV_DESC_MAGIC)) {
                            CDEBUG(D_OTHER, "%s: Swabbing lov desc %p\n",
                                   obd->obd_name, desc);
                            lustre_swab_lov_desc(desc);
                } else {
                        CERROR("%s: Bad lov desc magic: %#x\n",
                               obd->obd_name, desc->ld_magic);
                        RETURN(-EINVAL);
                }
        }

        lov_fix_desc(desc);

        desc->ld_active_tgt_count = 0;
        lov->desc = *desc;
        lov->lov_tgt_size = 0;

        cfs_sema_init(&lov->lov_lock, 1);
        cfs_atomic_set(&lov->lov_refcount, 0);
        CFS_INIT_LIST_HEAD(&lov->lov_qos.lq_oss_list);
        cfs_init_rwsem(&lov->lov_qos.lq_rw_sem);
        lov->lov_sp_me = LUSTRE_SP_CLI;
        lov->lov_qos.lq_dirty = 1;
        lov->lov_qos.lq_rr.lqr_dirty = 1;
        lov->lov_qos.lq_reset = 1;
        /* Default priority is toward free space balance */
        lov->lov_qos.lq_prio_free = 232;
        /* Default threshold for rr (roughly 17%) */
        lov->lov_qos.lq_threshold_rr = 43;
        /* Init statfs fields */
        OBD_ALLOC_PTR(lov->lov_qos.lq_statfs_data);
        if (NULL == lov->lov_qos.lq_statfs_data)
                RETURN(-ENOMEM);
        cfs_waitq_init(&lov->lov_qos.lq_statfs_waitq);

        cfs_init_rwsem(&lov->lov_notify_lock);

        lov->lov_pools_hash_body = cfs_hash_create("POOLS", HASH_POOLS_CUR_BITS,
                                                   HASH_POOLS_MAX_BITS,
                                                   HASH_POOLS_BKT_BITS, 0,
                                                   CFS_HASH_MIN_THETA,
                                                   CFS_HASH_MAX_THETA,
                                                   &pool_hash_operations,
                                                   CFS_HASH_DEFAULT);
        CFS_INIT_LIST_HEAD(&lov->lov_pool_list);
        lov->lov_pool_count = 0;
        rc = lov_ost_pool_init(&lov->lov_packed, 0);
        if (rc)
                GOTO(out_free_statfs, rc);
        rc = lov_ost_pool_init(&lov->lov_qos.lq_rr.lqr_pool, 0);
        if (rc)
                GOTO(out_free_lov_packed, rc);

        lprocfs_lov_init_vars(&lvars);
        lprocfs_obd_setup(obd, lvars.obd_vars);
#ifdef LPROCFS
        {
                int rc;

                rc = lprocfs_seq_create(obd->obd_proc_entry, "target_obd",
                                        0444, &lov_proc_target_fops, obd);
                if (rc)
                        CWARN("Error adding the target_obd file\n");
        }
#endif
        lov->lov_pool_proc_entry = lprocfs_register("pools",
                                                    obd->obd_proc_entry,
                                                    NULL, NULL);

        RETURN(0);

out_free_lov_packed:
        lov_ost_pool_free(&lov->lov_packed);
out_free_statfs:
        OBD_FREE_PTR(lov->lov_qos.lq_statfs_data);
        return rc;
}

static int lov_precleanup(struct obd_device *obd, enum obd_cleanup_stage stage)
{
        int rc = 0;
        struct lov_obd *lov = &obd->u.lov;

        ENTRY;

        switch (stage) {
        case OBD_CLEANUP_EARLY: {
                int i;
                for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                        if (!lov->lov_tgts[i] || !lov->lov_tgts[i]->ltd_active)
                                continue;
                        obd_precleanup(class_exp2obd(lov->lov_tgts[i]->ltd_exp),
                                       OBD_CLEANUP_EARLY);
                }
                break;
        }
        case OBD_CLEANUP_EXPORTS:
                lprocfs_obd_cleanup(obd);
                rc = obd_llog_finish(obd, 0);
                if (rc != 0)
                        CERROR("failed to cleanup llogging subsystems\n");
                break;
        }
        RETURN(rc);
}

static int lov_cleanup(struct obd_device *obd)
{
        struct lov_obd *lov = &obd->u.lov;
        cfs_list_t *pos, *tmp;
        struct pool_desc *pool;
        ENTRY;

        cfs_list_for_each_safe(pos, tmp, &lov->lov_pool_list) {
                pool = cfs_list_entry(pos, struct pool_desc, pool_list);
                /* free pool structs */
                CDEBUG(D_INFO, "delete pool %p\n", pool);
                lov_pool_del(obd, pool->pool_name);
        }
        cfs_hash_putref(lov->lov_pools_hash_body);
        lov_ost_pool_free(&(lov->lov_qos.lq_rr.lqr_pool));
        lov_ost_pool_free(&lov->lov_packed);

        if (lov->lov_tgts) {
                int i;
                obd_getref(obd);
                for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                        if (!lov->lov_tgts[i])
                                continue;

                        /* Inactive targets may never have connected */
                        if (lov->lov_tgts[i]->ltd_active ||
                            cfs_atomic_read(&lov->lov_refcount))
                            /* We should never get here - these
                               should have been removed in the
                             disconnect. */
                                CERROR("lov tgt %d not cleaned!"
                                       " deathrow=%d, lovrc=%d\n",
                                       i, lov->lov_death_row,
                                       cfs_atomic_read(&lov->lov_refcount));
                        lov_del_target(obd, i, 0, 0);
                }
                obd_putref(obd);
                OBD_FREE(lov->lov_tgts, sizeof(*lov->lov_tgts) *
                         lov->lov_tgt_size);
                lov->lov_tgt_size = 0;
        }
        OBD_FREE_PTR(lov->lov_qos.lq_statfs_data);
        RETURN(0);
}

int lov_process_config_base(struct obd_device *obd, struct lustre_cfg *lcfg,
                            __u32 *indexp, int *genp)
{
        struct obd_uuid obd_uuid;
        int cmd;
        int rc = 0;
        ENTRY;

        switch(cmd = lcfg->lcfg_command) {
        case LCFG_LOV_ADD_OBD:
        case LCFG_LOV_ADD_INA:
        case LCFG_LOV_DEL_OBD: {
                __u32 index;
                int gen;
                /* lov_modify_tgts add  0:lov_mdsA  1:ost1_UUID  2:0  3:1 */
                if (LUSTRE_CFG_BUFLEN(lcfg, 1) > sizeof(obd_uuid.uuid))
                        GOTO(out, rc = -EINVAL);

                obd_str2uuid(&obd_uuid,  lustre_cfg_buf(lcfg, 1));

                if (sscanf(lustre_cfg_buf(lcfg, 2), "%d", indexp) != 1)
                        GOTO(out, rc = -EINVAL);
                if (sscanf(lustre_cfg_buf(lcfg, 3), "%d", genp) != 1)
                        GOTO(out, rc = -EINVAL);
                index = *indexp;
                gen = *genp;
                if (cmd == LCFG_LOV_ADD_OBD)
                        rc = lov_add_target(obd, &obd_uuid, index, gen, 1);
                else if (cmd == LCFG_LOV_ADD_INA)
                        rc = lov_add_target(obd, &obd_uuid, index, gen, 0);
                else
                        rc = lov_del_target(obd, index, &obd_uuid, gen);
                GOTO(out, rc);
        }
        case LCFG_PARAM: {
                struct lprocfs_static_vars lvars = { 0 };
                struct lov_desc *desc = &(obd->u.lov.desc);

                if (!desc)
                        GOTO(out, rc = -EINVAL);

                lprocfs_lov_init_vars(&lvars);

                rc = class_process_proc_param(PARAM_LOV, lvars.obd_vars,
                                              lcfg, obd);
                if (rc > 0)
                        rc = 0;
                GOTO(out, rc);
        }
        case LCFG_POOL_NEW:
        case LCFG_POOL_ADD:
        case LCFG_POOL_DEL:
        case LCFG_POOL_REM:
                GOTO(out, rc);

        default: {
                CERROR("Unknown command: %d\n", lcfg->lcfg_command);
                GOTO(out, rc = -EINVAL);

        }
        }
out:
        RETURN(rc);
}

#ifndef log2
#define log2(n) cfs_ffz(~(n))
#endif

static int lov_clear_orphans(struct obd_export *export, struct obdo *src_oa,
                             struct lov_stripe_md **ea,
                             struct obd_trans_info *oti)
{
        struct lov_obd *lov;
        struct obdo *tmp_oa;
        struct obd_uuid *ost_uuid = NULL;
        int rc = 0, i;
        ENTRY;

        LASSERT(src_oa->o_valid & OBD_MD_FLFLAGS &&
                src_oa->o_flags == OBD_FL_DELORPHAN);

        lov = &export->exp_obd->u.lov;

        OBDO_ALLOC(tmp_oa);
        if (tmp_oa == NULL)
                RETURN(-ENOMEM);

        if (oti->oti_ost_uuid) {
                ost_uuid = oti->oti_ost_uuid;
                CDEBUG(D_HA, "clearing orphans only for %s\n",
                       ost_uuid->uuid);
        }

        obd_getref(export->exp_obd);
        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                struct lov_stripe_md obj_md;
                struct lov_stripe_md *obj_mdp = &obj_md;
                struct lov_tgt_desc *tgt;
                int err;

                tgt = lov->lov_tgts[i];
                if (!tgt)
                        continue;

                /* if called for a specific target, we don't
                   care if it is not active. */
                if (!lov->lov_tgts[i]->ltd_active && ost_uuid == NULL) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", i);
                        continue;
                }

                if (ost_uuid && !obd_uuid_equals(ost_uuid, &tgt->ltd_uuid))
                        continue;

                CDEBUG(D_CONFIG,"Clear orphans for %d:%s\n", i,
                       obd_uuid2str(ost_uuid));

                memcpy(tmp_oa, src_oa, sizeof(*tmp_oa));

                LASSERT(lov->lov_tgts[i]->ltd_exp);
                /* XXX: LOV STACKING: use real "obj_mdp" sub-data */
                err = obd_create(lov->lov_tgts[i]->ltd_exp,
                                 tmp_oa, &obj_mdp, oti);
                if (err) {
                        /* This export will be disabled until it is recovered,
                           and then orphan recovery will be completed. */
                        CERROR("error in orphan recovery on OST idx %d/%d: "
                               "rc = %d\n", i, lov->desc.ld_tgt_count, err);
                        rc = err;
                }

                if (ost_uuid)
                        break;
        }
        obd_putref(export->exp_obd);

        OBDO_FREE(tmp_oa);
        RETURN(rc);
}

static int lov_recreate(struct obd_export *exp, struct obdo *src_oa,
                        struct lov_stripe_md **ea, struct obd_trans_info *oti)
{
        struct lov_stripe_md *obj_mdp, *lsm;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        unsigned ost_idx;
        int rc, i;
        ENTRY;

        LASSERT(src_oa->o_valid & OBD_MD_FLFLAGS &&
                src_oa->o_flags & OBD_FL_RECREATE_OBJS);

        OBD_ALLOC(obj_mdp, sizeof(*obj_mdp));
        if (obj_mdp == NULL)
                RETURN(-ENOMEM);

        ost_idx = src_oa->o_nlink;
        lsm = *ea;
        if (lsm == NULL)
                GOTO(out, rc = -EINVAL);
        if (ost_idx >= lov->desc.ld_tgt_count ||
            !lov->lov_tgts[ost_idx])
                GOTO(out, rc = -EINVAL);

        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                if (lsm->lsm_oinfo[i]->loi_ost_idx == ost_idx) {
                        if (lsm->lsm_oinfo[i]->loi_id != src_oa->o_id)
                                GOTO(out, rc = -EINVAL);
                        break;
                }
        }
        if (i == lsm->lsm_stripe_count)
                GOTO(out, rc = -EINVAL);

        rc = obd_create(lov->lov_tgts[ost_idx]->ltd_exp, src_oa, &obj_mdp, oti);
out:
        OBD_FREE(obj_mdp, sizeof(*obj_mdp));
        RETURN(rc);
}

/* the LOV expects oa->o_id to be set to the LOV object id */
static int lov_create(struct obd_export *exp, struct obdo *src_oa,
                      struct lov_stripe_md **ea, struct obd_trans_info *oti)
{
        struct lov_obd *lov;
        struct obd_info oinfo;
        struct lov_request_set *set = NULL;
        struct lov_request *req;
        struct l_wait_info  lwi = { 0 };
        int rc = 0;
        ENTRY;

        LASSERT(ea != NULL);
        if (exp == NULL)
                RETURN(-EINVAL);

        if ((src_oa->o_valid & OBD_MD_FLFLAGS) &&
            src_oa->o_flags == OBD_FL_DELORPHAN) {
                rc = lov_clear_orphans(exp, src_oa, ea, oti);
                RETURN(rc);
        }

        lov = &exp->exp_obd->u.lov;
        if (!lov->desc.ld_active_tgt_count)
                RETURN(-EIO);

        obd_getref(exp->exp_obd);
        /* Recreate a specific object id at the given OST index */
        if ((src_oa->o_valid & OBD_MD_FLFLAGS) &&
            (src_oa->o_flags & OBD_FL_RECREATE_OBJS)) {
                 rc = lov_recreate(exp, src_oa, ea, oti);
                 GOTO(out, rc);
        }

        /* issue statfs rpcs if the osfs data is older than qos_maxage - 1s,
         * later in alloc_qos(), we will wait for those rpcs to complete if
         * the osfs age is older than 2 * qos_maxage */
        qos_statfs_update(exp->exp_obd,
                          cfs_time_shift_64(-lov->desc.ld_qos_maxage +
                                            OBD_STATFS_CACHE_SECONDS),
                          0);

        rc = lov_prep_create_set(exp, &oinfo, ea, src_oa, oti, &set);
        if (rc)
                GOTO(out, rc);

        cfs_list_for_each_entry(req, &set->set_list, rq_link) {
                /* XXX: LOV STACKING: use real "obj_mdp" sub-data */
                rc = obd_create_async(lov->lov_tgts[req->rq_idx]->ltd_exp,
                                      &req->rq_oi, &req->rq_oi.oi_md, oti);
        }

        /* osc_create have timeout equ obd_timeout/2 so waiting don't be
         * longer then this */
        l_wait_event(set->set_waitq, lov_set_finished(set, 1), &lwi);

        /* we not have ptlrpc set for assign set->interpret and should
         * be call interpret function himself. calling from cb_create_update
         * not permited because lov_fini_create_set can sleep for long time,
         * but we must avoid sleeping in ptlrpcd interpret function. */
        rc = lov_fini_create_set(set, ea);
out:
        obd_putref(exp->exp_obd);
        RETURN(rc);
}

#define ASSERT_LSM_MAGIC(lsmp)                                                  \
do {                                                                            \
        LASSERT((lsmp) != NULL);                                                \
        LASSERTF(((lsmp)->lsm_magic == LOV_MAGIC_V1 ||                          \
                 (lsmp)->lsm_magic == LOV_MAGIC_V3),                            \
                 "%p->lsm_magic=%x\n", (lsmp), (lsmp)->lsm_magic);              \
} while (0)

static int lov_destroy(struct obd_export *exp, struct obdo *oa,
                       struct lov_stripe_md *lsm, struct obd_trans_info *oti,
                       struct obd_export *md_exp, void *capa)
{
        struct lov_request_set *set;
        struct obd_info oinfo;
        struct lov_request *req;
        cfs_list_t *pos;
        struct lov_obd *lov;
        int rc = 0, err = 0;
        ENTRY;

        ASSERT_LSM_MAGIC(lsm);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        if (oa->o_valid & OBD_MD_FLCOOKIE) {
                LASSERT(oti);
                LASSERT(oti->oti_logcookies);
        }

        lov = &exp->exp_obd->u.lov;
        obd_getref(exp->exp_obd);
        rc = lov_prep_destroy_set(exp, &oinfo, oa, lsm, oti, &set);
        if (rc)
                GOTO(out, rc);

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                if (oa->o_valid & OBD_MD_FLCOOKIE)
                        oti->oti_logcookies = set->set_cookies + req->rq_stripe;

                err = obd_destroy(lov->lov_tgts[req->rq_idx]->ltd_exp,
                                  req->rq_oi.oi_oa, NULL, oti, NULL, capa);
                err = lov_update_common_set(set, req, err);
                if (err) {
                        CERROR("error: destroying objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               oa->o_id, req->rq_oi.oi_oa->o_id,
                               req->rq_idx, err);
                        if (!rc)
                                rc = err;
                }
        }

        if (rc == 0) {
                LASSERT(lsm_op_find(lsm->lsm_magic) != NULL);
                rc = lsm_op_find(lsm->lsm_magic)->lsm_destroy(lsm, oa, md_exp);
        }
        err = lov_fini_destroy_set(set);
out:
        obd_putref(exp->exp_obd);
        RETURN(rc ? rc : err);
}

static int lov_getattr(struct obd_export *exp, struct obd_info *oinfo)
{
        struct lov_request_set *set;
        struct lov_request *req;
        cfs_list_t *pos;
        struct lov_obd *lov;
        int err = 0, rc = 0;
        ENTRY;

        LASSERT(oinfo);
        ASSERT_LSM_MAGIC(oinfo->oi_md);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;

        rc = lov_prep_getattr_set(exp, oinfo, &set);
        if (rc)
                RETURN(rc);

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                CDEBUG(D_INFO, "objid "LPX64"[%d] has subobj "LPX64" at idx "
                       "%u\n", oinfo->oi_oa->o_id, req->rq_stripe,
                       req->rq_oi.oi_oa->o_id, req->rq_idx);

                rc = obd_getattr(lov->lov_tgts[req->rq_idx]->ltd_exp,
                                 &req->rq_oi);
                err = lov_update_common_set(set, req, rc);
                if (err) {
                        CERROR("error: getattr objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               oinfo->oi_oa->o_id, req->rq_oi.oi_oa->o_id,
                               req->rq_idx, err);
                        break;
                }
        }

        rc = lov_fini_getattr_set(set);
        if (err)
                rc = err;
        RETURN(rc);
}

static int lov_getattr_interpret(struct ptlrpc_request_set *rqset,
                                 void *data, int rc)
{
        struct lov_request_set *lovset = (struct lov_request_set *)data;
        int err;
        ENTRY;

        /* don't do attribute merge if this aysnc op failed */
        if (rc)
                lovset->set_completes = 0;
        err = lov_fini_getattr_set(lovset);
        RETURN(rc ? rc : err);
}

static int lov_getattr_async(struct obd_export *exp, struct obd_info *oinfo,
                              struct ptlrpc_request_set *rqset)
{
        struct lov_request_set *lovset;
        struct lov_obd *lov;
        cfs_list_t *pos;
        struct lov_request *req;
        int rc = 0, err;
        ENTRY;

        LASSERT(oinfo);
        ASSERT_LSM_MAGIC(oinfo->oi_md);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;

        rc = lov_prep_getattr_set(exp, oinfo, &lovset);
        if (rc)
                RETURN(rc);

        CDEBUG(D_INFO, "objid "LPX64": %ux%u byte stripes\n",
               oinfo->oi_md->lsm_object_id, oinfo->oi_md->lsm_stripe_count,
               oinfo->oi_md->lsm_stripe_size);

        cfs_list_for_each (pos, &lovset->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                CDEBUG(D_INFO, "objid "LPX64"[%d] has subobj "LPX64" at idx "
                       "%u\n", oinfo->oi_oa->o_id, req->rq_stripe,
                       req->rq_oi.oi_oa->o_id, req->rq_idx);
                rc = obd_getattr_async(lov->lov_tgts[req->rq_idx]->ltd_exp,
                                       &req->rq_oi, rqset);
                if (rc) {
                        CERROR("error: getattr objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               oinfo->oi_oa->o_id, req->rq_oi.oi_oa->o_id,
                               req->rq_idx, rc);
                        GOTO(out, rc);
                }
        }

        if (!cfs_list_empty(&rqset->set_requests)) {
                LASSERT(rc == 0);
                LASSERT (rqset->set_interpret == NULL);
                rqset->set_interpret = lov_getattr_interpret;
                rqset->set_arg = (void *)lovset;
                RETURN(rc);
        }
out:
        if (rc)
                lovset->set_completes = 0;
        err = lov_fini_getattr_set(lovset);
        RETURN(rc ? rc : err);
}

static int lov_setattr(struct obd_export *exp, struct obd_info *oinfo,
                       struct obd_trans_info *oti)
{
        struct lov_request_set *set;
        struct lov_obd *lov;
        cfs_list_t *pos;
        struct lov_request *req;
        int err = 0, rc = 0;
        ENTRY;

        LASSERT(oinfo);
        ASSERT_LSM_MAGIC(oinfo->oi_md);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        /* for now, we only expect the following updates here */
        LASSERT(!(oinfo->oi_oa->o_valid & ~(OBD_MD_FLID | OBD_MD_FLTYPE |
                                            OBD_MD_FLMODE | OBD_MD_FLATIME |
                                            OBD_MD_FLMTIME | OBD_MD_FLCTIME |
                                            OBD_MD_FLFLAGS | OBD_MD_FLSIZE |
                                            OBD_MD_FLGROUP | OBD_MD_FLUID |
                                            OBD_MD_FLGID | OBD_MD_FLFID |
                                            OBD_MD_FLGENER)));
        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_setattr_set(exp, oinfo, oti, &set);
        if (rc)
                RETURN(rc);

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                rc = obd_setattr(lov->lov_tgts[req->rq_idx]->ltd_exp,
                                 &req->rq_oi, NULL);
                err = lov_update_setattr_set(set, req, rc);
                if (err) {
                        CERROR("error: setattr objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               set->set_oi->oi_oa->o_id,
                               req->rq_oi.oi_oa->o_id, req->rq_idx, err);
                        if (!rc)
                                rc = err;
                }
        }
        err = lov_fini_setattr_set(set);
        if (!rc)
                rc = err;
        RETURN(rc);
}

static int lov_setattr_interpret(struct ptlrpc_request_set *rqset,
                                 void *data, int rc)
{
        struct lov_request_set *lovset = (struct lov_request_set *)data;
        int err;
        ENTRY;

        if (rc)
                lovset->set_completes = 0;
        err = lov_fini_setattr_set(lovset);
        RETURN(rc ? rc : err);
}

/* If @oti is given, the request goes from MDS and responses from OSTs are not
   needed. Otherwise, a client is waiting for responses. */
static int lov_setattr_async(struct obd_export *exp, struct obd_info *oinfo,
                             struct obd_trans_info *oti,
                             struct ptlrpc_request_set *rqset)
{
        struct lov_request_set *set;
        struct lov_request *req;
        cfs_list_t *pos;
        struct lov_obd *lov;
        int rc = 0;
        ENTRY;

        LASSERT(oinfo);
        ASSERT_LSM_MAGIC(oinfo->oi_md);
        if (oinfo->oi_oa->o_valid & OBD_MD_FLCOOKIE) {
                LASSERT(oti);
                LASSERT(oti->oti_logcookies);
        }

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_setattr_set(exp, oinfo, oti, &set);
        if (rc)
                RETURN(rc);

        CDEBUG(D_INFO, "objid "LPX64": %ux%u byte stripes\n",
               oinfo->oi_md->lsm_object_id, oinfo->oi_md->lsm_stripe_count,
               oinfo->oi_md->lsm_stripe_size);

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                if (oinfo->oi_oa->o_valid & OBD_MD_FLCOOKIE)
                        oti->oti_logcookies = set->set_cookies + req->rq_stripe;

                CDEBUG(D_INFO, "objid "LPX64"[%d] has subobj "LPX64" at idx "
                       "%u\n", oinfo->oi_oa->o_id, req->rq_stripe,
                       req->rq_oi.oi_oa->o_id, req->rq_idx);

                rc = obd_setattr_async(lov->lov_tgts[req->rq_idx]->ltd_exp,
                                       &req->rq_oi, oti, rqset);
                if (rc) {
                        CERROR("error: setattr objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               set->set_oi->oi_oa->o_id,
                               req->rq_oi.oi_oa->o_id,
                               req->rq_idx, rc);
                        break;
                }
        }

        /* If we are not waiting for responses on async requests, return. */
        if (rc || !rqset || cfs_list_empty(&rqset->set_requests)) {
                int err;
                if (rc)
                        set->set_completes = 0;
                err = lov_fini_setattr_set(set);
                RETURN(rc ? rc : err);
        }

        LASSERT(rqset->set_interpret == NULL);
        rqset->set_interpret = lov_setattr_interpret;
        rqset->set_arg = (void *)set;

        RETURN(0);
}

static int lov_punch_interpret(struct ptlrpc_request_set *rqset,
                               void *data, int rc)
{
        struct lov_request_set *lovset = (struct lov_request_set *)data;
        int err;
        ENTRY;

        if (rc)
                lovset->set_completes = 0;
        err = lov_fini_punch_set(lovset);
        RETURN(rc ? rc : err);
}

/* FIXME: maybe we'll just make one node the authoritative attribute node, then
 * we can send this 'punch' to just the authoritative node and the nodes
 * that the punch will affect. */
static int lov_punch(struct obd_export *exp, struct obd_info *oinfo,
                     struct obd_trans_info *oti,
                     struct ptlrpc_request_set *rqset)
{
        struct lov_request_set *set;
        struct lov_obd *lov;
        cfs_list_t *pos;
        struct lov_request *req;
        int rc = 0;
        ENTRY;

        LASSERT(oinfo);
        ASSERT_LSM_MAGIC(oinfo->oi_md);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_punch_set(exp, oinfo, oti, &set);
        if (rc)
                RETURN(rc);

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                rc = obd_punch(lov->lov_tgts[req->rq_idx]->ltd_exp,
                               &req->rq_oi, NULL, rqset);
                if (rc) {
                        CERROR("error: punch objid "LPX64" subobj "LPX64
                               " on OST idx %d: rc = %d\n",
                               set->set_oi->oi_oa->o_id,
                               req->rq_oi.oi_oa->o_id, req->rq_idx, rc);
                        break;
                }
        }

        if (rc || cfs_list_empty(&rqset->set_requests)) {
                int err;
                err = lov_fini_punch_set(set);
                RETURN(rc ? rc : err);
        }

        LASSERT(rqset->set_interpret == NULL);
        rqset->set_interpret = lov_punch_interpret;
        rqset->set_arg = (void *)set;

        RETURN(0);
}

static int lov_sync_interpret(struct ptlrpc_request_set *rqset,
                              void *data, int rc)
{
        struct lov_request_set *lovset = data;
        int err;
        ENTRY;

        if (rc)
                lovset->set_completes = 0;
        err = lov_fini_sync_set(lovset);
        RETURN(rc ?: err);
}

static int lov_sync(struct obd_export *exp, struct obd_info *oinfo,
                    obd_off start, obd_off end,
                    struct ptlrpc_request_set *rqset)
{
        struct lov_request_set *set = NULL;
        struct lov_obd *lov;
        cfs_list_t *pos;
        struct lov_request *req;
        int rc = 0;
        ENTRY;

        ASSERT_LSM_MAGIC(oinfo->oi_md);
        LASSERT(rqset != NULL);

        if (!exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_sync_set(exp, oinfo, start, end, &set);
        if (rc)
                RETURN(rc);

        CDEBUG(D_INFO, "fsync objid "LPX64" ["LPX64", "LPX64"]\n",
               set->set_oi->oi_oa->o_id, start, end);

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                rc = obd_sync(lov->lov_tgts[req->rq_idx]->ltd_exp, &req->rq_oi,
                              req->rq_oi.oi_policy.l_extent.start,
                              req->rq_oi.oi_policy.l_extent.end, rqset);
                if (rc) {
                        CERROR("error: fsync objid "LPX64" subobj "LPX64
                               " on OST idx %d: rc = %d\n",
                               set->set_oi->oi_oa->o_id,
                               req->rq_oi.oi_oa->o_id, req->rq_idx, rc);
                        break;
                }
        }

        /* If we are not waiting for responses on async requests, return. */
        if (rc || cfs_list_empty(&rqset->set_requests)) {
                int err = lov_fini_sync_set(set);

                RETURN(rc ?: err);
        }

        LASSERT(rqset->set_interpret == NULL);
        rqset->set_interpret = lov_sync_interpret;
        rqset->set_arg = (void *)set;

        RETURN(0);
}

static int lov_brw_check(struct lov_obd *lov, struct obd_info *lov_oinfo,
                         obd_count oa_bufs, struct brw_page *pga)
{
        struct obd_info oinfo = { { { 0 } } };
        int i, rc = 0;

        oinfo.oi_oa = lov_oinfo->oi_oa;

        /* The caller just wants to know if there's a chance that this
         * I/O can succeed */
        for (i = 0; i < oa_bufs; i++) {
                int stripe = lov_stripe_number(lov_oinfo->oi_md, pga[i].off);
                int ost = lov_oinfo->oi_md->lsm_oinfo[stripe]->loi_ost_idx;
                obd_off start, end;

                if (!lov_stripe_intersects(lov_oinfo->oi_md, i, pga[i].off,
                                           pga[i].off + pga[i].count - 1,
                                           &start, &end))
                        continue;

                if (!lov->lov_tgts[ost] || !lov->lov_tgts[ost]->ltd_active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", ost);
                        return -EIO;
                }

                rc = obd_brw(OBD_BRW_CHECK, lov->lov_tgts[ost]->ltd_exp, &oinfo,
                             1, &pga[i], NULL);
                if (rc)
                        break;
        }
        return rc;
}

static int lov_brw(int cmd, struct obd_export *exp, struct obd_info *oinfo,
                   obd_count oa_bufs, struct brw_page *pga,
                   struct obd_trans_info *oti)
{
        struct lov_request_set *set;
        struct lov_request *req;
        cfs_list_t *pos;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int err, rc = 0;
        ENTRY;

        ASSERT_LSM_MAGIC(oinfo->oi_md);

        if (cmd == OBD_BRW_CHECK) {
                rc = lov_brw_check(lov, oinfo, oa_bufs, pga);
                RETURN(rc);
        }

        rc = lov_prep_brw_set(exp, oinfo, oa_bufs, pga, oti, &set);
        if (rc)
                RETURN(rc);

        cfs_list_for_each (pos, &set->set_list) {
                struct obd_export *sub_exp;
                struct brw_page *sub_pga;
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                sub_exp = lov->lov_tgts[req->rq_idx]->ltd_exp;
                sub_pga = set->set_pga + req->rq_pgaidx;
                rc = obd_brw(cmd, sub_exp, &req->rq_oi, req->rq_oabufs,
                             sub_pga, oti);
                if (rc)
                        break;
                lov_update_common_set(set, req, rc);
        }

        err = lov_fini_brw_set(set);
        if (!rc)
                rc = err;
        RETURN(rc);
}

static int lov_enqueue_interpret(struct ptlrpc_request_set *rqset,
                                 void *data, int rc)
{
        struct lov_request_set *lovset = (struct lov_request_set *)data;
        ENTRY;
        rc = lov_fini_enqueue_set(lovset, lovset->set_ei->ei_mode, rc, rqset);
        RETURN(rc);
}

static int lov_enqueue(struct obd_export *exp, struct obd_info *oinfo,
                       struct ldlm_enqueue_info *einfo,
                       struct ptlrpc_request_set *rqset)
{
        ldlm_mode_t mode = einfo->ei_mode;
        struct lov_request_set *set;
        struct lov_request *req;
        cfs_list_t *pos;
        struct lov_obd *lov;
        ldlm_error_t rc;
        ENTRY;

        LASSERT(oinfo);
        ASSERT_LSM_MAGIC(oinfo->oi_md);
        LASSERT(mode == (mode & -mode));

        /* we should never be asked to replay a lock this way. */
        LASSERT((oinfo->oi_flags & LDLM_FL_REPLAY) == 0);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_enqueue_set(exp, oinfo, einfo, &set);
        if (rc)
                RETURN(rc);

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                rc = obd_enqueue(lov->lov_tgts[req->rq_idx]->ltd_exp,
                                 &req->rq_oi, einfo, rqset);
                if (rc != ELDLM_OK)
                        GOTO(out, rc);
        }

        if (rqset && !cfs_list_empty(&rqset->set_requests)) {
                LASSERT(rc == 0);
                LASSERT(rqset->set_interpret == NULL);
                rqset->set_interpret = lov_enqueue_interpret;
                rqset->set_arg = (void *)set;
                RETURN(rc);
        }
out:
        rc = lov_fini_enqueue_set(set, mode, rc, rqset);
        RETURN(rc);
}

static int lov_change_cbdata(struct obd_export *exp,
                             struct lov_stripe_md *lsm, ldlm_iterator_t it,
                             void *data)
{
        struct lov_obd *lov;
        int rc = 0, i;
        ENTRY;

        ASSERT_LSM_MAGIC(lsm);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                struct lov_stripe_md submd;
                struct lov_oinfo *loi = lsm->lsm_oinfo[i];

                if (!lov->lov_tgts[loi->loi_ost_idx]) {
                        CDEBUG(D_HA, "lov idx %d NULL \n", loi->loi_ost_idx);
                        continue;
                }

                submd.lsm_object_id = loi->loi_id;
                submd.lsm_object_seq = loi->loi_seq;
                submd.lsm_stripe_count = 0;
                rc = obd_change_cbdata(lov->lov_tgts[loi->loi_ost_idx]->ltd_exp,
                                       &submd, it, data);
        }
        RETURN(rc);
}

/* find any ldlm lock of the inode in lov
 * return 0    not find
 *        1    find one
 *      < 0    error */
static int lov_find_cbdata(struct obd_export *exp,
                           struct lov_stripe_md *lsm, ldlm_iterator_t it,
                           void *data)
{
        struct lov_obd *lov;
        int rc = 0, i;
        ENTRY;

        ASSERT_LSM_MAGIC(lsm);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                struct lov_stripe_md submd;
                struct lov_oinfo *loi = lsm->lsm_oinfo[i];

                if (!lov->lov_tgts[loi->loi_ost_idx]) {
                        CDEBUG(D_HA, "lov idx %d NULL \n", loi->loi_ost_idx);
                        continue;
                }

                submd.lsm_object_id = loi->loi_id;
                submd.lsm_object_seq = loi->loi_seq;
                submd.lsm_stripe_count = 0;
                rc = obd_find_cbdata(lov->lov_tgts[loi->loi_ost_idx]->ltd_exp,
                                     &submd, it, data);
                if (rc != 0)
                        RETURN(rc);
        }
        RETURN(rc);
}

static int lov_cancel(struct obd_export *exp, struct lov_stripe_md *lsm,
                      __u32 mode, struct lustre_handle *lockh)
{
        struct lov_request_set *set;
        struct obd_info oinfo;
        struct lov_request *req;
        cfs_list_t *pos;
        struct lov_obd *lov;
        struct lustre_handle *lov_lockhp;
        int err = 0, rc = 0;
        ENTRY;

        ASSERT_LSM_MAGIC(lsm);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        LASSERT(lockh);
        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_cancel_set(exp, &oinfo, lsm, mode, lockh, &set);
        if (rc)
                RETURN(rc);

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);
                lov_lockhp = set->set_lockh->llh_handles + req->rq_stripe;

                rc = obd_cancel(lov->lov_tgts[req->rq_idx]->ltd_exp,
                                req->rq_oi.oi_md, mode, lov_lockhp);
                rc = lov_update_common_set(set, req, rc);
                if (rc) {
                        CERROR("error: cancel objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               lsm->lsm_object_id,
                               req->rq_oi.oi_md->lsm_object_id,
                               req->rq_idx, rc);
                        err = rc;
                }

        }
        lov_fini_cancel_set(set);
        RETURN(err);
}

static int lov_cancel_unused(struct obd_export *exp,
                             struct lov_stripe_md *lsm,
                             ldlm_cancel_flags_t flags, void *opaque)
{
        struct lov_obd *lov;
        int rc = 0, i;
        ENTRY;

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        if (lsm == NULL) {
                for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                        int err;
                        if (!lov->lov_tgts[i] || !lov->lov_tgts[i]->ltd_exp)
                                continue;

                        err = obd_cancel_unused(lov->lov_tgts[i]->ltd_exp, NULL,
                                                flags, opaque);
                        if (!rc)
                                rc = err;
                }
                RETURN(rc);
        }

        ASSERT_LSM_MAGIC(lsm);

        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                struct lov_stripe_md submd;
                struct lov_oinfo *loi = lsm->lsm_oinfo[i];
                int err;

                if (!lov->lov_tgts[loi->loi_ost_idx]) {
                        CDEBUG(D_HA, "lov idx %d NULL\n", loi->loi_ost_idx);
                        continue;
                }

                if (!lov->lov_tgts[loi->loi_ost_idx]->ltd_active)
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);

                submd.lsm_object_id = loi->loi_id;
                submd.lsm_object_seq = loi->loi_seq;
                submd.lsm_stripe_count = 0;
                err = obd_cancel_unused(lov->lov_tgts[loi->loi_ost_idx]->ltd_exp,
                                        &submd, flags, opaque);
                if (err && lov->lov_tgts[loi->loi_ost_idx]->ltd_active) {
                        CERROR("error: cancel unused objid "LPX64" subobj "LPX64
                               " on OST idx %d: rc = %d\n", lsm->lsm_object_id,
                               loi->loi_id, loi->loi_ost_idx, err);
                        if (!rc)
                                rc = err;
                }
        }
        RETURN(rc);
}

int lov_statfs_interpret(struct ptlrpc_request_set *rqset, void *data, int rc)
{
        struct lov_request_set *lovset = (struct lov_request_set *)data;
        int err;
        ENTRY;

        if (rc)
                lovset->set_completes = 0;

        err = lov_fini_statfs_set(lovset);
        RETURN(rc ? rc : err);
}

static int lov_statfs_async(struct obd_device *obd, struct obd_info *oinfo,
                            __u64 max_age, struct ptlrpc_request_set *rqset)
{
        struct lov_request_set *set;
        struct lov_request *req;
        cfs_list_t *pos;
        struct lov_obd *lov;
        int rc = 0;
        ENTRY;

        LASSERT(oinfo != NULL);
        LASSERT(oinfo->oi_osfs != NULL);

        lov = &obd->u.lov;
        rc = lov_prep_statfs_set(obd, oinfo, &set);
        if (rc)
                RETURN(rc);

        cfs_list_for_each (pos, &set->set_list) {
                struct obd_device *osc_obd;

                req = cfs_list_entry(pos, struct lov_request, rq_link);

                osc_obd = class_exp2obd(lov->lov_tgts[req->rq_idx]->ltd_exp);
                rc = obd_statfs_async(osc_obd, &req->rq_oi, max_age, rqset);
                if (rc)
                        break;
        }

        if (rc || cfs_list_empty(&rqset->set_requests)) {
                int err;
                if (rc)
                        set->set_completes = 0;
                err = lov_fini_statfs_set(set);
                RETURN(rc ? rc : err);
        }

        LASSERT(rqset->set_interpret == NULL);
        rqset->set_interpret = lov_statfs_interpret;
        rqset->set_arg = (void *)set;
        RETURN(0);
}

static int lov_statfs(struct obd_device *obd, struct obd_statfs *osfs,
                      __u64 max_age, __u32 flags)
{
        struct ptlrpc_request_set *set = NULL;
        struct obd_info oinfo = { { { 0 } } };
        int rc = 0;
        ENTRY;


        /* for obdclass we forbid using obd_statfs_rqset, but prefer using async
         * statfs requests */
        set = ptlrpc_prep_set();
        if (set == NULL)
                RETURN(-ENOMEM);

        oinfo.oi_osfs = osfs;
        oinfo.oi_flags = flags;
        rc = lov_statfs_async(obd, &oinfo, max_age, set);
        if (rc == 0)
                rc = ptlrpc_set_wait(set);
        ptlrpc_set_destroy(set);

        RETURN(rc);
}

static int lov_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                         void *karg, void *uarg)
{
        struct obd_device *obddev = class_exp2obd(exp);
        struct lov_obd *lov = &obddev->u.lov;
        int i = 0, rc = 0, count = lov->desc.ld_tgt_count;
        struct obd_uuid *uuidp;
        ENTRY;

        switch (cmd) {
        case IOC_OBD_STATFS: {
                struct obd_ioctl_data *data = karg;
                struct obd_device *osc_obd;
                struct obd_statfs stat_buf = {0};
                __u32 index;

                memcpy(&index, data->ioc_inlbuf2, sizeof(__u32));
                if ((index >= count))
                        RETURN(-ENODEV);

                if (!lov->lov_tgts[index])
                        /* Try again with the next index */
                        RETURN(-EAGAIN);
                if (!lov->lov_tgts[index]->ltd_active)
                        RETURN(-ENODATA);

                osc_obd = class_exp2obd(lov->lov_tgts[index]->ltd_exp);
                if (!osc_obd)
                        RETURN(-EINVAL);

                /* copy UUID */
                if (cfs_copy_to_user(data->ioc_pbuf2, obd2cli_tgt(osc_obd),
                                     min((int) data->ioc_plen2,
                                         (int) sizeof(struct obd_uuid))))
                        RETURN(-EFAULT);

                /* got statfs data */
                rc = obd_statfs(osc_obd, &stat_buf,
                                cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS),
                                0);
                if (rc)
                        RETURN(rc);
                if (cfs_copy_to_user(data->ioc_pbuf1, &stat_buf,
                                     min((int) data->ioc_plen1,
                                         (int) sizeof(stat_buf))))
                        RETURN(-EFAULT);
                break;
        }
        case OBD_IOC_LOV_GET_CONFIG: {
                struct obd_ioctl_data *data;
                struct lov_desc *desc;
                char *buf = NULL;
                __u32 *genp;

                len = 0;
                if (obd_ioctl_getdata(&buf, &len, (void *)uarg))
                        RETURN(-EINVAL);

                data = (struct obd_ioctl_data *)buf;

                if (sizeof(*desc) > data->ioc_inllen1) {
                        obd_ioctl_freedata(buf, len);
                        RETURN(-EINVAL);
                }

                if (sizeof(uuidp->uuid) * count > data->ioc_inllen2) {
                        obd_ioctl_freedata(buf, len);
                        RETURN(-EINVAL);
                }

                if (sizeof(__u32) * count > data->ioc_inllen3) {
                        obd_ioctl_freedata(buf, len);
                        RETURN(-EINVAL);
                }

                desc = (struct lov_desc *)data->ioc_inlbuf1;
                memcpy(desc, &(lov->desc), sizeof(*desc));

                uuidp = (struct obd_uuid *)data->ioc_inlbuf2;
                genp = (__u32 *)data->ioc_inlbuf3;
                /* the uuid will be empty for deleted OSTs */
                for (i = 0; i < count; i++, uuidp++, genp++) {
                        if (!lov->lov_tgts[i])
                                continue;
                        *uuidp = lov->lov_tgts[i]->ltd_uuid;
                        *genp = lov->lov_tgts[i]->ltd_gen;
                }

                if (cfs_copy_to_user((void *)uarg, buf, len))
                        rc = -EFAULT;
                obd_ioctl_freedata(buf, len);
                break;
        }
        case LL_IOC_LOV_SETSTRIPE:
                rc = lov_setstripe(exp, len, karg, uarg);
                break;
        case LL_IOC_LOV_GETSTRIPE:
                rc = lov_getstripe(exp, karg, uarg);
                break;
        case LL_IOC_LOV_SETEA:
                rc = lov_setea(exp, karg, uarg);
                break;
        case OBD_IOC_QUOTACTL: {
                struct if_quotactl *qctl = karg;
                struct lov_tgt_desc *tgt = NULL;
                struct obd_quotactl *oqctl;

                if (qctl->qc_valid == QC_OSTIDX) {
                        if (qctl->qc_idx < 0 || count <= qctl->qc_idx)
                                RETURN(-EINVAL);

                        tgt = lov->lov_tgts[qctl->qc_idx];
                        if (!tgt || !tgt->ltd_exp)
                                RETURN(-EINVAL);
                } else if (qctl->qc_valid == QC_UUID) {
                        for (i = 0; i < count; i++) {
                                tgt = lov->lov_tgts[i];
                                if (!tgt ||
                                    !obd_uuid_equals(&tgt->ltd_uuid,
                                                     &qctl->obd_uuid))
                                        continue;

                                if (tgt->ltd_exp == NULL)
                                        RETURN(-EINVAL);

                                break;
                        }
                } else {
                        RETURN(-EINVAL);
                }

                if (i >= count)
                        RETURN(-EAGAIN);

                LASSERT(tgt && tgt->ltd_exp);
                OBD_ALLOC_PTR(oqctl);
                if (!oqctl)
                        RETURN(-ENOMEM);

                QCTL_COPY(oqctl, qctl);
                rc = obd_quotactl(tgt->ltd_exp, oqctl);
                if (rc == 0) {
                        QCTL_COPY(qctl, oqctl);
                        qctl->qc_valid = QC_OSTIDX;
                        qctl->obd_uuid = tgt->ltd_uuid;
                }
                OBD_FREE_PTR(oqctl);
                break;
        }
        default: {
                int set = 0;

                if (count == 0)
                        RETURN(-ENOTTY);

                for (i = 0; i < count; i++) {
                        int err;
                        struct obd_device *osc_obd;

                        /* OST was disconnected */
                        if (!lov->lov_tgts[i] || !lov->lov_tgts[i]->ltd_exp)
                                continue;

                        /* ll_umount_begin() sets force flag but for lov, not
                         * osc. Let's pass it through */
                        osc_obd = class_exp2obd(lov->lov_tgts[i]->ltd_exp);
                        osc_obd->obd_force = obddev->obd_force;
                        err = obd_iocontrol(cmd, lov->lov_tgts[i]->ltd_exp,
                                            len, karg, uarg);
                        if (err == -ENODATA && cmd == OBD_IOC_POLL_QUOTACHECK) {
                                RETURN(err);
                        } else if (err) {
                                if (lov->lov_tgts[i]->ltd_active) {
                                        CDEBUG(err == -ENOTTY ?
                                               D_IOCTL : D_WARNING,
                                               "iocontrol OSC %s on OST "
                                               "idx %d cmd %x: err = %d\n",
                                               lov_uuid2str(lov, i),
                                               i, cmd, err);
                                        if (!rc)
                                                rc = err;
                                }
                        } else {
                                set = 1;
                        }
                }
                if (!set && !rc)
                        rc = -EIO;
        }
        }

        RETURN(rc);
}

#define FIEMAP_BUFFER_SIZE 4096

/**
 * Non-zero fe_logical indicates that this is a continuation FIEMAP
 * call. The local end offset and the device are sent in the first
 * fm_extent. This function calculates the stripe number from the index.
 * This function returns a stripe_no on which mapping is to be restarted.
 *
 * This function returns fm_end_offset which is the in-OST offset at which
 * mapping should be restarted. If fm_end_offset=0 is returned then caller
 * will re-calculate proper offset in next stripe.
 * Note that the first extent is passed to lov_get_info via the value field.
 *
 * \param fiemap fiemap request header
 * \param lsm striping information for the file
 * \param fm_start logical start of mapping
 * \param fm_end logical end of mapping
 * \param start_stripe starting stripe will be returned in this
 */
obd_size fiemap_calc_fm_end_offset(struct ll_user_fiemap *fiemap,
                                   struct lov_stripe_md *lsm, obd_size fm_start,
                                   obd_size fm_end, int *start_stripe)
{
        obd_size local_end = fiemap->fm_extents[0].fe_logical;
        obd_off lun_start, lun_end;
        obd_size fm_end_offset;
        int stripe_no = -1, i;

        if (fiemap->fm_extent_count == 0 ||
            fiemap->fm_extents[0].fe_logical == 0)
                return 0;

        /* Find out stripe_no from ost_index saved in the fe_device */
        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                if (lsm->lsm_oinfo[i]->loi_ost_idx ==
                                        fiemap->fm_extents[0].fe_device) {
                        stripe_no = i;
                        break;
                }
        }
	if (stripe_no == -1)
		return -EINVAL;

        /* If we have finished mapping on previous device, shift logical
         * offset to start of next device */
        if ((lov_stripe_intersects(lsm, stripe_no, fm_start, fm_end,
                                   &lun_start, &lun_end)) != 0 &&
                                   local_end < lun_end) {
                fm_end_offset = local_end;
                *start_stripe = stripe_no;
        } else {
                /* This is a special value to indicate that caller should
                 * calculate offset in next stripe. */
                fm_end_offset = 0;
                *start_stripe = (stripe_no + 1) % lsm->lsm_stripe_count;
        }

        return fm_end_offset;
}

/**
 * We calculate on which OST the mapping will end. If the length of mapping
 * is greater than (stripe_size * stripe_count) then the last_stripe will
 * will be one just before start_stripe. Else we check if the mapping
 * intersects each OST and find last_stripe.
 * This function returns the last_stripe and also sets the stripe_count
 * over which the mapping is spread
 *
 * \param lsm striping information for the file
 * \param fm_start logical start of mapping
 * \param fm_end logical end of mapping
 * \param start_stripe starting stripe of the mapping
 * \param stripe_count the number of stripes across which to map is returned
 *
 * \retval last_stripe return the last stripe of the mapping
 */
int fiemap_calc_last_stripe(struct lov_stripe_md *lsm, obd_size fm_start,
                            obd_size fm_end, int start_stripe,
                            int *stripe_count)
{
        int last_stripe;
        obd_off obd_start, obd_end;
        int i, j;

        if (fm_end - fm_start > lsm->lsm_stripe_size * lsm->lsm_stripe_count) {
                last_stripe = (start_stripe < 1 ? lsm->lsm_stripe_count - 1 :
                                                              start_stripe - 1);
                *stripe_count = lsm->lsm_stripe_count;
        } else {
                for (j = 0, i = start_stripe; j < lsm->lsm_stripe_count;
                     i = (i + 1) % lsm->lsm_stripe_count, j++) {
                        if ((lov_stripe_intersects(lsm, i, fm_start, fm_end,
                                                   &obd_start, &obd_end)) == 0)
                                break;
                }
                *stripe_count = j;
                last_stripe = (start_stripe + j - 1) %lsm->lsm_stripe_count;
        }

        return last_stripe;
}

/**
 * Set fe_device and copy extents from local buffer into main return buffer.
 *
 * \param fiemap fiemap request header
 * \param lcl_fm_ext array of local fiemap extents to be copied
 * \param ost_index OST index to be written into the fm_device field for each
                    extent
 * \param ext_count number of extents to be copied
 * \param current_extent where to start copying in main extent array
 */
void fiemap_prepare_and_copy_exts(struct ll_user_fiemap *fiemap,
                                  struct ll_fiemap_extent *lcl_fm_ext,
                                  int ost_index, unsigned int ext_count,
                                  int current_extent)
{
        char *to;
        int ext;

        for (ext = 0; ext < ext_count; ext++) {
                lcl_fm_ext[ext].fe_device = ost_index;
                lcl_fm_ext[ext].fe_flags |= FIEMAP_EXTENT_NET;
        }

        /* Copy fm_extent's from fm_local to return buffer */
        to = (char *)fiemap + fiemap_count_to_size(current_extent);
        memcpy(to, lcl_fm_ext, ext_count * sizeof(struct ll_fiemap_extent));
}

/**
 * Break down the FIEMAP request and send appropriate calls to individual OSTs.
 * This also handles the restarting of FIEMAP calls in case mapping overflows
 * the available number of extents in single call.
 */
static int lov_fiemap(struct lov_obd *lov, __u32 keylen, void *key,
                      __u32 *vallen, void *val, struct lov_stripe_md *lsm)
{
        struct ll_fiemap_info_key *fm_key = key;
        struct ll_user_fiemap *fiemap = val;
        struct ll_user_fiemap *fm_local = NULL;
        struct ll_fiemap_extent *lcl_fm_ext;
        int count_local;
        unsigned int get_num_extents = 0;
        int ost_index = 0, actual_start_stripe, start_stripe;
	obd_size fm_start, fm_end, fm_length, fm_end_offset;
        obd_size curr_loc;
        int current_extent = 0, rc = 0, i;
        int ost_eof = 0; /* EOF for object */
        int ost_done = 0; /* done with required mapping for this OST? */
        int last_stripe;
        int cur_stripe = 0, cur_stripe_wrap = 0, stripe_count;
        unsigned int buffer_size = FIEMAP_BUFFER_SIZE;

        if (lsm == NULL)
                GOTO(out, rc = 0);

        if (fiemap_count_to_size(fm_key->fiemap.fm_extent_count) < buffer_size)
                buffer_size = fiemap_count_to_size(fm_key->fiemap.fm_extent_count);

        OBD_ALLOC_LARGE(fm_local, buffer_size);
        if (fm_local == NULL)
                GOTO(out, rc = -ENOMEM);
        lcl_fm_ext = &fm_local->fm_extents[0];

        count_local = fiemap_size_to_count(buffer_size);

        memcpy(fiemap, &fm_key->fiemap, sizeof(*fiemap));
        fm_start = fiemap->fm_start;
        fm_length = fiemap->fm_length;
        /* Calculate start stripe, last stripe and length of mapping */
        actual_start_stripe = start_stripe = lov_stripe_number(lsm, fm_start);
        fm_end = (fm_length == ~0ULL ? fm_key->oa.o_size :
                                                fm_start + fm_length - 1);
        /* If fm_length != ~0ULL but fm_start+fm_length-1 exceeds file size */
        if (fm_end > fm_key->oa.o_size)
                fm_end = fm_key->oa.o_size;

        last_stripe = fiemap_calc_last_stripe(lsm, fm_start, fm_end,
                                            actual_start_stripe, &stripe_count);

	fm_end_offset = fiemap_calc_fm_end_offset(fiemap, lsm, fm_start,
						  fm_end, &start_stripe);
	if (fm_end_offset == -EINVAL)
		return -EINVAL;

        if (fiemap->fm_extent_count == 0) {
                get_num_extents = 1;
                count_local = 0;
        }

        /* Check each stripe */
        for (cur_stripe = start_stripe, i = 0; i < stripe_count;
             i++, cur_stripe = (cur_stripe + 1) % lsm->lsm_stripe_count) {
                obd_size req_fm_len; /* Stores length of required mapping */
                obd_size len_mapped_single_call;
                obd_off lun_start, lun_end, obd_object_end;
                unsigned int ext_count;

                cur_stripe_wrap = cur_stripe;

                /* Find out range of mapping on this stripe */
                if ((lov_stripe_intersects(lsm, cur_stripe, fm_start, fm_end,
                                           &lun_start, &obd_object_end)) == 0)
                        continue;

                /* If this is a continuation FIEMAP call and we are on
                 * starting stripe then lun_start needs to be set to
                 * fm_end_offset */
                if (fm_end_offset != 0 && cur_stripe == start_stripe)
                        lun_start = fm_end_offset;

                if (fm_length != ~0ULL) {
                        /* Handle fm_start + fm_length overflow */
                        if (fm_start + fm_length < fm_start)
                                fm_length = ~0ULL - fm_start;
                        lun_end = lov_size_to_stripe(lsm, fm_start + fm_length,
                                                     cur_stripe);
                } else {
                        lun_end = ~0ULL;
                }

                if (lun_start == lun_end)
                        continue;

                req_fm_len = obd_object_end - lun_start;
                fm_local->fm_length = 0;
                len_mapped_single_call = 0;

                /* If the output buffer is very large and the objects have many
                 * extents we may need to loop on a single OST repeatedly */
                ost_eof = 0;
                ost_done = 0;
                do {
                        if (get_num_extents == 0) {
                                /* Don't get too many extents. */
                                if (current_extent + count_local >
                                    fiemap->fm_extent_count)
                                        count_local = fiemap->fm_extent_count -
                                                                 current_extent;
                        }

                        lun_start += len_mapped_single_call;
                        fm_local->fm_length = req_fm_len - len_mapped_single_call;
                        req_fm_len = fm_local->fm_length;
                        fm_local->fm_extent_count = count_local;
                        fm_local->fm_mapped_extents = 0;
                        fm_local->fm_flags = fiemap->fm_flags;

                        fm_key->oa.o_id = lsm->lsm_oinfo[cur_stripe]->loi_id;
                        fm_key->oa.o_seq = lsm->lsm_oinfo[cur_stripe]->loi_seq;
                        ost_index = lsm->lsm_oinfo[cur_stripe]->loi_ost_idx;

                        if (ost_index < 0 || ost_index >=lov->desc.ld_tgt_count)
                                GOTO(out, rc = -EINVAL);

                        /* If OST is inactive, return extent with UNKNOWN flag */
                        if (!lov->lov_tgts[ost_index]->ltd_active) {
                                fm_local->fm_flags |= FIEMAP_EXTENT_LAST;
                                fm_local->fm_mapped_extents = 1;

                                lcl_fm_ext[0].fe_logical = lun_start;
                                lcl_fm_ext[0].fe_length = obd_object_end -
                                                                      lun_start;
                                lcl_fm_ext[0].fe_flags |= FIEMAP_EXTENT_UNKNOWN;

                                goto inactive_tgt;
                        }

                        fm_local->fm_start = lun_start;
                        fm_local->fm_flags &= ~FIEMAP_FLAG_DEVICE_ORDER;
                        memcpy(&fm_key->fiemap, fm_local, sizeof(*fm_local));
                        *vallen=fiemap_count_to_size(fm_local->fm_extent_count);
                        rc = obd_get_info(lov->lov_tgts[ost_index]->ltd_exp,
                                          keylen, key, vallen, fm_local, lsm);
                        if (rc != 0)
                                GOTO(out, rc);

inactive_tgt:
                        ext_count = fm_local->fm_mapped_extents;
                        if (ext_count == 0) {
                                ost_done = 1;
                                /* If last stripe has hole at the end,
                                 * then we need to return */
                                if (cur_stripe_wrap == last_stripe) {
                                        fiemap->fm_mapped_extents = 0;
                                        goto finish;
                                }
                                break;
                        }

                        /* If we just need num of extents then go to next device */
                        if (get_num_extents) {
                                current_extent += ext_count;
                                break;
                        }

                        len_mapped_single_call = lcl_fm_ext[ext_count-1].fe_logical -
                                  lun_start + lcl_fm_ext[ext_count - 1].fe_length;

                        /* Have we finished mapping on this device? */
                        if (req_fm_len <= len_mapped_single_call)
                                ost_done = 1;

                        /* Clear the EXTENT_LAST flag which can be present on
                         * last extent */
                        if (lcl_fm_ext[ext_count-1].fe_flags & FIEMAP_EXTENT_LAST)
                                lcl_fm_ext[ext_count - 1].fe_flags &=
                                                            ~FIEMAP_EXTENT_LAST;

                        curr_loc = lov_stripe_size(lsm,
                                           lcl_fm_ext[ext_count - 1].fe_logical+
                                           lcl_fm_ext[ext_count - 1].fe_length,
                                           cur_stripe);
                        if (curr_loc >= fm_key->oa.o_size)
                                ost_eof = 1;

                        fiemap_prepare_and_copy_exts(fiemap, lcl_fm_ext,
                                                     ost_index, ext_count,
                                                     current_extent);

                        current_extent += ext_count;

                        /* Ran out of available extents? */
                        if (current_extent >= fiemap->fm_extent_count)
                                goto finish;
                } while (ost_done == 0 && ost_eof == 0);

                if (cur_stripe_wrap == last_stripe)
                        goto finish;
        }

finish:
        /* Indicate that we are returning device offsets unless file just has
         * single stripe */
        if (lsm->lsm_stripe_count > 1)
                fiemap->fm_flags |= FIEMAP_FLAG_DEVICE_ORDER;

        if (get_num_extents)
                goto skip_last_device_calc;

        /* Check if we have reached the last stripe and whether mapping for that
         * stripe is done. */
        if (cur_stripe_wrap == last_stripe) {
                if (ost_done || ost_eof)
                        fiemap->fm_extents[current_extent - 1].fe_flags |=
                                                             FIEMAP_EXTENT_LAST;
        }

skip_last_device_calc:
        fiemap->fm_mapped_extents = current_extent;

out:
        OBD_FREE_LARGE(fm_local, buffer_size);
        return rc;
}

static int lov_get_info(struct obd_export *exp, __u32 keylen,
                        void *key, __u32 *vallen, void *val,
                        struct lov_stripe_md *lsm)
{
        struct obd_device *obddev = class_exp2obd(exp);
        struct lov_obd *lov = &obddev->u.lov;
        int i, rc;
        ENTRY;

        if (!vallen || !val)
                RETURN(-EFAULT);

        obd_getref(obddev);

        if (KEY_IS(KEY_LOCK_TO_STRIPE)) {
                struct {
                        char name[16];
                        struct ldlm_lock *lock;
                } *data = key;
                struct ldlm_res_id *res_id = &data->lock->l_resource->lr_name;
                struct lov_oinfo *loi;
                __u32 *stripe = val;

                if (*vallen < sizeof(*stripe))
                        GOTO(out, rc = -EFAULT);
                *vallen = sizeof(*stripe);

                /* XXX This is another one of those bits that will need to
                 * change if we ever actually support nested LOVs.  It uses
                 * the lock's export to find out which stripe it is. */
                /* XXX - it's assumed all the locks for deleted OSTs have
                 * been cancelled. Also, the export for deleted OSTs will
                 * be NULL and won't match the lock's export. */
                for (i = 0; i < lsm->lsm_stripe_count; i++) {
                        loi = lsm->lsm_oinfo[i];
                        if (!lov->lov_tgts[loi->loi_ost_idx])
                                continue;
                        if (lov->lov_tgts[loi->loi_ost_idx]->ltd_exp ==
                            data->lock->l_conn_export &&
                            osc_res_name_eq(loi->loi_id, loi->loi_seq, res_id)) {
                                *stripe = i;
                                GOTO(out, rc = 0);
                        }
                }
                LDLM_ERROR(data->lock, "lock on inode without such object");
                dump_lsm(D_ERROR, lsm);
                GOTO(out, rc = -ENXIO);
        } else if (KEY_IS(KEY_LAST_ID)) {
                struct obd_id_info *info = val;
                __u32 size = sizeof(obd_id);
                struct lov_tgt_desc *tgt;

                LASSERT(*vallen == sizeof(struct obd_id_info));
                tgt = lov->lov_tgts[info->idx];

                if (!tgt || !tgt->ltd_active)
                        GOTO(out, rc = -ESRCH);

                rc = obd_get_info(tgt->ltd_exp, keylen, key, &size, info->data, NULL);
                GOTO(out, rc = 0);
        } else if (KEY_IS(KEY_LOVDESC)) {
                struct lov_desc *desc_ret = val;
                *desc_ret = lov->desc;

                GOTO(out, rc = 0);
        } else if (KEY_IS(KEY_FIEMAP)) {
                rc = lov_fiemap(lov, keylen, key, vallen, val, lsm);
                GOTO(out, rc);
        } else if (KEY_IS(KEY_CONNECT_FLAG)) {
                struct lov_tgt_desc *tgt;
                __u64 ost_idx = *((__u64*)val);

                LASSERT(*vallen == sizeof(__u64));
                LASSERT(ost_idx < lov->desc.ld_tgt_count);
                tgt = lov->lov_tgts[ost_idx];

                if (!tgt || !tgt->ltd_exp)
                        GOTO(out, rc = -ESRCH);

                *((__u64*)val) = tgt->ltd_exp->exp_connect_flags;
                GOTO(out, rc = 0);
        } else if (KEY_IS(KEY_TGT_COUNT)) {
                *((int *)val) = lov->desc.ld_tgt_count;
                GOTO(out, rc = 0);
        }

        rc = -EINVAL;

out:
        obd_putref(obddev);
        RETURN(rc);
}

static int lov_set_info_async(struct obd_export *exp, obd_count keylen,
                              void *key, obd_count vallen, void *val,
                              struct ptlrpc_request_set *set)
{
        struct obd_device *obddev = class_exp2obd(exp);
        struct lov_obd *lov = &obddev->u.lov;
        obd_count count;
        int i, rc = 0, err;
        struct lov_tgt_desc *tgt;
        unsigned incr, check_uuid,
                 do_inactive, no_set;
        unsigned next_id = 0,  mds_con = 0, capa = 0;
        ENTRY;

        incr = check_uuid = do_inactive = no_set = 0;
        if (set == NULL) {
                no_set = 1;
                set = ptlrpc_prep_set();
                if (!set)
                        RETURN(-ENOMEM);
        }

        obd_getref(obddev);
        count = lov->desc.ld_tgt_count;

        if (KEY_IS(KEY_NEXT_ID)) {
                count = vallen / sizeof(struct obd_id_info);
                vallen = sizeof(obd_id);
                incr = sizeof(struct obd_id_info);
                do_inactive = 1;
                next_id = 1;
        } else if (KEY_IS(KEY_CHECKSUM)) {
                do_inactive = 1;
        } else if (KEY_IS(KEY_EVICT_BY_NID)) {
                /* use defaults:  do_inactive = incr = 0; */
        } else if (KEY_IS(KEY_MDS_CONN)) {
                mds_con = 1;
        } else if (KEY_IS(KEY_CAPA_KEY)) {
                capa = 1;
        }

        for (i = 0; i < count; i++, val = (char *)val + incr) {
                if (next_id) {
                        tgt = lov->lov_tgts[((struct obd_id_info*)val)->idx];
                } else {
                        tgt = lov->lov_tgts[i];
                }
                /* OST was disconnected */
                if (!tgt || !tgt->ltd_exp)
                        continue;

                /* OST is inactive and we don't want inactive OSCs */
                if (!tgt->ltd_active && !do_inactive)
                        continue;

                if (mds_con) {
                        struct mds_group_info *mgi;

                        LASSERT(vallen == sizeof(*mgi));
                        mgi = (struct mds_group_info *)val;

                        /* Only want a specific OSC */
                        if (mgi->uuid && !obd_uuid_equals(mgi->uuid,
                                                &tgt->ltd_uuid))
                                continue;

                        err = obd_set_info_async(tgt->ltd_exp,
                                         keylen, key, sizeof(int),
                                         &mgi->group, set);
                } else if (next_id) {
                        err = obd_set_info_async(tgt->ltd_exp,
                                         keylen, key, vallen,
                                         ((struct obd_id_info*)val)->data, set);
                } else if (capa) {
                        struct mds_capa_info *info = (struct mds_capa_info*)val;

                        LASSERT(vallen == sizeof(*info));

                         /* Only want a specific OSC */
                        if (info->uuid &&
                            !obd_uuid_equals(info->uuid, &tgt->ltd_uuid))
                                continue;

                        err = obd_set_info_async(tgt->ltd_exp, keylen, key,
                                                 sizeof(*info->capa),
                                                 info->capa, set);
                } else {
                        /* Only want a specific OSC */
                        if (check_uuid &&
                            !obd_uuid_equals(val, &tgt->ltd_uuid))
                                continue;

                        err = obd_set_info_async(tgt->ltd_exp,
                                         keylen, key, vallen, val, set);
                }

                if (!rc)
                        rc = err;
        }

        obd_putref(obddev);
        if (no_set) {
                err = ptlrpc_set_wait(set);
                if (!rc)
                        rc = err;
                ptlrpc_set_destroy(set);
        }
        RETURN(rc);
}

int lov_test_and_clear_async_rc(struct lov_stripe_md *lsm)
{
        int i, rc = 0;
        ENTRY;

        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                struct lov_oinfo *loi = lsm->lsm_oinfo[i];
                if (loi->loi_ar.ar_rc && !rc)
                        rc = loi->loi_ar.ar_rc;
                loi->loi_ar.ar_rc = 0;
        }
        RETURN(rc);
}
EXPORT_SYMBOL(lov_test_and_clear_async_rc);


static int lov_extent_calc(struct obd_export *exp, struct lov_stripe_md *lsm,
                           int cmd, __u64 *offset)
{
        __u32 ssize = lsm->lsm_stripe_size;
        __u64 start;

        start = *offset;
        do_div(start, ssize);
        start = start * ssize;

        CDEBUG(D_DLMTRACE, "offset "LPU64", stripe %u, start "LPU64
                           ", end "LPU64"\n", *offset, ssize, start,
                           start + ssize - 1);
        if (cmd == OBD_CALC_STRIPE_END) {
                *offset = start + ssize - 1;
        } else if (cmd == OBD_CALC_STRIPE_START) {
                *offset = start;
        } else {
                LBUG();
        }

        RETURN(0);
}

void lov_stripe_lock(struct lov_stripe_md *md)
{
        LASSERT(md->lsm_lock_owner != cfs_curproc_pid());
        cfs_spin_lock(&md->lsm_lock);
        LASSERT(md->lsm_lock_owner == 0);
        md->lsm_lock_owner = cfs_curproc_pid();
}
EXPORT_SYMBOL(lov_stripe_lock);

void lov_stripe_unlock(struct lov_stripe_md *md)
{
        LASSERT(md->lsm_lock_owner == cfs_curproc_pid());
        md->lsm_lock_owner = 0;
        cfs_spin_unlock(&md->lsm_lock);
}
EXPORT_SYMBOL(lov_stripe_unlock);


struct obd_ops lov_obd_ops = {
        .o_owner               = THIS_MODULE,
        .o_setup               = lov_setup,
        .o_precleanup          = lov_precleanup,
        .o_cleanup             = lov_cleanup,
        //.o_process_config      = lov_process_config,
        .o_connect             = lov_connect,
        .o_disconnect          = lov_disconnect,
        .o_statfs              = lov_statfs,
        .o_statfs_async        = lov_statfs_async,
        .o_packmd              = lov_packmd,
        .o_unpackmd            = lov_unpackmd,
        .o_create              = lov_create,
        .o_destroy             = lov_destroy,
        .o_getattr             = lov_getattr,
        .o_getattr_async       = lov_getattr_async,
        .o_setattr             = lov_setattr,
        .o_setattr_async       = lov_setattr_async,
        .o_brw                 = lov_brw,
        .o_merge_lvb           = lov_merge_lvb,
        .o_adjust_kms          = lov_adjust_kms,
        .o_punch               = lov_punch,
        .o_sync                = lov_sync,
        .o_enqueue             = lov_enqueue,
        .o_change_cbdata       = lov_change_cbdata,
        .o_find_cbdata         = lov_find_cbdata,
        .o_cancel              = lov_cancel,
        .o_cancel_unused       = lov_cancel_unused,
        .o_iocontrol           = lov_iocontrol,
        .o_get_info            = lov_get_info,
        .o_set_info_async      = lov_set_info_async,
        .o_extent_calc         = lov_extent_calc,
        .o_llog_init           = lov_llog_init,
        .o_llog_finish         = lov_llog_finish,
        .o_notify              = lov_notify,
        .o_pool_new            = lov_pool_new,
        .o_pool_rem            = lov_pool_remove,
        .o_pool_add            = lov_pool_add,
        .o_pool_del            = lov_pool_del,
        .o_getref              = lov_getref,
        .o_putref              = lov_putref,
};

static quota_interface_t *quota_interface;
extern quota_interface_t lov_quota_interface;

cfs_mem_cache_t *lov_oinfo_slab;

extern struct lu_kmem_descr lov_caches[];

int __init lov_init(void)
{
        struct lprocfs_static_vars lvars = { 0 };
        int rc, rc2;
        ENTRY;

        /* print an address of _any_ initialized kernel symbol from this
         * module, to allow debugging with gdb that doesn't support data
         * symbols from modules.*/
        CDEBUG(D_CONSOLE, "Lustre LOV module (%p).\n", &lov_caches);

        rc = lu_kmem_init(lov_caches);
        if (rc)
                return rc;

        lov_oinfo_slab = cfs_mem_cache_create("lov_oinfo",
                                              sizeof(struct lov_oinfo),
                                              0, CFS_SLAB_HWCACHE_ALIGN);
        if (lov_oinfo_slab == NULL) {
                lu_kmem_fini(lov_caches);
                return -ENOMEM;
        }
        lprocfs_lov_init_vars(&lvars);

        cfs_request_module("lquota");
        quota_interface = PORTAL_SYMBOL_GET(lov_quota_interface);
        init_obd_quota_ops(quota_interface, &lov_obd_ops);

        rc = class_register_type(&lov_obd_ops, NULL, lvars.module_vars,
                                 LUSTRE_LOV_NAME, &lov_device_type);

        if (rc) {
                if (quota_interface)
                        PORTAL_SYMBOL_PUT(lov_quota_interface);
                rc2 = cfs_mem_cache_destroy(lov_oinfo_slab);
                LASSERT(rc2 == 0);
                lu_kmem_fini(lov_caches);
        }

        RETURN(rc);
}

#ifdef __KERNEL__
static void /*__exit*/ lov_exit(void)
{
        int rc;

        lu_device_type_fini(&lov_device_type);
        lu_kmem_fini(lov_caches);

        if (quota_interface)
                PORTAL_SYMBOL_PUT(lov_quota_interface);

        class_unregister_type(LUSTRE_LOV_NAME);
        rc = cfs_mem_cache_destroy(lov_oinfo_slab);
        LASSERT(rc == 0);
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Logical Object Volume OBD driver");
MODULE_LICENSE("GPL");

cfs_module(lov, LUSTRE_VERSION_STRING, lov_init, lov_exit);
#endif
