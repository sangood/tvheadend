/*
 *  tvheadend - API access to MPEGTS system
 *
 *  Copyright (C) 2013 Adam Sutton
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "access.h"
#include "htsmsg.h"
#include "api.h"
#include "input/mpegts.h"
#if ENABLE_LINUXDVB
#include "input/mpegts/linuxdvb.h"
#include "input/mpegts/linuxdvb/linuxdvb_private.h"
#endif

/*
 * Inputs
 */
static int
api_mpegts_input_network_list
  ( void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  int i, err = EINVAL;
  const char *uuid;
  mpegts_input_t *mi;
  mpegts_network_t *mn;
  idnode_set_t *is;
  extern const idclass_t mpegts_input_class;

  if (!(uuid = htsmsg_get_str(args, "uuid")))
    return EINVAL;

  pthread_mutex_lock(&global_lock);

  mi = mpegts_input_find(uuid);
  if (!mi)
    goto exit;

  htsmsg_t     *l  = htsmsg_create_list();
  if ((is = mi->mi_network_list(mi))) {
    for (i = 0; i < is->is_count; i++) {
      char buf[256];
      htsmsg_t *e = htsmsg_create_map();
      mn = (mpegts_network_t*)is->is_array[i];
      htsmsg_add_str(e, "key", idnode_uuid_as_str(is->is_array[i]));
      mn->mn_display_name(mn, buf, sizeof(buf));
      htsmsg_add_str(e, "val", buf);
      htsmsg_add_msg(l, NULL, e);
    }
    idnode_set_free(is);
  }
  err   = 0;
  *resp = htsmsg_create_map();
  htsmsg_add_msg(*resp, "entries", l);

exit:
  pthread_mutex_unlock(&global_lock);

  return err;
}

/*
 * Networks
 */
static void
api_mpegts_network_grid
  ( idnode_set_t *ins, api_idnode_grid_conf_t *conf )
{
  mpegts_network_t *mn;

  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link) {
    idnode_set_add(ins, (idnode_t*)mn, &conf->filter);
  }
}

static int
api_mpegts_network_builders
  ( void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  mpegts_network_builder_t *mnb;
  htsmsg_t *l, *e;

  /* List of available builder classes */
  l = htsmsg_create_list();
  LIST_FOREACH(mnb, &mpegts_network_builders, link)
    if ((e = idclass_serialize(mnb->idc)))
      htsmsg_add_msg(l, NULL, e);

  /* Output */
  *resp = htsmsg_create_map();  
  htsmsg_add_msg(*resp, "entries", l);

  return 0;
}

static int
api_mpegts_network_create
  ( void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  int err;
  const char *class;
  htsmsg_t *conf;
  mpegts_network_t *mn;

  if (!(class = htsmsg_get_str(args, "class")))
    return EINVAL;
  if (!(conf  = htsmsg_get_map(args, "conf")))
    return EINVAL;

  pthread_mutex_lock(&global_lock);
  mn = mpegts_network_build(class, conf);
  if (mn) {
    err = 0;
    *resp = htsmsg_create_map();
    mn->mn_config_save(mn);
  } else {
    err = EINVAL;
  }
  pthread_mutex_unlock(&global_lock);

  return err;
}

static int
api_mpegts_network_muxclass
  ( void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  int err = EINVAL;
  const idclass_t *idc; 
  mpegts_network_t *mn;
  const char *uuid;

  if (!(uuid = htsmsg_get_str(args, "uuid")))
    return EINVAL;
  
  pthread_mutex_lock(&global_lock);
  
  if (!(mn  = mpegts_network_find(uuid)))
    goto exit;

  if (!(idc = mn->mn_mux_class(mn)))
    goto exit;

  *resp = idclass_serialize(idc);
  err    = 0;

exit:
  pthread_mutex_unlock(&global_lock);
  return err;
}

static int
api_mpegts_network_muxcreate
  ( void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  int err = EINVAL;
  mpegts_network_t *mn;
  mpegts_mux_t *mm;
  htsmsg_t *conf;
  const char *uuid;

  if (!(uuid = htsmsg_get_str(args, "uuid")))
    return EINVAL;
  if (!(conf = htsmsg_get_map(args, "conf")))
    return EINVAL;
  
  pthread_mutex_lock(&global_lock);
  
  if (!(mn  = mpegts_network_find(uuid)))
    goto exit;
  
  if (!(mm = mn->mn_mux_create2(mn, conf)))
    goto exit;

  mm->mm_config_save(mm);
  err = 0;

exit:
  pthread_mutex_unlock(&global_lock);
  return err;
}

/*
 * Muxes
 */
static void
api_mpegts_mux_grid
  ( idnode_set_t *ins, api_idnode_grid_conf_t *conf )
{
  mpegts_network_t *mn;
  mpegts_mux_t *mm;

  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link) {
    LIST_FOREACH(mm, &mn->mn_muxes, mm_network_link) {
      idnode_set_add(ins, (idnode_t*)mm, &conf->filter);
    }
  }
}

/*
 * Services
 */
static void
api_mpegts_service_grid
  ( idnode_set_t *ins, api_idnode_grid_conf_t *conf )
{
  mpegts_network_t *mn;
  mpegts_mux_t *mm;
  mpegts_service_t *ms;

  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link) {
    LIST_FOREACH(mm, &mn->mn_muxes, mm_network_link) {
      LIST_FOREACH(ms, &mm->mm_services, s_dvb_mux_link) {
        idnode_set_add(ins, (idnode_t*)ms, &conf->filter);
      }
    }
  }
}

/*
 * Satconfs
 */
#if ENABLE_LINUXDVB
static void
api_linuxdvb_satconf_grid
  ( idnode_set_t *ins, api_idnode_grid_conf_t *conf )
{
  mpegts_input_t *mi;
  extern const idclass_t linuxdvb_satconf_class;

  LIST_FOREACH(mi, &mpegts_input_all, mi_global_link)
    if (idnode_is_instance((idnode_t*)mi, &linuxdvb_satconf_class))
      idnode_set_add(ins, (idnode_t*)mi, &conf->filter);
}

static int
api_linuxdvb_satconf_create
  ( void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  int err;
  htsmsg_t *conf;
  idnode_t *in;

  if (!(conf  = htsmsg_get_map(args, "conf")))
    return -EINVAL;

  pthread_mutex_lock(&global_lock);
  in = (idnode_t*)linuxdvb_satconf_create0(NULL, conf);
  if (in) {
    err = 0;
    in->in_class->ic_save(in);
    *resp = htsmsg_create_map();
  } else {
    err = -EINVAL;
  }
  pthread_mutex_unlock(&global_lock);

  return err;
}
#endif

/*
 * Adapter list
 *
 * TODO: this will need reworking for mpegps etc...
 */
static idnode_set_t *
api_tvadapter_tree ( void )
{
#if ENABLE_LINUXDVB
  return linuxdvb_root();
#else
  return NULL;
#endif
}

/*
 * Init
 */
void
api_mpegts_init ( void )
{
  extern const idclass_t mpegts_network_class;
  extern const idclass_t mpegts_mux_class;
  extern const idclass_t mpegts_service_class;
  extern const idclass_t linuxdvb_satconf_class;

  static api_hook_t ah[] = {
    { "tvadapter/tree",            ACCESS_ANONYMOUS, api_idnode_tree,  api_tvadapter_tree },
    { "mpegts/input/network_list", ACCESS_ANONYMOUS, api_mpegts_input_network_list, NULL },
    { "mpegts/network/grid",       ACCESS_ANONYMOUS, api_idnode_grid,  api_mpegts_network_grid },
    { "mpegts/network/class",      ACCESS_ANONYMOUS, api_idnode_class, (void*)&mpegts_network_class },
    { "mpegts/network/builders",   ACCESS_ANONYMOUS, api_mpegts_network_builders, NULL },
    { "mpegts/network/create",     ACCESS_ANONYMOUS, api_mpegts_network_create,   NULL },
    { "mpegts/network/mux_class",  ACCESS_ANONYMOUS, api_mpegts_network_muxclass, NULL },
    { "mpegts/network/mux_create", ACCESS_ANONYMOUS, api_mpegts_network_muxcreate, NULL },
    { "mpegts/mux/grid",           ACCESS_ANONYMOUS, api_idnode_grid,  api_mpegts_mux_grid },
    { "mpegts/mux/class",          ACCESS_ANONYMOUS, api_idnode_class, (void*)&mpegts_mux_class },
    { "mpegts/service/grid",       ACCESS_ANONYMOUS, api_idnode_grid,  api_mpegts_service_grid },
    { "mpegts/service/class",      ACCESS_ANONYMOUS, api_idnode_class, (void*)&mpegts_service_class },
#if ENABLE_LINUXDVB
    { "linuxdvb/satconf/grid",     ACCESS_ANONYMOUS, api_idnode_grid,  api_linuxdvb_satconf_grid },
    { "linuxdvb/satconf/class",    ACCESS_ANONYMOUS, api_idnode_class, (void*)&linuxdvb_satconf_class },
    { "linuxdvb/satconf/create",   ACCESS_ANONYMOUS, api_linuxdvb_satconf_create, NULL },
#endif
    { NULL },
  };

  api_register_all(ah);
}
