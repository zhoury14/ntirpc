/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

/**
 *
 * \file    fsal_init.c
 * \author  $Author: leibovic $
 * \date    $Date: 2006/01/24 13:45:37 $
 * \version $Revision: 1.20 $
 * \brief   Initialization functions.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fsal.h"
#include "fsal_internal.h"
#include "fsal_common.h"
#include <string.h>

extern size_t i_snapshots;
extern char **ppsz_snapshots;
extern int *pi_indexes;
extern libzfswrap_vfs_t **pp_vfs;
extern pthread_rwlock_t vfs_lock;

/* Macros for analysing parameters. */
#define SET_BITMAP_PARAM( api_cfg, p_init_info, _field )      \
    switch( (p_init_info)->behaviors._field ){                \
      case FSAL_INIT_FORCE_VALUE :                            \
        /* force the value in any case */                     \
        api_cfg._field = (p_init_info)->hpss_config._field;   \
        break;                                                \
      case FSAL_INIT_MAX_LIMIT :                              \
        /* remove the flags not specified by user (AND) */    \
        api_cfg._field &= (p_init_info)->hpss_config._field;  \
        break;                                                \
      case FSAL_INIT_MIN_LIMIT :                              \
        /* add the flags specified by user (OR) */            \
        api_cfg._field |= (p_init_info)->hpss_config._field;  \
        break;                                                \
    /* In the other cases, we keep the default value. */      \
    }                                                         \


#define SET_INTEGER_PARAM( api_cfg, p_init_info, _field )         \
    switch( (p_init_info)->behaviors._field ){                    \
    case FSAL_INIT_FORCE_VALUE :                                  \
        /* force the value in any case */                         \
        api_cfg._field = (p_init_info)->hpss_config._field;       \
        break;                                                \
    case FSAL_INIT_MAX_LIMIT :                                    \
      /* check the higher limit */                                \
      if ( api_cfg._field > (p_init_info)->hpss_config._field )   \
        api_cfg._field = (p_init_info)->hpss_config._field ;      \
        break;                                                \
    case FSAL_INIT_MIN_LIMIT :                                    \
      /* check the lower limit */                                 \
      if ( api_cfg._field < (p_init_info)->hpss_config._field )   \
        api_cfg._field = (p_init_info)->hpss_config._field ;      \
        break;                                                \
    /* In the other cases, we keep the default value. */          \
    }                                                             \


#define SET_STRING_PARAM( api_cfg, p_init_info, _field )          \
    switch( (p_init_info)->behaviors._field ){                    \
    case FSAL_INIT_FORCE_VALUE :                                  \
      /* force the value in any case */                           \
      strcpy(api_cfg._field,(p_init_info)->hpss_config._field);   \
      break;                                                \
    /* In the other cases, we keep the default value. */          \
    }                                                             \


size_t stack_size = 0;

/**
 * FSAL_Init : Initializes the FileSystem Abstraction Layer.
 *
 * \param init_info (input, fsal_parameter_t *) :
 *        Pointer to a structure that contains
 *        all initialization parameters for the FSAL.
 *        Specifically, it contains settings about
 *        the filesystem on which the FSAL is based,
 *        security settings, logging policy and outputs,
 *        and other general FSAL options.
 *
 * \return Major error codes :
 *         ERR_FSAL_NO_ERROR     (initialisation OK)
 *         ERR_FSAL_FAULT        (init_info pointer is null)
 *         ERR_FSAL_SERVERFAULT  (misc FSAL error)
 *         ERR_FSAL_ALREADY_INIT (The FS is already initialized)
 *         ERR_FSAL_BAD_INIT     (FS specific init error,
 *                                minor error code gives the reason
 *                                for this error.)
 *         ERR_FSAL_SEC_INIT     (Security context init error).
 */
fsal_status_t ZFSFSAL_Init(fsal_parameter_t * init_info    /* IN */
    )
{

  fsal_status_t status;

  /* sanity check.  */

  if(!init_info)
    Return(ERR_FSAL_FAULT, 0, INDEX_FSAL_Init);

  /* proceeds FSAL internal status initialization */

  status = fsal_internal_init_global(&(init_info->fsal_info),
                                     &(init_info->fs_common_info),
                                     &(init_info->fs_specific_info));

  if(FSAL_IS_ERROR(status))
    Return(status.major, status.minor, INDEX_FSAL_Init);

  /* Initilise the libzfswrap library */
  p_zhd = libzfswrap_init();
  if(!p_zhd)
  {
    LogCrit(COMPONENT_FSAL,"FSAL INIT: *** ERROR: Unable to initialize the libzfswrap library.");
    Return(ERR_FSAL_FAULT, 0, INDEX_FSAL_Init);
  }

  /* Mount the zpool */
  libzfswrap_vfs_t *p_vfs = libzfswrap_mount(init_info->fs_specific_info.psz_zpool, "/tank", "");
  if(!p_vfs)
  {
    LogMajor(COMPONENT_FSAL,"FSAL INIT: *** ERROR: Unable to mount the file system.");
    libzfswrap_exit(p_zhd);
    Return(ERR_FSAL_FAULT, 0, INDEX_FSAL_Init);
  }

  /* List the snapshots of the given zpool and mount them */
  const char *psz_error;
  i_snapshots = libzfswrap_zfs_get_list_snapshots(p_zhd, init_info->fs_specific_info.psz_zpool,
                                                  &ppsz_snapshots, &psz_error);

  if(i_snapshots != -1)
  {
    pp_vfs = calloc(i_snapshots + 1, sizeof(*pp_vfs));
    pp_vfs[0] = p_vfs;
    pi_indexes = calloc(i_snapshots + 1, sizeof(*pi_indexes));
    pi_indexes[0] = 0;

    int i,j;
    for(i = 0; i < i_snapshots; i++)
    {
      libzfswrap_vfs_t *p_snap_vfs = libzfswrap_mount(ppsz_snapshots[i], ppsz_snapshots[i], "");
      if(!p_snap_vfs)
      {
        LogMajor(COMPONENT_FSAL, "FSAL INIT: *** ERROR: Unable to mount the snapshot %s", ppsz_snapshots[i]);
        for(j = i; j >= 0; j--)
          libzfswrap_umount(pp_vfs[j], 1);
        free(pp_vfs);
        libzfswrap_exit(p_zhd);
        Return(ERR_FSAL_FAULT, 0, INDEX_FSAL_Init);
      }
      pp_vfs[i+1] = p_snap_vfs;
      pi_indexes[i+1] = i + 1;

      /* Change the name of the snapshot from zpool_name@snap_name to snap_name
         The '@' character is allways present, so no need to check it */
      char *psz_snap = strdup(strchr(ppsz_snapshots[i], '@') + 1);
      free(ppsz_snapshots[i]);
      ppsz_snapshots[i] = psz_snap;
    }
  }
  else
  {
    pp_vfs = malloc(sizeof(*pp_vfs));
    pp_vfs[0] = p_vfs;
    i_snapshots = 0;
  }
  pthread_rwlock_init(&vfs_lock, NULL);

  /* Everything went OK. */
  Return(ERR_FSAL_NO_ERROR, 0, INDEX_FSAL_Init);

}

/* To be called before exiting */
fsal_status_t ZFSFSAL_terminate()
{
  /* Unmount every snapshots and free the memory */
  int i;
  for(i = i_snapshots; i >= 0; i--)
    libzfswrap_umount(pp_vfs[i], 1);
  free(pp_vfs);
  free(pi_indexes);

  for(i = 0; i < i_snapshots; i++)
    free(ppsz_snapshots[i]);
  free(ppsz_snapshots);

  pthread_rwlock_destroy(&vfs_lock);

  libzfswrap_exit(p_zhd);
  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}