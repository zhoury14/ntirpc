/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    nfs41_op_lock.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:50 $
 * \version $Revision: 1.8 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs41_op_lock.c : Routines used for managing the NFS4 COMPOUND functions.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "HashData.h"
#include "HashTable.h"
#include "rpc.h"
#include "log_macros.h"
#include "stuff_alloc.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "sal_functions.h"
#include "nfs_proto_functions.h"

/**
 *
 * nfs41_op_lock: The NFS4_OP_LOCK operation.
 *
 * This function implements the NFS4_OP_LOCK operation.
 *
 * @param op    [IN]    pointer to nfs4_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs4_op results
 *
 * @return NFS4_OK if successfull, other values show an error.
 *
 * @see all the nfs41_op_<*> function
 * @see nfs4_Compound
 *
 */

#define arg_LOCK4 op->nfs_argop4_u.oplock
#define res_LOCK4 resp->nfs_resop4_u.oplock

int nfs41_op_lock(struct nfs_argop4 *op, compound_data_t * data, struct nfs_resop4 *resp)
{
  char __attribute__ ((__unused__)) funcname[] = "nfs41_op_lock";

  state_status_t            state_status;
  state_data_t              candidate_data;
  state_type_t              candidate_type;
  int                       rc = 0;
  state_t                 * plock_state;    /* state for the lock */
  state_t                 * pstate_open;    /* state for the open owner */
  state_t                 * pstate_previous_iterate;
  state_t                 * pstate_iterate;
  state_owner_t           * plock_owner;
  state_owner_t           * popen_owner;
  state_owner_t           * conflict_owner = NULL;
  state_nfs4_owner_name_t   owner_name;
  state_lock_desc_t         lock_desc, conflict_desc;
  state_blocking_t          blocking = STATE_NON_BLOCKING;

  /* Lock are not supported */
  resp->resop = NFS4_OP_LOCK;

#ifdef _WITH_NO_NFSV41_LOCKS
  res_LOCK4.status = NFS4ERR_LOCK_NOTSUPP;
  return res_LOCK4.status;
#else

  /* If there is no FH */
  if(nfs4_Is_Fh_Empty(&(data->currentFH)))
    {
      res_LOCK4.status = NFS4ERR_NOFILEHANDLE;
      return res_LOCK4.status;
    }

  /* If the filehandle is invalid */
  if(nfs4_Is_Fh_Invalid(&(data->currentFH)))
    {
      res_LOCK4.status = NFS4ERR_BADHANDLE;
      return res_LOCK4.status;
    }

  /* Tests if the Filehandle is expired (for volatile filehandle) */
  if(nfs4_Is_Fh_Expired(&(data->currentFH)))
    {
      res_LOCK4.status = NFS4ERR_FHEXPIRED;
      return res_LOCK4.status;
    }

  /* Commit is done only on a file */
  if(data->current_filetype != REGULAR_FILE)
    {
      /* Type of the entry is not correct */
      switch (data->current_filetype)
        {
        case DIR_BEGINNING:
        case DIR_CONTINUE:
          res_LOCK4.status = NFS4ERR_ISDIR;
          break;
        default:
          res_LOCK4.status = NFS4ERR_INVAL;
          break;
        }
    }

  /* Lock length should not be 0 */
  if(arg_LOCK4.length == 0LL)
    {
      res_LOCK4.status = NFS4ERR_INVAL;
      return res_LOCK4.status;
    }

  /* Convert lock parameters to internal types */
  switch(arg_LOCK4.locktype)
    {
      case READ_LT:
        lock_desc.sld_type = STATE_LOCK_R;
        blocking           = STATE_NON_BLOCKING;
        break;

      case WRITE_LT:
        lock_desc.sld_type = STATE_LOCK_W;
        blocking           = STATE_NON_BLOCKING;
        break;

      case READW_LT:
        lock_desc.sld_type = STATE_LOCK_R;
        blocking           = STATE_NFSV4_BLOCKING;
        break;

      case WRITEW_LT:
        lock_desc.sld_type = STATE_LOCK_W;
        blocking           = STATE_NFSV4_BLOCKING;
        break;
    }

  lock_desc.sld_offset = arg_LOCK4.offset;

  if(arg_LOCK4.length != STATE_LOCK_OFFSET_EOF)
    lock_desc.sld_length = arg_LOCK4.length;
  else
    lock_desc.sld_length = 0;

  /* Check for range overflow.
   * Comparing beyond 2^64 is not possible int 64 bits precision,
   * but off+len > 2^64-1 is equivalent to len > 2^64-1 - off
   */
  if(lock_desc.sld_length > (STATE_LOCK_OFFSET_EOF - lock_desc.sld_offset))
    {
      res_LOCK4.status = NFS4ERR_INVAL;
      return res_LOCK4.status;
    }

  if(arg_LOCK4.locker.new_lock_owner)
    {
      /* New lock owner
       * Find the open owner
       */
      if(state_get(arg_LOCK4.locker.locker4_u.open_owner.open_stateid.other,
                   &pstate_open,
                   data->pclient, &state_status) != STATE_SUCCESS)
        {
          res_LOCK4.status = NFS4ERR_STALE_STATEID;
          return res_LOCK4.status;
        }

      popen_owner = pstate_open->state_powner;
      plock_state = NULL;
      plock_owner = NULL;
    }
  else
    {
      /* Existing lock owner
       * Find the lock stateid
       * From that, get the open_owner
       */
      if(state_get(arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.other,
                   &plock_state,
                   data->pclient, &state_status) != STATE_SUCCESS)
        {
          /* Handle the case where all-0 stateid is used */
          if(!
             (!memcmp((char *)all_zero,
                      arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.other,
                      OTHERSIZE)
              && arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.seqid == 0))
            {
              if(state_status == STATE_NOT_FOUND)
                res_LOCK4.status = NFS4ERR_STALE_STATEID;
              else
                res_LOCK4.status = NFS4ERR_INVAL;

              return res_LOCK4.status;
            }
        }

      if(plock_state != NULL)
        {
          /* Get the old lockowner. We can do the following 'cast', in NFSv4 lock_owner4 and open_owner4
           * are different types but with the same definition*/
          plock_owner = plock_state->state_powner;
          popen_owner = plock_owner->so_owner.so_nfs4_owner.so_related_owner;
        }
      else
        {
          /* TODO FSF: this is an odd case, not sure we're using it yet... */
          plock_owner = NULL;
          popen_owner = NULL;
        }
    }                           /* if( arg_LOCK4.locker.new_lock_owner ) */

  /* Check for conflicts with previously obtained states */
  /* At this step of the code, if plock_state == NULL, then all-0 or all-1 stateid is used */

  /* TODO FSF:
   * This will eventually all go into the function of state_lock()
   * For now, we will keep checking against SHARE
   * Check against LOCK will be removed
   * We eventually need to handle special stateids, I don't think we'll ever see them on a real lock
   * call, but read and write need to get temporary lock, whether read/write is from NFS v2/3 or NFS v4.x
   */

  /* loop into the states related to this pentry to find the related lock */
  pstate_iterate = NULL;
  pstate_previous_iterate = NULL;
  do
    {
      state_iterate(data->current_entry,
                    &pstate_iterate,
                    pstate_previous_iterate,
                    data->pclient, data->pcontext, &state_status);
      if((state_status == STATE_STATE_ERROR)
         || (state_status == STATE_INVALID_ARGUMENT))
        {
          res_LOCK4.status = NFS4ERR_INVAL;
          return res_LOCK4.status;
        }

      if(pstate_iterate != NULL)
        {
          /* For now still check conflicts with SHARE here */
          if(pstate_iterate->state_type == STATE_TYPE_SHARE)
            {
              /* In a correct POSIX behavior, a write lock should not be allowed on a read-mode file */
              if((pstate_iterate->state_data.share.share_deny &
                   OPEN4_SHARE_DENY_WRITE) &&
                 !(pstate_iterate->state_data.share.share_access &
                   OPEN4_SHARE_ACCESS_WRITE) &&
                 (arg_LOCK4.locktype == WRITE_LT))
                {
                  /* A conflicting open state, return NFS4ERR_OPENMODE
                   * This behavior is implemented to comply with newpynfs's test LOCK4 */
                  res_LOCK4.status = NFS4ERR_OPENMODE;
                  return res_LOCK4.status;

                }
            }

        }                       /* if( pstate_iterate != NULL ) */
      pstate_previous_iterate = pstate_iterate;
    }
  while(pstate_iterate != NULL);

  /* TODO FSF:
   * Ok from here on out, stuff is broken...
   * For new lock owner, need to create a new stateid
   * And then call state_lock()
   * If that fails, need to back out any stateid changes
   * If that succeeds, need to increment seqids
   */
  if(arg_LOCK4.locker.new_lock_owner)
    {
      /* A lock owner is always associated with a previously made open
       * which has itself a previously made stateid */

      /* Check stateid correctness */
      if((rc = nfs4_Check_Stateid(&arg_LOCK4.locker.locker4_u.open_owner.open_stateid,
                                  data->current_entry,
                                  data->psession->clientid)) != NFS4_OK)
        {
          res_LOCK4.status = rc;
          return res_LOCK4.status;
        }

      /* An open state has been found. Check its type */
      if(pstate_open->state_type != STATE_TYPE_SHARE)
        {
          res_LOCK4.status = NFS4ERR_BAD_STATEID;
          return res_LOCK4.status;
        }

      /* Sanity check : Is this the right file ? */
      if(pstate_open->state_pentry != data->current_entry)
        {
          res_LOCK4.status = NFS4ERR_BAD_STATEID;
          return res_LOCK4.status;
        }

      /* Is this lock_owner known ? */
      if(!convert_nfs4_owner
         ((open_owner4 *) & arg_LOCK4.locker.locker4_u.open_owner.lock_owner,
          &owner_name))
        {
          res_LOCK4.status = NFS4ERR_SERVERFAULT;
          return res_LOCK4.status;
        }

      /* This lock owner is not known yet, allocated and set up a new one */
      plock_owner = create_nfs4_owner(data->pclient,
                                      &owner_name,
                                      (open_owner4 *) &arg_LOCK4.locker.locker4_u.open_owner.lock_owner,
                                      popen_owner,
                                      0);

      if(plock_owner == NULL)
        {
          res_LOCK4.status = NFS4ERR_SERVERFAULT;
          return res_LOCK4.status;
        }

      /* Prepare state management structure */
      candidate_type = STATE_TYPE_LOCK;
      candidate_data.lock.popenstate = pstate_open;

      /* Add the lock state to the lock table */
      if(state_add(data->current_entry,
                   candidate_type,
                   &candidate_data,
                   plock_owner,
                   data->pclient,
                   data->pcontext,
                   &plock_state, &state_status) != STATE_SUCCESS)
        {
          res_LOCK4.status = NFS4ERR_STALE_STATEID;
          return res_LOCK4.status;
        }

      /** @todo BUGAZOMEU: Manage the case if lock conflicts */
      res_LOCK4.LOCK4res_u.resok4.lock_stateid.seqid = 0;
      memcpy(res_LOCK4.LOCK4res_u.resok4.lock_stateid.other,
             plock_state->stateid_other,
             OTHERSIZE);

      /* update the lock counter in the related open-stateid */
      pstate_open->state_data.share.lockheld += 1;
    }
  else
    {
      /* The owner already exists, use the provided owner to create a new state */
      /* Get the former state */
      if(state_get(arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.other,
                   &plock_state,
                   data->pclient, &state_status) != STATE_SUCCESS)
        {
          res_LOCK4.status = NFS4ERR_STALE_STATEID;
          return res_LOCK4.status;
        }

      /* An lock state has been found. Check its type */
      if(plock_state->state_type != STATE_TYPE_LOCK)
        {
          res_LOCK4.status = NFS4ERR_BAD_STATEID;
          return res_LOCK4.status;
        }

      /* Sanity check : Is this the right file ? */
      if(plock_state->state_pentry != data->current_entry)
        {
          res_LOCK4.status = NFS4ERR_BAD_STATEID;
          return res_LOCK4.status;
        }

      memcpy(res_LOCK4.LOCK4res_u.resok4.lock_stateid.other,
             plock_state->stateid_other,
             OTHERSIZE);
    }                           /* if( arg_LOCK4.locker.new_lock_owner ) */

  /* Now we have a lock owner and a stateid.
   * Go ahead and push lock into SAL (and FSAL).
   */
  if(state_lock(data->current_entry,
                data->pcontext,
                plock_owner,
                plock_state,
                blocking,
                NULL,     /* No block data for now */
                &lock_desc,
                &conflict_owner,
                &conflict_desc,
                data->pclient,
                &state_status) != STATE_SUCCESS)
    {
      if(state_status == STATE_LOCK_CONFLICT)
        {
          /* A  conflicting lock from a different lock_owner, returns NFS4ERR_DENIED */
          res_LOCK4.LOCK4res_u.denied.offset = conflict_desc.sld_offset;
          res_LOCK4.LOCK4res_u.denied.length = conflict_desc.sld_length;

          if(conflict_desc.sld_type == STATE_LOCK_R)
            res_LOCK4.LOCK4res_u.denied.locktype = READ_LT;
          else
            res_LOCK4.LOCK4res_u.denied.locktype = WRITE_LT;

          res_LOCK4.LOCK4res_u.denied.owner.owner.owner_len =
            conflict_owner->so_owner_len;

          memcpy(res_LOCK4.LOCK4res_u.denied.owner.owner.owner_val,
                 conflict_owner->so_owner_val,
                 conflict_owner->so_owner_len);

          if(conflict_owner->so_type == STATE_LOCK_OWNER_NFSV4)
            res_LOCK4.LOCK4res_u.denied.owner.clientid =
              conflict_owner->so_owner.so_nfs4_owner.so_clientid;
          else
            res_LOCK4.LOCK4res_u.denied.owner.clientid = 0;
        }

      res_LOCK4.status = nfs4_Errno_state(state_status);
      return res_LOCK4.status;
    }
                
  res_LOCK4.status = NFS4_OK;
  return res_LOCK4.status;
#endif
}                               /* nfs41_op_lock */

/**
 * nfs41_op_lock_Free: frees what was allocared to handle nfs41_op_lock.
 *
 * Frees what was allocared to handle nfs41_op_lock.
 *
 * @param resp  [INOUT]    Pointer to nfs4_op results
 *
 * @return nothing (void function )
 *
 */
void nfs41_op_lock_Free(LOCK4res * resp)
{
  /* Nothing to Mem_Free */
  return;
}                               /* nfs41_op_lock_Free */