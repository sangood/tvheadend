/*
rk_class
 *  Tvheadend - Linux DVB frontend
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
#include "linuxdvb_private.h"
#include "notify.h"
#include "atomic.h"
#include "tvhpoll.h"

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <linux/dvb/dmx.h>

static void
linuxdvb_frontend_monitor ( void *aux );
static void *
linuxdvb_frontend_input_thread ( void *aux );

/* **************************************************************************
 * Class definition
 * *************************************************************************/

extern const idclass_t linuxdvb_hardware_class;

static void
linuxdvb_frontend_class_save ( idnode_t *in )
{
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)in;
  if (lfe->lh_parent && lfe->lh_parent->lh_parent)
    linuxdvb_device_save((linuxdvb_device_t*)lfe->lh_parent->lh_parent);
}

static const void*
linuxdvb_frontend_class_network_get(void *o)
{
  static const char *s;
  linuxdvb_frontend_t *lfe = o;
  if (lfe->mi_network)
    s = idnode_uuid_as_str(&lfe->mi_network->mn_id);
  else
    s = NULL;
  return &s;
}

static int
linuxdvb_frontend_class_network_set(void *o, const void *v)
{
  mpegts_input_t   *mi = o;
  mpegts_network_t *mn = mi->mi_network;
  linuxdvb_network_t *ln = (linuxdvb_network_t*)mn;
  linuxdvb_frontend_t *lfe = o;
  const char *s = v;

  if (lfe->lfe_info.type == FE_QPSK) {
    tvherror("linuxdvb", "cannot set network on DVB-S FE");
    return 0;
  }

  if (mi->mi_network && !strcmp(idnode_uuid_as_str(&mn->mn_id), s ?: ""))
    return 0;

  if (ln && ln->ln_type != lfe->lfe_info.type) {
    tvherror("linuxdvb", "attempt to set network of wrong type");
    return 0;
  }

  mpegts_input_set_network(mi, s ? mpegts_network_find(s) : NULL);
  return 1;
}

static htsmsg_t *
linuxdvb_frontend_class_network_enum(void *o)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_t *p = htsmsg_create_map();
  htsmsg_add_str(m, "type",  "api");
  htsmsg_add_str(m, "uri",   "mpegts/input/network_list");
  htsmsg_add_str(p, "uuid",  idnode_uuid_as_str((idnode_t*)o));
  htsmsg_add_str(m, "event", "mpegts_network");
  htsmsg_add_msg(m, "params", p);

  return m;
}

const idclass_t linuxdvb_frontend_class =
{
  .ic_super      = &linuxdvb_hardware_class,
  .ic_class      = "linuxdvb_frontend",
  .ic_caption    = "Linux DVB Frontend",
  .ic_save       = linuxdvb_frontend_class_save,
  .ic_properties = (const property_t[]) {
    {
      .type     = PT_STR,
      .id       = "fe_path",
      .name     = "Frontend Path",
      .opts     = PO_RDONLY,
      .off      = offsetof(linuxdvb_frontend_t, lfe_fe_path),
    },
    {
      .type     = PT_STR,
      .id       = "dvr_path",
      .name     = "Input Path",
      .opts     = PO_RDONLY,
      .off      = offsetof(linuxdvb_frontend_t, lfe_dvr_path),
    },
    {
      .type     = PT_STR,
      .id       = "dmx_path",
      .name     = "Demux Path",
      .opts     = PO_RDONLY,
      .off      = offsetof(linuxdvb_frontend_t, lfe_dmx_path),
    },
    {
      .type     = PT_INT,
      .id       = "fe_number",
      .name     = "Frontend Number",
      .opts     = PO_RDONLY,
      .off      = offsetof(linuxdvb_frontend_t, lfe_number),
    },
    {
      .type     = PT_BOOL,
      .id       = "fullmux",
      .name     = "Full Mux RX mode",
      .off      = offsetof(linuxdvb_frontend_t, lfe_fullmux),
    },
    {}
  }
};

const idclass_t linuxdvb_frontend_dvbt_class =
{
  .ic_super      = &linuxdvb_frontend_class,
  .ic_class      = "linuxdvb_frontend_dvbt",
  .ic_caption    = "Linux DVB-T Frontend",
  .ic_properties = (const property_t[]){
    {
      .type     = PT_STR,
      .id       = "network",
      .name     = "Network",
      .get      = linuxdvb_frontend_class_network_get,
      .set      = linuxdvb_frontend_class_network_set,
      .list     = linuxdvb_frontend_class_network_enum
    },
    {}
  }
};

const idclass_t linuxdvb_frontend_dvbs_class =
{
  .ic_super      = &linuxdvb_frontend_class,
  .ic_class      = "linuxdvb_frontend_dvbs",
  .ic_caption    = "Linux DVB-S Frontend",
  .ic_properties = (const property_t[]){
    {}
  }
};

const idclass_t linuxdvb_frontend_dvbc_class =
{
  .ic_super      = &linuxdvb_frontend_class,
  .ic_class      = "linuxdvb_frontend_dvbc",
  .ic_caption    = "Linux DVB-C Frontend",
  .ic_properties = (const property_t[]){
    {
      .type     = PT_STR,
      .id       = "network",
      .name     = "Network",
      .get      = linuxdvb_frontend_class_network_get,
      .set      = linuxdvb_frontend_class_network_set,
      .list     = linuxdvb_frontend_class_network_enum
    },
    {}
  }
};

const idclass_t linuxdvb_frontend_atsc_class =
{
  .ic_super      = &linuxdvb_frontend_class,
  .ic_class      = "linuxdvb_frontend_atsc",
  .ic_caption    = "Linux ATSC Frontend",
  .ic_properties = (const property_t[]){
    {
      .type     = PT_STR,
      .id       = "network",
      .name     = "Network",
      .get      = linuxdvb_frontend_class_network_get,
      .set      = linuxdvb_frontend_class_network_set,
      .list     = linuxdvb_frontend_class_network_enum
    },
    {}
  }
};

/* **************************************************************************
 * Class methods
 * *************************************************************************/

static int
linuxdvb_frontend_is_enabled ( mpegts_input_t *mi )
{
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;
  if (lfe->lfe_fe_path == NULL) return 0;
  if (!lfe->mi_enabled) return 0;
  if (access(lfe->lfe_fe_path, R_OK | W_OK)) return 0;
  return 1;
}

#if 0
static int
linuxdvb_frontend_is_free ( mpegts_input_t *mi )
{
#if 0
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;
  linuxdvb_adapter_t  *la =  lfe->lfe_adapter;
  return linuxdvb_adapter_is_free(la);
#endif
  return 0;
}

static int
linuxdvb_frontend_current_weight ( mpegts_input_t *mi )
{
#if 0
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;
  linuxdvb_adapter_t  *la =  lfe->lfe_adapter;
  return linuxdvb_adapter_current_weight(la);
#endif
  return 0;
}
#endif

static void
linuxdvb_frontend_stop_mux
  ( mpegts_input_t *mi, mpegts_mux_instance_t *mmi )
{
  char buf1[256], buf2[256];
  
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;
  mi->mi_display_name(mi, buf1, sizeof(buf1));
  mmi->mmi_mux->mm_display_name(mmi->mmi_mux, buf2, sizeof(buf2));
  tvhdebug("linuxdvb", "%s - stopping %s", buf1, buf2);

  /* Stop thread */
  if (lfe->lfe_dvr_pipe.wr > 0) {
    tvh_write(lfe->lfe_dvr_pipe.wr, "", 1);
    tvhtrace("linuxdvb", "%s - waiting for dvr thread", buf1);
    pthread_join(lfe->lfe_dvr_thread, NULL);
    tvh_pipe_close(&lfe->lfe_dvr_pipe);
    tvhdebug("linuxdvb", "%s - stopped dvr thread", buf1);
  }

  /* Not locked */
  lfe->lfe_locked = 0;
}

static int
linuxdvb_frontend_start_mux
  ( mpegts_input_t *mi, mpegts_mux_instance_t *mmi )
{
  return linuxdvb_frontend_tune1((linuxdvb_frontend_t*)mi, mmi, -1);
}

static int
linuxdvb_frontend_open_pid
  ( linuxdvb_frontend_t *lfe, int pid, const char *name )
{
  char buf[256];
  struct dmx_pes_filter_params dmx_param;
  int fd = tvh_open(lfe->lfe_dmx_path, O_RDWR, 0);

  if (!name) {
    lfe->mi_display_name((mpegts_input_t*)lfe, buf, sizeof(buf));
    name = buf;
  }

  if(fd == -1) {
    tvherror("linuxdvb", "%s - failed to open dmx for pid %d [e=%s]",
             name, pid, strerror(errno));
    return -1;
  }

  tvhtrace("linuxdvb", "%s - open PID %04X (%d)", name, pid, pid);
  memset(&dmx_param, 0, sizeof(dmx_param));
  dmx_param.pid      = pid;
  dmx_param.input    = DMX_IN_FRONTEND;
  dmx_param.output   = DMX_OUT_TS_TAP;
  dmx_param.pes_type = DMX_PES_OTHER;
  dmx_param.flags    = DMX_IMMEDIATE_START;

  if(ioctl(fd, DMX_SET_PES_FILTER, &dmx_param)) {
    tvherror("linuxdvb", "%s - failed to config dmx for pid %d [e=%s]",
             name, pid, strerror(errno));
    close(fd);
    return -1;
  }

  return fd;
}

static void
linuxdvb_frontend_open_service
  ( mpegts_input_t *mi, mpegts_service_t *s, int init )
{
  char buf[256];
  elementary_stream_t *st;
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;

  /* Ignore in full rx mode OR if not yet locked */
  if (!lfe->lfe_locked || lfe->lfe_fullmux) goto exit;
  mi->mi_display_name(mi, buf, sizeof(buf));
  
  /* Install PES filters */
  TAILQ_FOREACH(st, &s->s_components, es_link) {
    if(st->es_pid >= 0x2000)
      continue;

    if(st->es_demuxer_fd != -1)
      continue;

    st->es_cc_valid   = 0;
    st->es_demuxer_fd
      = linuxdvb_frontend_open_pid((linuxdvb_frontend_t*)mi, st->es_pid, buf);
  }

exit:
  mpegts_input_open_service(mi, s, init);
}

static void
linuxdvb_frontend_close_service
  ( mpegts_input_t *mi, mpegts_service_t *s )
{
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;

  /* Ignore in full rx mode OR if not yet locked */
  if (!lfe->lfe_locked || lfe->lfe_fullmux) goto exit;

exit:
  mpegts_input_close_service(mi, s);
}

static idnode_set_t *
linuxdvb_frontend_network_list ( mpegts_input_t *mi )
{
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;
  const idclass_t     *idc;
  extern const idclass_t linuxdvb_network_dvbt_class;
  extern const idclass_t linuxdvb_network_dvbc_class;
  extern const idclass_t linuxdvb_network_dvbs_class;
  extern const idclass_t linuxdvb_network_atsc_class;

  if (lfe->lfe_info.type == FE_OFDM)
    idc = &linuxdvb_network_dvbt_class;
  else if (lfe->lfe_info.type == FE_QAM)
    idc = &linuxdvb_network_dvbc_class;
  else if (lfe->lfe_info.type == FE_QPSK)
    idc = &linuxdvb_network_dvbs_class;
  else if (lfe->lfe_info.type == FE_ATSC)
    idc = &linuxdvb_network_atsc_class;
  else
    return NULL;

  return idnode_find_all(idc);
}

/* **************************************************************************
 * Data processing
 * *************************************************************************/

static void
linuxdvb_frontend_default_tables 
  ( linuxdvb_frontend_t *lfe, linuxdvb_mux_t *lm )
{
  mpegts_mux_t *mm = (mpegts_mux_t*)lm;

  /* Common */
  mpegts_table_add(mm, DVB_PAT_BASE, DVB_PAT_MASK, dvb_pat_callback,
                   NULL, "pat", MT_QUICKREQ | MT_CRC, DVB_PAT_PID);
#if 0
  mpegts_table_add(mm, DVB_CAT_BASE, DVB_CAT_MASK, dvb_cat_callback,
                   NULL, "cat", MT_CRC, DVB_CAT_PID);
#endif

  /* ATSC */
  if (lfe->lfe_info.type == FE_ATSC) {
#if 0
    int tableid;
    if (lc->lm_tuning.dmc_fe_params.u.vsb.modulation == VSB_8)
      tableid = ATSC_VCT_TERR;
    else
      tableid = ATSC_VCT_CAB;
    mpegts_table_add(mm, tableid, 0xff, atsc_vct_callback,
                     NULL, "vct", MT_QUICKREQ | MT_CRC, ATSC_VCT_PID);
#endif

  /* DVB */
  } else {
    mpegts_table_add(mm, DVB_CAT_BASE, DVB_CAT_MASK, dvb_cat_callback,
                     NULL, "cat", MT_QUICKREQ | MT_CRC, DVB_CAT_PID);
    mpegts_table_add(mm, DVB_NIT_BASE, DVB_NIT_MASK, dvb_nit_callback,
                     NULL, "nit", MT_QUICKREQ | MT_CRC, DVB_NIT_PID);
    mpegts_table_add(mm, DVB_SDT_BASE, DVB_SDT_MASK, dvb_sdt_callback,
                     NULL, "sdt", MT_QUICKREQ | MT_CRC, DVB_SDT_PID);
    mpegts_table_add(mm, DVB_BAT_BASE, DVB_BAT_MASK, dvb_bat_callback,
                     NULL, "bat", MT_CRC, DVB_BAT_PID);
#if 0
    mpegts_table_add(mm, DVB_TOT_BASE, DVB_TOT_MASK, dvb_tot_callback,
                     NULL, "tot", MT_CRC, DVB_TOT_PID);
#endif
  }
}

static void
linuxdvb_frontend_open_services ( linuxdvb_frontend_t *lfe )
{
  service_t *s;
  LIST_FOREACH(s, &lfe->mi_transports, s_active_link) {
    linuxdvb_frontend_open_service((mpegts_input_t*)lfe,
                                   (mpegts_service_t*)s, 0);
  }
}

static void
linuxdvb_frontend_monitor_stats ( linuxdvb_frontend_t *lfe, const char *name )
{
  int bw;
  htsmsg_t *m, *l, *e;
  mpegts_mux_instance_t *mmi;

  /* Send message */
  m = htsmsg_create_map();
  htsmsg_add_str(m, "uuid", idnode_uuid_as_str(&lfe->mi_id));
  htsmsg_add_str(m, "name", name);
  htsmsg_add_str(m, "type", "linuxdvb");
  
  /* Mux list */
  if ((mmi = LIST_FIRST(&lfe->mi_mux_active))) {
    char buf[256];
    l = htsmsg_create_list();
    e = htsmsg_create_map();
    mmi->mmi_mux->mm_display_name(mmi->mmi_mux, buf, sizeof(buf));
    htsmsg_add_str(e, "name", buf);
    htsmsg_add_u32(e, "bytes", 0); // TODO 
    // TODO: signal info
    htsmsg_add_msg(l, NULL, e);
    htsmsg_add_msg(m, "muxes", l);
  }

  /* Total data */
  bw = atomic_exchange(&lfe->mi_bytes, 0);
  htsmsg_add_u32(m, "bytes", bw);

  notify_by_msg("input", m);
}

static void
linuxdvb_frontend_monitor ( void *aux )
{
  char buf[256];
  linuxdvb_frontend_t *lfe = aux;
  mpegts_mux_instance_t *mmi = LIST_FIRST(&lfe->mi_mux_active);
  mpegts_mux_t *mm;
  fe_status_t fe_status;
  signal_state_t status;

  lfe->mi_display_name((mpegts_input_t*)lfe, buf, sizeof(buf));
  tvhtrace("linuxdvb", "%s - checking FE status", buf);

  /* Check accessibility */
  if (lfe->lfe_fe_fd <= 0) {
    if (lfe->lfe_fe_path && access(lfe->lfe_fe_path, R_OK | W_OK)) {
      tvherror("linuxdvb", "%s - device is not accessible", buf);
      // TODO: disable device
      return;
    }
  }

  /* Get current status */
  if (ioctl(lfe->lfe_fe_fd, FE_READ_STATUS, &fe_status) == -1) {
    tvhwarn("linuxdvb", "%s - FE_READ_STATUS error %s", buf, strerror(errno));
    /* TODO: check error value */
    return;

  } else if (fe_status & FE_HAS_LOCK)
    status = SIGNAL_GOOD;
  else if (fe_status & (FE_HAS_SYNC | FE_HAS_VITERBI | FE_HAS_CARRIER))
    status = SIGNAL_BAD;
  else if (fe_status & FE_HAS_SIGNAL)
    status = SIGNAL_FAINT;
  else
    status = SIGNAL_NONE;

  /* Set default period */
  gtimer_arm(&lfe->lfe_monitor_timer, linuxdvb_frontend_monitor, lfe, 1);
  tvhtrace("linuxdvb", "%s - status %d", buf, status);

  /* Get current mux */
  if (!mmi) return;
  mm = mmi->mmi_mux;

  /* Waiting for lock */
  if (!lfe->lfe_locked) {

    /* Locked */
    if (status == SIGNAL_GOOD) {
      tvhdebug("linuxdvb", "%s - locked", buf);
      lfe->lfe_locked = 1;
  
      /* Start input */
      tvh_pipe(O_NONBLOCK, &lfe->lfe_dvr_pipe);
      pthread_mutex_lock(&lfe->lfe_dvr_lock);
      pthread_create(&lfe->lfe_dvr_thread, NULL,
                     linuxdvb_frontend_input_thread, lfe);
      pthread_cond_wait(&lfe->lfe_dvr_cond, &lfe->lfe_dvr_lock);
      pthread_mutex_unlock(&lfe->lfe_dvr_lock);

      /* Table handlers */
      linuxdvb_frontend_default_tables(lfe, (linuxdvb_mux_t*)mm);

      /* Services */
      linuxdvb_frontend_open_services(lfe);

    /* Re-arm (quick) */
    } else {
      gtimer_arm_ms(&lfe->lfe_monitor_timer, linuxdvb_frontend_monitor,
                    lfe, 50);

      /* Monitor 1 per sec */
      if (dispatch_clock < lfe->lfe_monitor)
        return;
      lfe->lfe_monitor = dispatch_clock + 1;
    }
  }

  /* Monitor stats */
  linuxdvb_frontend_monitor_stats(lfe, buf);
}

static void *
linuxdvb_frontend_input_thread ( void *aux )
{
  linuxdvb_frontend_t *lfe = aux;
  mpegts_mux_instance_t *mmi;
  int dmx = -1, dvr = -1;
  char buf[256];
  uint8_t tsb[18800];
  int pos = 0, nfds;
  ssize_t c;
  tvhpoll_event_t ev[2];
  struct dmx_pes_filter_params dmx_param;
  int fullmux;
  tvhpoll_t *efd;

  /* Get MMI */
  pthread_mutex_lock(&lfe->lfe_dvr_lock);
  lfe->mi_display_name((mpegts_input_t*)lfe, buf, sizeof(buf));
  mmi = LIST_FIRST(&lfe->mi_mux_active);
  fullmux = lfe->lfe_fullmux;
  pthread_cond_signal(&lfe->lfe_dvr_cond);
  pthread_mutex_unlock(&lfe->lfe_dvr_lock);
  if (mmi == NULL) return NULL;

  /* Open DMX */
  if (fullmux) {
    dmx = tvh_open(lfe->lfe_dmx_path, O_RDWR, 0);
    if (dmx < 0) {
      tvherror("linuxdvb", "%s - failed to open %s", buf, lfe->lfe_dmx_path);
      return NULL;
    }
    memset(&dmx_param, 0, sizeof(dmx_param));
    dmx_param.pid      = 0x2000;
    dmx_param.input    = DMX_IN_FRONTEND;
    dmx_param.output   = DMX_OUT_TS_TAP;
    dmx_param.pes_type = DMX_PES_OTHER;
    dmx_param.flags    = DMX_IMMEDIATE_START;
    if(ioctl(dmx, DMX_SET_PES_FILTER, &dmx_param) == -1) {
      tvherror("linuxdvb", "%s - open raw filter failed [e=%s]",
               buf, strerror(errno));
      close(dmx);
      return NULL;
    }
  }

  /* Open DVR */
  dvr = tvh_open(lfe->lfe_dvr_path, O_RDONLY | O_NONBLOCK, 0);
  if (dvr < 0) {
    close(dmx);
    tvherror("linuxdvb", "%s - failed to open %s", buf, lfe->lfe_dvr_path);
    return NULL;
  }

  /* Setup poll */
  efd = tvhpoll_create(2);
  memset(ev, 0, sizeof(ev));
  ev[0].events  = TVHPOLL_IN;
  ev[0].fd      = dvr;
  ev[1].events  = TVHPOLL_IN;
  ev[1].fd      = lfe->lfe_dvr_pipe.rd;
  tvhpoll_add(efd, ev, 2);

  /* Read */
  while (1) {
    nfds = tvhpoll_wait(efd, ev, 1, 10);
    if (nfds < 1) continue;
    if (ev[0].data.fd != dvr) break;
    
    /* Read */
    c = read(dvr, tsb+pos, sizeof(tsb)-pos);
    if (c < 0) {
      if ((errno == EAGAIN) || (errno == EINTR))
        continue;
      if (errno == EOVERFLOW) {
        tvhlog(LOG_WARNING, "linuxdvb", "%s - read() EOVERFLOW", buf);
        continue;
      }
      tvhlog(LOG_ERR, "linuxdvb", "%s - read() error %d (%s)",
             buf, errno, strerror(errno));
      break;
    }
    
    /* Process */
    pos = mpegts_input_recv_packets((mpegts_input_t*)lfe, mmi, tsb, c+pos,
                                    NULL, NULL, buf);
  }

  tvhpoll_destroy(efd);
  if (dmx != -1) close(dmx);
  close(dvr);
  return NULL;
}

/* **************************************************************************
 * Tuning
 * *************************************************************************/

int
linuxdvb_frontend_tune0
  ( linuxdvb_frontend_t *lfe, mpegts_mux_instance_t *mmi, uint32_t freq )
{
  int r;
  struct dvb_frontend_event ev;
  char buf1[256];
  mpegts_mux_instance_t *cur = LIST_FIRST(&lfe->mi_mux_active);
  linuxdvb_mux_t *lm = (linuxdvb_mux_t*)mmi->mmi_mux;

  // Not sure if this is right place?
  /* Currently active */
  if (cur != NULL) {

    /* Already tuned */
    if (mmi == cur)
      return 0;

    /* Stop current */
    cur->mmi_mux->mm_stop(cur->mmi_mux);
  }
  assert(LIST_FIRST(&lfe->mi_mux_active) == NULL);

  /* Open FE */
  if (lfe->lfe_fe_fd <= 0) {
    lfe->mi_display_name((mpegts_input_t*)lfe, buf1, sizeof(buf1));
    lfe->lfe_fe_fd = tvh_open(lfe->lfe_fe_path, O_RDWR | O_NONBLOCK, 0);
    tvhtrace("linuxdvb", "%s - opening FE %s (%d)", buf1, lfe->lfe_fe_path, lfe->lfe_fe_fd);
    if (lfe->lfe_fe_fd <= 0) {
      return SM_CODE_TUNING_FAILED;
    }
  }

  /* S2 tuning */
#if DVB_API_VERSION >= 5
  dvb_mux_conf_t *dmc = &lm->lm_tuning;
  struct dvb_frontend_parameters *p = &dmc->dmc_fe_params;
  struct dtv_property cmds[20];
  struct dtv_properties cmdseq = { .num = 0, .props = cmds };
  
  /* Clear Q */
  static struct dtv_property clear_p[] = {
    { .cmd = DTV_CLEAR },
  };
  static struct dtv_properties clear_cmdseq = {
    .num = 1,
    .props = clear_p
  };
  if ((ioctl(lfe->lfe_fe_fd, FE_SET_PROPERTY, &clear_cmdseq)) != 0)
    return -1;

  if (freq == (uint32_t)-1)
    freq = p->frequency;
  
  /* Tune */
#define S2CMD(c, d)\
  cmds[cmdseq.num].cmd      = c;\
  cmds[cmdseq.num++].u.data = d
  S2CMD(DTV_DELIVERY_SYSTEM, lm->lm_tuning.dmc_fe_delsys);
  S2CMD(DTV_FREQUENCY,       freq);
  S2CMD(DTV_INVERSION,       p->inversion);

  /* DVB-T */
  if (lfe->lfe_info.type == FE_OFDM) {
    S2CMD(DTV_BANDWIDTH_HZ,      dvb_bandwidth(p->u.ofdm.bandwidth));
    S2CMD(DTV_CODE_RATE_HP,      p->u.ofdm.code_rate_HP);
    S2CMD(DTV_CODE_RATE_LP,      p->u.ofdm.code_rate_LP);
    S2CMD(DTV_MODULATION,        p->u.ofdm.constellation);
    S2CMD(DTV_TRANSMISSION_MODE, p->u.ofdm.transmission_mode);
    S2CMD(DTV_GUARD_INTERVAL,    p->u.ofdm.guard_interval);
    S2CMD(DTV_HIERARCHY,         p->u.ofdm.hierarchy_information);

  /* DVB-C */
  } else if (lfe->lfe_info.type == FE_QAM) {
    S2CMD(DTV_SYMBOL_RATE,       p->u.qam.symbol_rate);
    S2CMD(DTV_MODULATION,        p->u.qam.modulation);
    S2CMD(DTV_INNER_FEC,         p->u.qam.fec_inner);

  /* DVB-S */
  } else if (lfe->lfe_info.type == FE_QPSK) {
    S2CMD(DTV_SYMBOL_RATE,       p->u.qpsk.symbol_rate);
    S2CMD(DTV_INNER_FEC,         p->u.qpsk.fec_inner);
    S2CMD(DTV_MODULATION,        dmc->dmc_fe_modulation);
    S2CMD(DTV_ROLLOFF,           dmc->dmc_fe_rolloff);

  /* ATSC */
  } else {
    S2CMD(DTV_MODULATION,        p->u.vsb.modulation);
  }

  /* Tune */
  S2CMD(DTV_TUNE, 0);
#undef S2CMD
#else
  dvb_mux_conf_t dmc = lm->lm_tuning;
  struct dvb_frontend_parameters *p = &dmc.dmc_fe_params;
  if (freq != (uint32_t)-1)
    p->frequency = freq;
#endif

  /* discard stale events */
  while (1) {
    if (ioctl(lfe->lfe_fe_fd, FE_GET_EVENT, &ev) == -1)
      break;
  }

  /* S2 tuning */
#if DVB_API_VERSION >= 5
#if ENABLE_TRACE
  int i;
  for (i = 0; i < cmdseq.num; i++)
    tvhtrace("linuxdvb", "S2CMD %02u => %u", cmds[i].cmd, cmds[i].u.data);
#endif
  r = ioctl(lfe->lfe_fe_fd, FE_SET_PROPERTY, &cmdseq);

  /* v3 tuning */
#else
  r = ioctl(lfe->lfe_fe_fd, FE_SET_FRONTEND, p);
#endif

  /* Failed */
  if (r != 0) {
    tvherror("linuxdvb", "%s - failed to tune [e=%s]", buf1, strerror(errno));
    if (errno == EINVAL)
      mmi->mmi_tune_failed = 1;
    return SM_CODE_TUNING_FAILED;
  }

  return r;
}

int
linuxdvb_frontend_tune1
  ( linuxdvb_frontend_t *lfe, mpegts_mux_instance_t *mmi, uint32_t freq )
{
  int r;
  char buf1[256], buf2[256];

  lfe->mi_display_name((mpegts_input_t*)lfe, buf1, sizeof(buf1));
  mmi->mmi_mux->mm_display_name(mmi->mmi_mux, buf2, sizeof(buf2));
  tvhdebug("linuxdvb", "%s - starting %s", buf1, buf2);

  /* Tune */
  tvhtrace("linuxdvb", "%s - tuning", buf1);
  r = linuxdvb_frontend_tune0(lfe, mmi, freq);

  /* Start monitor */
  time(&lfe->lfe_monitor);
  lfe->lfe_monitor += 4;
  gtimer_arm_ms(&lfe->lfe_monitor_timer, linuxdvb_frontend_monitor, lfe, 50);
  
  return r;
}

/* **************************************************************************
 * Creation/Config
 * *************************************************************************/
 
linuxdvb_frontend_t *
linuxdvb_frontend_create0
  ( linuxdvb_adapter_t *la, const char *uuid, htsmsg_t *conf, fe_type_t type )
{
  const char *str;
  const idclass_t *idc;
  pthread_t tid;

  /* Get type */
  if (conf) {
    if (!(str = htsmsg_get_str(conf, "type")))
      return NULL;
    type = dvb_str2type(str);
  }

  /* Class */
  if (type == FE_QPSK)
    idc = &linuxdvb_frontend_dvbs_class;
  else if (type == FE_QAM)
    idc = &linuxdvb_frontend_dvbc_class;
  else if (type == FE_OFDM)
    idc = &linuxdvb_frontend_dvbt_class;
  else if (type == FE_ATSC)
    idc = &linuxdvb_frontend_atsc_class;
  else {
    tvherror("linuxdvb", "unknown FE type %d", type);
    return NULL;
  }

  linuxdvb_frontend_t *lfe = calloc(1, sizeof(linuxdvb_frontend_t));
  lfe->lfe_info.type = type;
  lfe = (linuxdvb_frontend_t*)mpegts_input_create0((mpegts_input_t*)lfe, idc, uuid, conf);

  /* Input callbacks */
  lfe->mi_is_enabled     = linuxdvb_frontend_is_enabled;
  lfe->mi_start_mux      = linuxdvb_frontend_start_mux;
  lfe->mi_stop_mux       = linuxdvb_frontend_stop_mux;
  lfe->mi_open_service   = linuxdvb_frontend_open_service;
  lfe->mi_close_service  = linuxdvb_frontend_close_service;
  lfe->mi_network_list   = linuxdvb_frontend_network_list;
  lfe->lfe_open_pid      = linuxdvb_frontend_open_pid;

  /* Adapter link */
  lfe->lh_parent = (linuxdvb_hardware_t*)la;
  LIST_INSERT_HEAD(&la->lh_children, (linuxdvb_hardware_t*)lfe, lh_parent_link);

  /* DVR lock/cond */
  pthread_mutex_init(&lfe->lfe_dvr_lock, NULL);
  pthread_cond_init(&lfe->lfe_dvr_cond, NULL);
 
  /* Start table thread */
  pthread_create(&tid, NULL, mpegts_input_table_thread, lfe);

  /* No conf */
  if (!conf)
    return lfe;

  return lfe;
}

linuxdvb_frontend_t *
linuxdvb_frontend_added
  ( linuxdvb_adapter_t *la, int fe_num,
    const char *fe_path,
    const char *dmx_path,
    const char *dvr_path,
    const struct dvb_frontend_info *fe_info )
{
  linuxdvb_hardware_t *lh;
  linuxdvb_frontend_t *lfe = NULL;

  /* Find existing */
  LIST_FOREACH(lh, &la->lh_children, lh_parent_link) {
    lfe = (linuxdvb_frontend_t*)lh;
    if (lfe->lfe_number == fe_num) {
      if (lfe->lfe_info.type != fe_info->type) {
        tvhlog(LOG_ERR, "linuxdvb", "detected incorrect fe_type %s != %s",
               dvb_type2str(lfe->lfe_info.type), dvb_type2str(fe_info->type));
        return NULL;
      }
      break;
    }
  }

  /* Create new */
  if (!lfe) {
    if (!(lfe = linuxdvb_frontend_create0(la, NULL, NULL, fe_info->type))) {
      tvhlog(LOG_ERR, "linuxdvb", "failed to create frontend");
      return NULL;
    }
  }

  /* Defaults */
  if (!lfe->mi_displayname)
    lfe->mi_displayname = strdup(fe_path);

  /* Copy info */
  lfe->lfe_number = fe_num;
  memcpy(&lfe->lfe_info, fe_info, sizeof(struct dvb_frontend_info));

  /* Set paths */
  lfe->lfe_fe_path  = strdup(fe_path);
  lfe->lfe_dmx_path = strdup(dmx_path);
  lfe->lfe_dvr_path = strdup(dvr_path);

  return lfe;
}

void
linuxdvb_frontend_save ( linuxdvb_frontend_t *lfe, htsmsg_t *m )
{
  mpegts_input_save((mpegts_input_t*)lfe, m);
  htsmsg_add_str(m, "type", dvb_type2str(lfe->lfe_info.type));
}

/******************************************************************************
 * Editor Configuration
 *
 * vim:sts=2:ts=2:sw=2:et
 *****************************************************************************/
