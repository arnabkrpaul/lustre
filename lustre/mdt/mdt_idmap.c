/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2004-2006 Cluster File Systems, Inc.
 *   Author: Lai Siyao <lsy@clusterfs.com>
 *   Author: Fan Yong <fanyong@clusterfs.com>
 *
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
#define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#ifdef HAVE_KERNEL_CONFIG_H
#include <linux/config.h>
#endif
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/kmod.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/version.h>
#include <linux/unistd.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <asm/segment.h>

#include <libcfs/kp30.h>
#include <obd.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lustre_net.h>
#include <lustre_import.h>
#include <lustre_dlm.h>
#include <lustre_sec.h>
#include <lustre_lib.h>
#include <lustre_ucache.h>

#include "mdt_internal.h"

enum {
        MDT_IDMAP_NOTFOUND      = -1,
};

struct mdt_idmap_entry {
        struct list_head mie_rmt_hash; /* hashed as mie_rmt_id; */
        struct list_head mie_lcl_hash; /* hashed as mie_lcl_id; */
        int              mie_refcount;
        uid_t            mie_rmt_id;   /* remote uid/gid */
        uid_t            mie_lcl_id;   /* local uid/gid */
};

/* uid/gid mapping */
static struct mdt_idmap_table *mdt_idmap_alloc(void)
{
        struct mdt_idmap_table *tbl;
        int i, j;

        OBD_ALLOC_PTR(tbl);
        if (!tbl)
                return NULL;

        spin_lock_init(&tbl->mit_lock);
        for (i = 0; i < ARRAY_SIZE(tbl->mit_idmaps); i++)
                for (j = 0; j < ARRAY_SIZE(tbl->mit_idmaps[i]); j++)
                        INIT_LIST_HEAD(&tbl->mit_idmaps[i][j]);

        return tbl;
}

static struct mdt_idmap_entry *idmap_entry_alloc(__u32 mie_rmt_id,
                                                 __u32 mie_lcl_id)
{
        struct mdt_idmap_entry *e;

        OBD_ALLOC_PTR(e);
        if (!e)
                return NULL;

        INIT_LIST_HEAD(&e->mie_rmt_hash);
        INIT_LIST_HEAD(&e->mie_lcl_hash);
        e->mie_refcount = 1;
        e->mie_rmt_id = mie_rmt_id;
        e->mie_lcl_id = mie_lcl_id;

        return e;
}

static void idmap_entry_free(struct mdt_idmap_entry *e)
{
        if (!list_empty(&e->mie_rmt_hash))
                list_del(&e->mie_rmt_hash);
        if (!list_empty(&e->mie_lcl_hash))
                list_del(&e->mie_lcl_hash);
        OBD_FREE_PTR(e);
}

int mdt_init_idmap(struct mdt_thread_info *info)
{
        struct ptlrpc_request *req = mdt_info_req(info);
        char *client = libcfs_nid2str(req->rq_peer.nid);
        struct mdt_export_data *med = mdt_req2med(req);
        struct obd_device *obd = req->rq_export->exp_obd;
        struct obd_connect_data *data, *reply;
        int rc = 0, remote;
        ENTRY;

        data = req_capsule_client_get(&info->mti_pill, &RMF_CONNECT_DATA);
        reply = req_capsule_server_get(&info->mti_pill, &RMF_CONNECT_DATA);
        if (data == NULL || reply == NULL)
                RETURN(-EFAULT);

        if (!req->rq_auth_gss || req->rq_auth_usr_mdt) {
                med->med_rmtclient = 0;
                reply->ocd_connect_flags &= ~OBD_CONNECT_RMT_CLIENT;
                //reply->ocd_connect_flags |= OBD_CONNECT_LCL_CLIENT;
                RETURN(0);
        }

        remote = data->ocd_connect_flags & OBD_CONNECT_RMT_CLIENT;

        if (remote) {
                med->med_rmtclient = 1;
                if (!req->rq_auth_remote)
                        CWARN("client (local realm) %s -> target %s asked "
                              "to be remote!\n", client, obd->obd_name);
        } else if (req->rq_auth_remote) {
                med->med_rmtclient = 1;
                CWARN("client (remote realm) %s -> target %s forced "
                      "to be remote!\n", client, obd->obd_name);
        }

        if (med->med_rmtclient) {
                med->med_nllu = data->ocd_nllu;
                med->med_nllg = data->ocd_nllg;
                if (!med->med_idmap)
                        med->med_idmap = mdt_idmap_alloc();
                if (!med->med_idmap) {
                        CERROR("client %s -> target %s failed to alloc idmap!\n"
                               , client, obd->obd_name);
                        RETURN(-ENOMEM);
                }

                reply->ocd_connect_flags &= ~OBD_CONNECT_LCL_CLIENT;
                //reply->ocd_connect_flags |= OBD_CONNECT_RMT_CLIENT;
                CDEBUG(D_SEC, "client %s -> target %s is remote.\n",
                       client, obd->obd_name);

                /* NB, MDT_CONNECT establish root idmap too! */
                rc = mdt_handle_idmap(info);
        } else {
                if (req->rq_auth_uid == INVALID_UID) {
                        CERROR("client %s -> target %s: user is not "
                               "authenticated!\n", client, obd->obd_name);
                        RETURN(-EACCES);
                }
                reply->ocd_connect_flags &= ~OBD_CONNECT_RMT_CLIENT;
                //reply->ocd_connect_flags |= OBD_CONNECT_LCL_CLIENT;
        }

        RETURN(rc);
}

static void idmap_clear_mie_rmt_hash(struct list_head *list)
{
        struct mdt_idmap_entry *e;
        int i;

        for (i = 0; i < MDT_IDMAP_HASHSIZE; i++) {
                while (!list_empty(&list[i])) {
                        e = list_entry(list[i].next, struct mdt_idmap_entry,
                                       mie_rmt_hash);
                        idmap_entry_free(e);
                }
        }
}

void mdt_cleanup_idmap(struct mdt_export_data *med)
{
        struct mdt_idmap_table *tbl = med->med_idmap;
        int i;

        LASSERT(med->med_rmtclient);
        LASSERT(tbl);

        spin_lock(&tbl->mit_lock);
        idmap_clear_mie_rmt_hash(tbl->mit_idmaps[RMT_UIDMAP_IDX]);
        idmap_clear_mie_rmt_hash(tbl->mit_idmaps[RMT_GIDMAP_IDX]);

        /* paranoid checking */
        for (i = 0; i < MDT_IDMAP_HASHSIZE; i++) {
                LASSERT(list_empty(&tbl->mit_idmaps[LCL_UIDMAP_IDX][i]));
                LASSERT(list_empty(&tbl->mit_idmaps[LCL_GIDMAP_IDX][i]));
        }
        spin_unlock(&tbl->mit_lock);

        OBD_FREE_PTR(tbl);
        med->med_idmap = NULL;
}

static inline void mdt_revoke_export_locks(struct obd_export *exp)
{
        /* don't revoke locks during recovery */
        if (exp->exp_obd->obd_recovering)
                return;

        ldlm_revoke_export_locks(exp);
}

static
struct mdt_idmap_entry *idmap_lookup_entry(struct list_head *mie_rmt_hash,
                                           uid_t mie_rmt_id, uid_t mie_lcl_id)
{
        struct list_head *rmt_head =
                         &mie_rmt_hash[MDT_IDMAP_HASHFUNC(mie_rmt_id)];
        struct mdt_idmap_entry *e;

        list_for_each_entry(e, rmt_head, mie_rmt_hash) {
                if ((e->mie_rmt_id == mie_rmt_id) &&
                    (e->mie_lcl_id == mie_lcl_id))
                        return e;
        }
        return NULL;
}

/*
 * return value
 * NULL: not found entry
 * ERR_PTR(-EACCES): found multi->single mapped entry
 * others: found normal entry
 */
static
struct mdt_idmap_entry *idmap_search_entry(struct list_head *mie_rmt_hash,
                                           uid_t mie_rmt_id,
                                           struct list_head *mie_lcl_hash,
                                           uid_t mie_lcl_id,
                                           const char *warn_msg)
{
        struct list_head *rmt_head =
                         &mie_rmt_hash[MDT_IDMAP_HASHFUNC(mie_rmt_id)];
        struct list_head *lcl_head =
                         &mie_lcl_hash[MDT_IDMAP_HASHFUNC(mie_lcl_id)];
        struct mdt_idmap_entry *e;

        list_for_each_entry(e, rmt_head, mie_rmt_hash) {
                if (e->mie_rmt_id == mie_rmt_id) {
                        if (e->mie_lcl_id == mie_lcl_id) {
                                e->mie_refcount++;
                                return e;
                        } else {
                                CERROR("%s: rmt id %u already be mapped to %u"
                                       " (new %u)\n", warn_msg, e->mie_rmt_id,
                                       e->mie_lcl_id, mie_lcl_id);
                                return ERR_PTR(-EACCES);
                        }
                }
        }

        list_for_each_entry(e, lcl_head, mie_lcl_hash) {
                if (e->mie_lcl_id == mie_lcl_id) {
                        if (e->mie_rmt_id == mie_rmt_id) {
                                e->mie_refcount++;
                                return e;
                        } else {
                                CERROR("%s: lcl id %u already be mapped from %u"
                                       " (new %u)\n", warn_msg, e->mie_lcl_id,
                                       e->mie_rmt_id, mie_rmt_id);
                                return ERR_PTR(-EACCES);
                        }
                }
        }

        return NULL;
}

static
struct mdt_idmap_entry *idmap_insert_entry(struct list_head *mie_rmt_hash,
                                           struct list_head *mie_lcl_hash,
                                           struct mdt_idmap_entry *new,
                                           const char *warn_msg)
{
        struct list_head *rmt_head =
                         &mie_rmt_hash[MDT_IDMAP_HASHFUNC(new->mie_rmt_id)];
        struct list_head *lcl_head =
                         &mie_lcl_hash[MDT_IDMAP_HASHFUNC(new->mie_lcl_id)];
        struct mdt_idmap_entry *e;

        e = idmap_search_entry(mie_rmt_hash, new->mie_rmt_id,
                               mie_lcl_hash, new->mie_lcl_id,
                               warn_msg);
        if (e == NULL) {
                list_add_tail(&new->mie_rmt_hash, rmt_head);
                list_add_tail(&new->mie_lcl_hash, lcl_head);
        }
        return e;
}

static int idmap_remove_entry(struct list_head *mie_rmt_hash,
                              struct list_head *mie_lcl_hash,
                              __u32 mie_rmt_id, __u32 mie_lcl_id)
{
        struct mdt_idmap_entry *e;
        int rc = -ENOENT;

        e = idmap_lookup_entry(mie_rmt_hash, mie_rmt_id, mie_lcl_id);
        if (e != NULL) {
                        e->mie_refcount--;
                        if ((rc = e->mie_refcount) <= 0)
                                idmap_entry_free(e);
        }
        return rc;
}

static int mdt_idmap_add(struct mdt_idmap_table *tbl,
                         uid_t ruid, uid_t luid,
                         gid_t rgid, gid_t lgid)
{
        struct mdt_idmap_entry *ue0, *ue1, *ge0, *ge1;
        ENTRY;

        LASSERT(tbl);

        spin_lock(&tbl->mit_lock);
        ue0 = idmap_search_entry(tbl->mit_idmaps[RMT_UIDMAP_IDX], ruid,
                                 tbl->mit_idmaps[LCL_UIDMAP_IDX], luid,
                                 "UID mapping");
        spin_unlock(&tbl->mit_lock);
        if (!ue0) {
                ue0 = idmap_entry_alloc(ruid, luid);
                if (!ue0)
                        RETURN(-ENOMEM);

                spin_lock(&tbl->mit_lock);
                ue1 = idmap_insert_entry(tbl->mit_idmaps[RMT_UIDMAP_IDX],
                                         tbl->mit_idmaps[LCL_UIDMAP_IDX],
                                         ue0, "UID mapping");
                if (ue1 != NULL) {
                        idmap_entry_free(ue0);
                        ue0 = ue1;
                }
                spin_unlock(&tbl->mit_lock);

                if (IS_ERR(ue1))
                        RETURN(PTR_ERR(ue1));
        } else if (IS_ERR(ue0)) {
                RETURN(PTR_ERR(ue0));
        }

        spin_lock(&tbl->mit_lock);
        ge0 = idmap_search_entry(tbl->mit_idmaps[RMT_GIDMAP_IDX], rgid,
                                 tbl->mit_idmaps[LCL_GIDMAP_IDX], lgid,
                                 "GID mapping");
        spin_unlock(&tbl->mit_lock);
        if (!ge0) {
                ge0 = idmap_entry_alloc(rgid, lgid);
                spin_lock(&tbl->mit_lock);
                if (!ge0) {
                        ue0->mie_refcount--;
                        if (ue0->mie_refcount <= 0)
                                idmap_entry_free(ue0);
                        spin_unlock(&tbl->mit_lock);
                        RETURN(-ENOMEM);
                }

                ge1 = idmap_insert_entry(tbl->mit_idmaps[RMT_GIDMAP_IDX],
                                         tbl->mit_idmaps[LCL_GIDMAP_IDX],
                                         ge0, "GID mapping");
                if (ge1 != NULL) {
                        ue0->mie_refcount--;
                        if (ue0->mie_refcount <= 0)
                                idmap_entry_free(ue0);
                        idmap_entry_free(ge0);
                }
                spin_unlock(&tbl->mit_lock);

                if (IS_ERR(ge1))
                        RETURN(PTR_ERR(ge1));
        } else if (IS_ERR(ge0)) {
                spin_lock(&tbl->mit_lock);
                ue0->mie_refcount--;
                if (ue0->mie_refcount <= 0)
                        idmap_entry_free(ue0);
                spin_unlock(&tbl->mit_lock);
                RETURN(PTR_ERR(ge0));
        }

        RETURN(0);
}

static int mdt_idmap_del(struct mdt_idmap_table *tbl,
                         uid_t ruid, uid_t luid,
                         gid_t rgid, gid_t lgid)
{
        ENTRY;

        if (!tbl)
                RETURN(0);

        spin_lock(&tbl->mit_lock);
        idmap_remove_entry(tbl->mit_idmaps[RMT_UIDMAP_IDX],
                           tbl->mit_idmaps[LCL_UIDMAP_IDX],
                           ruid, luid);
        idmap_remove_entry(tbl->mit_idmaps[RMT_GIDMAP_IDX],
                           tbl->mit_idmaps[LCL_GIDMAP_IDX],
                           rgid, lgid);
        spin_unlock(&tbl->mit_lock);

        RETURN(0);
}

int mdt_handle_idmap(struct mdt_thread_info *info)
{
        struct ptlrpc_request *req = mdt_info_req(info);
        struct mdt_device *mdt = info->mti_mdt;
        struct mdt_export_data *med;
        struct ptlrpc_user_desc *pud = req->rq_user_desc;
        struct mdt_identity *identity;
        __u32 opc;
        int rc = 0;

        ENTRY;

        if (!req->rq_export)
                RETURN(0);

        med = mdt_req2med(req);
        if (!med->med_rmtclient)
                RETURN(0);

        opc = lustre_msg_get_opc(req->rq_reqmsg);
        /* Bypass other opc */
        if ((opc != SEC_CTX_INIT) && (opc != SEC_CTX_INIT_CONT) &&
            (opc != SEC_CTX_FINI) && (opc != MDS_CONNECT))
                RETURN(0);

        LASSERT(pud);
        LASSERT(med->med_idmap);

        if (req->rq_auth_mapped_uid == INVALID_UID) {
                CERROR("invalid authorized mapped uid, please check "
                       "/etc/lustre/idmap.conf!\n");
                RETURN(-EACCES);
        }

        if (is_identity_get_disabled(mdt->mdt_identity_cache)) {
                CERROR("remote client must run with identity_get enabled!\n");
                RETURN(-EACCES);
        }

        identity = mdt_identity_get(mdt->mdt_identity_cache,
                                    req->rq_auth_mapped_uid);
        if (!identity) {
                CERROR("can't get mdt identity(%u), no mapping added\n",
                       req->rq_auth_mapped_uid);
                RETURN(-EACCES);
        }

        switch (opc) {
        case SEC_CTX_INIT:
        case SEC_CTX_INIT_CONT:
        case MDS_CONNECT:
                rc = mdt_idmap_add(med->med_idmap,
                                   pud->pud_uid, identity->mi_uid,
                                   pud->pud_gid, identity->mi_gid);
                break;
        case SEC_CTX_FINI:
                rc = mdt_idmap_del(med->med_idmap,
                                   pud->pud_uid, identity->mi_uid,
                                   pud->pud_gid, identity->mi_gid);
                break;
        }

        mdt_identity_put(mdt->mdt_identity_cache, identity);

        if (rc)
                RETURN(rc);

        switch (opc) {
        case SEC_CTX_INIT:
        case SEC_CTX_INIT_CONT:
        case SEC_CTX_FINI:
                mdt_revoke_export_locks(req->rq_export);
                break;
        }
        RETURN(0);
}

static __u32 idmap_lookup_id(struct list_head *hash, int reverse, __u32 id)
{
        struct list_head *head = &hash[MDT_IDMAP_HASHFUNC(id)];
        struct mdt_idmap_entry *e;

        if (!reverse) {
                list_for_each_entry(e, head, mie_rmt_hash) {
                        if (e->mie_rmt_id == id)
                                return e->mie_lcl_id;
                }
        } else {
                list_for_each_entry(e, head, mie_lcl_hash) {
                        if (e->mie_lcl_id == id)
                                return e->mie_rmt_id;
                }
        }
        return MDT_IDMAP_NOTFOUND;
}

static int mdt_idmap_lookup_uid(struct mdt_idmap_table *tbl, int reverse,
                                uid_t uid)
{
        struct list_head *hash;

        if (!tbl)
                return MDT_IDMAP_NOTFOUND;

        hash = tbl->mit_idmaps[reverse ? LCL_UIDMAP_IDX : RMT_UIDMAP_IDX];

        spin_lock(&tbl->mit_lock);
        uid = idmap_lookup_id(hash, reverse, uid);
        spin_unlock(&tbl->mit_lock);

        return uid;
}

static int mdt_idmap_lookup_gid(struct mdt_idmap_table *tbl, int reverse,
                                gid_t gid)
{
        struct list_head *hash;

        if (!tbl)
                return MDT_IDMAP_NOTFOUND;

        hash = tbl->mit_idmaps[reverse ? LCL_GIDMAP_IDX : RMT_GIDMAP_IDX];

        spin_lock(&tbl->mit_lock);
        gid = idmap_lookup_id(hash, reverse, gid);
        spin_unlock(&tbl->mit_lock);

        return gid;
}

int ptlrpc_user_desc_do_idmap(struct ptlrpc_request *req,
                              struct ptlrpc_user_desc *pud)
{
        struct mdt_export_data *med = mdt_req2med(req);
        struct mdt_idmap_table *idmap = med->med_idmap;
        uid_t uid, fsuid;
        gid_t gid, fsgid;

        /* Only remote client need desc_to_idmap. */
        if (!med->med_rmtclient)
                return 0;

        uid = mdt_idmap_lookup_uid(idmap, 0, pud->pud_uid);
        if (uid == MDT_IDMAP_NOTFOUND) {
                CERROR("no mapping for uid %u\n", pud->pud_uid);
                return -EACCES;
        }

        if (pud->pud_uid == pud->pud_fsuid) {
                fsuid = uid;
        } else {
                fsuid = mdt_idmap_lookup_uid(idmap, 0, pud->pud_fsuid);
                if (fsuid == MDT_IDMAP_NOTFOUND) {
                        CERROR("no mapping for fsuid %u\n", pud->pud_fsuid);
                        return -EACCES;
                }
        }

        gid = mdt_idmap_lookup_gid(idmap, 0, pud->pud_gid);
        if (gid == MDT_IDMAP_NOTFOUND) {
                CERROR("no mapping for gid %u\n", pud->pud_gid);
                return -EACCES;
        }

        if (pud->pud_gid == pud->pud_fsgid) {
                fsgid = gid;
        } else {
                fsgid = mdt_idmap_lookup_gid(idmap, 0, pud->pud_fsgid);
                if (fsgid == MDT_IDMAP_NOTFOUND) {
                        CERROR("no mapping for fsgid %u\n", pud->pud_fsgid);
                        return -EACCES;
                }
        }

        pud->pud_uid = uid;
        pud->pud_gid = gid;
        pud->pud_fsuid = fsuid;
        pud->pud_fsgid = fsgid;

        return 0;
}

/*
 * Reverse map
 * Do not ignore rootsquash.
 */
void mdt_body_reverse_idmap(struct mdt_thread_info *info, struct mdt_body *body)
{
        struct ptlrpc_request   *req = mdt_info_req(info);
        struct md_ucred         *uc = mdt_ucred(info);
        struct mdt_export_data  *med = mdt_req2med(req);
        struct mdt_idmap_table  *idmap = med->med_idmap;
        uid_t uid;
        gid_t gid;

        if (!med->med_rmtclient)
                return;

        if (body->valid & OBD_MD_FLUID) {
                if (((uc->mu_valid == UCRED_OLD) ||
                    (uc->mu_valid == UCRED_NEW)) &&
                    !(uc->mu_squash & SQUASH_UID)) {
                        if (body->uid == uc->mu_uid)
                                uid = uc->mu_o_uid;
                        else if (body->uid == uc->mu_fsuid)
                                uid = uc->mu_o_fsuid;
                        else
                                uid = mdt_idmap_lookup_uid(idmap, 1, body->uid);
                } else {
                        uid = mdt_idmap_lookup_uid(idmap, 1, body->uid);
                }

                if (uid == MDT_IDMAP_NOTFOUND) {
                        uid = med->med_nllu;
                        if (body->valid & OBD_MD_FLMODE)
                                body->mode = (body->mode & ~S_IRWXU) |
                                             ((body->mode & S_IRWXO) << 6);
                }

                body->uid = uid;
        }

        if (body->valid & OBD_MD_FLGID) {
                if (((uc->mu_valid == UCRED_OLD) ||
                    (uc->mu_valid == UCRED_NEW)) &&
                    !(uc->mu_squash & SQUASH_GID)) {
                        if (body->gid == uc->mu_gid)
                                gid = uc->mu_o_gid;
                        else if (body->gid == uc->mu_fsgid)
                                gid = uc->mu_o_fsgid;
                        else
                                gid = mdt_idmap_lookup_gid(idmap, 1, body->gid);
                } else {
                        gid = mdt_idmap_lookup_gid(idmap, 1, body->gid);
                }

                if (gid == MDT_IDMAP_NOTFOUND) {
                        gid = med->med_nllg;
                        if (body->valid & OBD_MD_FLMODE)
                                body->mode = (body->mode & ~S_IRWXG) |
                                             ((body->mode & S_IRWXO) << 3);
                }

                body->gid = gid;
        }
}

/* NB: return error if no mapping, so this will look strange:
 * if client hasn't kinit the to map xid for the mapped xid, client
 * will always get -EPERM, and the same for rootsquash case. */
int mdt_remote_perm_reverse_idmap(struct ptlrpc_request *req,
                                  struct mdt_remote_perm *perm)
{
        struct mdt_export_data *med = mdt_req2med(req);
        uid_t uid, fsuid;
        gid_t gid, fsgid;

        LASSERT(med->med_rmtclient);

        uid = mdt_idmap_lookup_uid(med->med_idmap, 1, perm->rp_uid);
        if (uid == MDT_IDMAP_NOTFOUND) {
                CERROR("no mapping for uid %u\n", perm->rp_uid);
                return -EPERM;
        }

        gid = mdt_idmap_lookup_gid(med->med_idmap, 1, perm->rp_gid);
        if (gid == MDT_IDMAP_NOTFOUND) {
                CERROR("no mapping for gid %u\n", perm->rp_gid);
                return -EPERM;
        }

        if (perm->rp_uid != perm->rp_fsuid) {
                fsuid = mdt_idmap_lookup_uid(med->med_idmap, 1, perm->rp_fsuid);
                if (fsuid == MDT_IDMAP_NOTFOUND) {
                        CERROR("no mapping for fsuid %u\n", perm->rp_fsuid);
                        return -EPERM;
                }
        } else {
                fsuid = uid;
        }

        if (perm->rp_gid != perm->rp_fsgid) {
                fsgid = mdt_idmap_lookup_gid(med->med_idmap, 1, perm->rp_fsgid);
                if (fsgid == MDT_IDMAP_NOTFOUND) {
                        CERROR("no mapping for fsgid %u\n", perm->rp_fsgid);
                        return -EPERM;
                }
        } else {
                fsgid = gid;
        }

        perm->rp_uid = uid;
        perm->rp_gid = gid;
        perm->rp_fsuid = fsuid;
        perm->rp_fsgid = fsgid;
        return 0;
}

/* Process remote client and rootsquash */
int mdt_fix_attr_ucred(struct mdt_thread_info *info, __u32 op)
{
        struct ptlrpc_request   *req = mdt_info_req(info);
        struct md_ucred         *uc = mdt_ucred(info);
        struct lu_attr          *attr = &info->mti_attr.ma_attr;
        struct mdt_export_data  *med = mdt_req2med(req);
        struct mdt_idmap_table  *idmap = med->med_idmap;

        ENTRY;

        if ((uc->mu_valid != UCRED_OLD) && (uc->mu_valid != UCRED_NEW))
                RETURN(-EINVAL);

        if (!med->med_rmtclient && (uc->mu_squash == SQUASH_NONE))
                RETURN(0);

        if (op != REINT_SETATTR) {
                if ((attr->la_valid & LA_UID) && (attr->la_uid != -1))
                        attr->la_uid = uc->mu_fsuid;
                if (op != REINT_CREATE) {
                        if ((attr->la_valid & LA_GID) && (attr->la_gid != -1))
                                attr->la_gid = uc->mu_fsgid;
                } else {
                        /* for S_ISGID, inherit gid from his parent */
                        if (!(attr->la_mode & S_ISGID) && (attr->la_gid != -1))
                                attr->la_gid = uc->mu_fsgid;
                }
        } else if (med->med_rmtclient) {
                /* NB: -1 case will be handled by mdt_fix_attr() later. */
                if ((attr->la_valid & LA_UID) && (attr->la_uid != -1)) {
                        uid_t uid;

                        if (attr->la_uid == uc->mu_o_uid)
                                uid = uc->mu_uid;
                        else if (attr->la_uid == uc->mu_o_fsuid)
                                uid = uc->mu_fsuid;
                        else
                                uid = mdt_idmap_lookup_uid(idmap, 0,
                                                           attr->la_uid);

                        if (uid == MDT_IDMAP_NOTFOUND) {
                                CWARN("Deny chown to uid %u\n", attr->la_uid);
                                RETURN(-EPERM);
                        }

                        attr->la_uid = uid;
                }
                if ((attr->la_valid & LA_GID) && (attr->la_gid != -1)) {
                        gid_t gid;

                        if (attr->la_gid == uc->mu_o_gid)
                                gid = uc->mu_gid;
                        else if (attr->la_gid == uc->mu_o_fsgid)
                                gid = uc->mu_fsgid;
                        else
                                gid = mdt_idmap_lookup_gid(idmap, 0,
                                                           attr->la_gid);

                        if (gid == MDT_IDMAP_NOTFOUND) {
                                CWARN("Deny chown to gid %u\n", attr->la_gid);
                                RETURN(-EPERM);
                        }

                        attr->la_gid = gid;
                }
        }

        RETURN(0);
}
