/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2003 Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 */

#ifndef LLITE_INTERNAL_H
#define LLITE_INTERNAL_H

struct ll_sb_info;

extern void lprocfs_unregister_mountpoint(struct ll_sb_info *sbi);
extern struct proc_dir_entry *proc_lustre_fs_root;

struct lustre_handle;
struct lov_stripe_md;
struct ll_sb_info;


/* llite/commit_callback.c */
int ll_commitcbd_setup(struct ll_sb_info *);
int ll_commitcbd_cleanup(struct ll_sb_info *);

/* lproc_llite.c */
int lprocfs_register_mountpoint(struct proc_dir_entry *parent,
                                struct super_block *sb, char *osc, char *mdc);
void lprocfs_unregister_mountpoint(struct ll_sb_info *sbi);

/* llite/namei.c */
struct dentry *ll_find_alias(struct inode *, struct dentry *);
int ll_it_open_error(int phase, struct lookup_intent *it);
int ll_mdc_cancel_unused(struct lustre_handle *conn, struct inode *inode,
                         int flags, void *opaque);

/* llite/rw.c */
void ll_remove_dirty(struct inode *inode, unsigned long start,
                     unsigned long end);
int ll_rd_dirty_pages(char *page, char **start, off_t off, int count,
                      int *eof, void *data);
int ll_rd_max_dirty_pages(char *page, char **start, off_t off, int count,
                          int *eof, void *data);
int ll_wr_max_dirty_pages(struct file *file, const char *buffer,
                          unsigned long count, void *data);

extern struct super_operations ll_super_operations;

/* llite/super.c */
int ll_inode_setattr(struct inode *inode, struct iattr *attr, int do_trunc);
int ll_setattr(struct dentry *de, struct iattr *attr);

/* llite/dcache.c */
void ll_set_dd(struct dentry *);

#endif /* LLITE_INTERNAL_H */
