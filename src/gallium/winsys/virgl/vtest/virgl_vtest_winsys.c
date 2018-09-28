/*
 * Copyright 2014, 2015 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include "util/u_memory.h"
#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "state_tracker/sw_winsys.h"
#include "state_tracker/xlibsw_api.h"

#include "virgl_vtest_winsys.h"
#include "virgl_vtest_public.h"



#define VT_SYNC_COORDS (1U<<0)
#define VT_IGNORE_MAP (1U<<1)
#define VT_IGNORE_VIS (1U<<2)
#define VT_TRACK_EVENTS (1U<<3)
#define VT_ALWAYS_READBACK (1U<<4)
#define VT_IGNORE_ATTR_COORDS (1U<<5)

static const struct debug_named_value dt_options[] = {
		{"sync_coords",      VT_SYNC_COORDS,   "Sync coordinates every frame"},
		{"ignore_map",      VT_IGNORE_MAP,   "Ignore map state"},
		{"ignore_vis",      VT_IGNORE_VIS,   "Ignore partial visibility"},
		{"track_events",      VT_TRACK_EVENTS,   "Track configure and visibility events"},
		{"always_readback",      VT_ALWAYS_READBACK,   "Always read texture back(slow)"},
		{"ignore_attr_coords",      VT_IGNORE_ATTR_COORDS,   "Always read texture back(slow)"},

		DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(dt_options, "VTEST_DT_OPTIONS", dt_options, 0)

static void *virgl_vtest_resource_map(struct virgl_winsys *vws,
                                      struct virgl_hw_res *res);
static void virgl_vtest_resource_unmap(struct virgl_winsys *vws,
                                       struct virgl_hw_res *res);

static inline boolean can_cache_resource(struct virgl_hw_res *res)
{
   return res->cacheable == TRUE;
}

static uint32_t vtest_get_transfer_size(struct virgl_hw_res *res,
                                        const struct pipe_box *box,
                                        uint32_t stride, uint32_t layer_stride,
                                        uint32_t level, uint32_t *valid_stride_p)
{
   uint32_t valid_stride, valid_layer_stride;

   valid_stride = util_format_get_stride(res->format, box->width);
   if (stride) {
      if (box->height > 1)
         valid_stride = stride;
   }

   valid_layer_stride = util_format_get_2d_size(res->format, valid_stride,
                                                box->height);
   if (layer_stride) {
      if (box->depth > 1)
         valid_layer_stride = layer_stride;
   }

   *valid_stride_p = valid_stride;
   return valid_layer_stride * box->depth;
}

static int
virgl_vtest_transfer_put(struct virgl_winsys *vws,
                         struct virgl_hw_res *res,
                         const struct pipe_box *box,
                         uint32_t stride, uint32_t layer_stride,
                         uint32_t buf_offset, uint32_t level)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);
   uint32_t size;
   void *ptr;
   uint32_t valid_stride;

   size = vtest_get_transfer_size(res, box, stride, layer_stride, level,
                                  &valid_stride);

   virgl_vtest_send_transfer_cmd(vtws, VCMD_TRANSFER_PUT, res->res_handle,
                                 level, stride, layer_stride,
                                 box, size);
   ptr = virgl_vtest_resource_map(vws, res);
   virgl_vtest_send_transfer_put_data(vtws, ptr + buf_offset, size);
   virgl_vtest_resource_unmap(vws, res);
   return 0;
}

static int
virgl_vtest_transfer_get(struct virgl_winsys *vws,
                         struct virgl_hw_res *res,
                         const struct pipe_box *box,
                         uint32_t stride, uint32_t layer_stride,
                         uint32_t buf_offset, uint32_t level)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);
   uint32_t size;
   void *ptr;
   uint32_t valid_stride;

   size = vtest_get_transfer_size(res, box, stride, layer_stride, level,
                                  &valid_stride);

   virgl_vtest_send_transfer_cmd(vtws, VCMD_TRANSFER_GET, res->res_handle,
                                 level, stride, layer_stride,
                                 box, size);


   ptr = virgl_vtest_resource_map(vws, res);
   virgl_vtest_recv_transfer_get_data(vtws, ptr + buf_offset, size,
                                      valid_stride, box, res->format);
   virgl_vtest_resource_unmap(vws, res);
   return 0;
}

struct vtest_displaytarget
{
struct sw_displaytarget *sws_dt;
Drawable drawable;
int x,y,w,h;
int vis;
bool mapped;

uint32_t id;
};

static Display *dt_dpy;

static void vtest_displaytarget_destroy(struct virgl_vtest_winsys *vtws, 
                                        struct vtest_displaytarget *dt)
{
   virgl_vtest_send_dt(vtws, VCMD_DT_CMD_DESTROY, 0, 0, 0, 0, dt->id, 0, 0);
   vtws->sws->displaytarget_destroy(vtws->sws, dt->sws_dt);
   vtws->dt_set &= ~(1 << dt->id);
   FREE(dt);
}

static struct vtest_displaytarget *
vtest_displaytarget_create(struct virgl_vtest_winsys *vtws,
                          unsigned tex_usage,
                          enum pipe_format format,
                          unsigned width, unsigned height,
                          unsigned alignment,
                          const void *front_private,
                          unsigned *stride)
{
   struct vtest_displaytarget *dt;
   int id = 0;

   // allocate handle (0<=handle<32) for remote dt
   while((vtws->dt_set & (1<<id)) && (id < 32)) id++;

   if(id == 32)
      return NULL;

   dt = CALLOC_STRUCT(vtest_displaytarget);
   dt->id = id;
   dt->sws_dt = vtws->sws->displaytarget_create(vtws->sws, tex_usage, format,
                                                width, height, alignment, front_private,
                                                stride);

   virgl_vtest_send_dt(vtws, VCMD_DT_CMD_CREATE, 0, 0, width, height, id, 0, 0);
   vtws->dt_set |= 1 << id;

   if( !dt_dpy )
      dt_dpy = XOpenDisplay(NULL);

   return dt;
}
static void virgl_hw_res_destroy(struct virgl_vtest_winsys *vtws,
                                 struct virgl_hw_res *res)
{
   virgl_vtest_send_resource_unref(vtws, res->res_handle);
   if (res->dt)
      vtest_displaytarget_destroy(vtws, res->dt);
   free(res->ptr);
   FREE(res);
}

static boolean virgl_vtest_resource_is_busy(struct virgl_vtest_winsys *vtws,
                                            struct virgl_hw_res *res)
{
   /* implement busy check */
   int ret;
   ret = virgl_vtest_busy_wait(vtws, res->res_handle, 0);

   if (ret < 0)
      return FALSE;

   return ret == 1 ? TRUE : FALSE;
}

static void
virgl_cache_flush(struct virgl_vtest_winsys *vtws)
{
   struct list_head *curr, *next;
   struct virgl_hw_res *res;

   mtx_lock(&vtws->mutex);
   curr = vtws->delayed.next;
   next = curr->next;

   while (curr != &vtws->delayed) {
      res = LIST_ENTRY(struct virgl_hw_res, curr, head);
      LIST_DEL(&res->head);
      virgl_hw_res_destroy(vtws, res);
      curr = next;
      next = curr->next;
   }
   mtx_unlock(&vtws->mutex);
}

static void
virgl_cache_list_check_free(struct virgl_vtest_winsys *vtws)
{
   struct list_head *curr, *next;
   struct virgl_hw_res *res;
   int64_t now;

   now = os_time_get();
   curr = vtws->delayed.next;
   next = curr->next;
   while (curr != &vtws->delayed) {
      res = LIST_ENTRY(struct virgl_hw_res, curr, head);
      if (!os_time_timeout(res->start, res->end, now))
         break;

      LIST_DEL(&res->head);
      virgl_hw_res_destroy(vtws, res);
      curr = next;
      next = curr->next;
   }
}

static void virgl_vtest_resource_reference(struct virgl_vtest_winsys *vtws,
                                           struct virgl_hw_res **dres,
                                           struct virgl_hw_res *sres)
{
   struct virgl_hw_res *old = *dres;
   if (pipe_reference(&(*dres)->reference, &sres->reference)) {
      if (!can_cache_resource(old)) {
         virgl_hw_res_destroy(vtws, old);
      } else {
         mtx_lock(&vtws->mutex);
         virgl_cache_list_check_free(vtws);

         old->start = os_time_get();
         old->end = old->start + vtws->usecs;
         LIST_ADDTAIL(&old->head, &vtws->delayed);
         vtws->num_delayed++;
         mtx_unlock(&vtws->mutex);
      }
   }
   *dres = sres;
}

static struct virgl_hw_res *
virgl_vtest_winsys_resource_create(struct virgl_winsys *vws,
                                   enum pipe_texture_target target,
                                   uint32_t format,
                                   uint32_t bind,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t depth,
                                   uint32_t array_size,
                                   uint32_t last_level,
                                   uint32_t nr_samples,
                                   uint32_t size)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);
   struct virgl_hw_res *res;
   static int handle = 1;

   res = CALLOC_STRUCT(virgl_hw_res);
   if (!res)
      return NULL;

   if (bind & (VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT)) {
      res->dt = vtest_displaytarget_create(vtws, bind, format,
                                                width, height, 64, NULL,
                                                &res->stride);

   } else {
      res->ptr = align_malloc(size, 64);
      if (!res->ptr) {
         FREE(res);
         return NULL;
      }
   }

   res->bind = bind;
   res->format = format;
   res->height = height;
   res->width = width;
   virgl_vtest_send_resource_create(vtws, handle, target, format, bind,
                                    width, height, depth, array_size,
                                    last_level, nr_samples);

   res->res_handle = handle++;
   pipe_reference_init(&res->reference, 1);
   return res;
}

static void virgl_vtest_winsys_resource_unref(struct virgl_winsys *vws,
                                              struct virgl_hw_res *hres)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);
   virgl_vtest_resource_reference(vtws, &hres, NULL);
}

static void *virgl_vtest_resource_map(struct virgl_winsys *vws,
                                      struct virgl_hw_res *res)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);

   if (res->dt) {
      return vtws->sws->displaytarget_map(vtws->sws, res->dt->sws_dt, 0);
   } else {
      res->mapped = res->ptr;
      return res->mapped;
   }
}

static void virgl_vtest_resource_unmap(struct virgl_winsys *vws,
                                       struct virgl_hw_res *res)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);
   if (res->mapped)
      res->mapped = NULL;

   if (res->dt)
      vtws->sws->displaytarget_unmap(vtws->sws, res->dt->sws_dt);
}

static void virgl_vtest_resource_wait(struct virgl_winsys *vws,
                                      struct virgl_hw_res *res)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);

   virgl_vtest_busy_wait(vtws, res->res_handle, VCMD_BUSY_WAIT_FLAG_WAIT);
}

static inline int virgl_is_res_compat(struct virgl_vtest_winsys *vtws,
                                      struct virgl_hw_res *res,
                                      uint32_t size, uint32_t bind,
                                      uint32_t format)
{
   if (res->bind != bind)
      return 0;
   if (res->format != format)
      return 0;
   if (res->size < size)
      return 0;
   if (res->size > size * 2)
      return 0;

   if (virgl_vtest_resource_is_busy(vtws, res)) {
      return -1;
   }

   return 1;
}

static struct virgl_hw_res *
virgl_vtest_winsys_resource_cache_create(struct virgl_winsys *vws,
                                         enum pipe_texture_target target,
                                         uint32_t format,
                                         uint32_t bind,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t depth,
                                         uint32_t array_size,
                                         uint32_t last_level,
                                         uint32_t nr_samples,
                                         uint32_t size)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);
   struct virgl_hw_res *res, *curr_res;
   struct list_head *curr, *next;
   int64_t now;
   int ret;

   /* only store binds for vertex/index/const buffers */
   if (bind != VIRGL_BIND_CONSTANT_BUFFER && bind != VIRGL_BIND_INDEX_BUFFER &&
       bind != VIRGL_BIND_VERTEX_BUFFER && bind != VIRGL_BIND_CUSTOM)
      goto alloc;

   mtx_lock(&vtws->mutex);

   res = NULL;
   curr = vtws->delayed.next;
   next = curr->next;

   now = os_time_get();
   while (curr != &vtws->delayed) {
      curr_res = LIST_ENTRY(struct virgl_hw_res, curr, head);

      if (!res && ((ret = virgl_is_res_compat(vtws, curr_res, size, bind, format)) > 0))
         res = curr_res;
      else if (os_time_timeout(curr_res->start, curr_res->end, now)) {
         LIST_DEL(&curr_res->head);
         virgl_hw_res_destroy(vtws, curr_res);
      } else
         break;

      if (ret == -1)
         break;

      curr = next;
      next = curr->next;
   }

   if (!res && ret != -1) {
      while (curr != &vtws->delayed) {
         curr_res = LIST_ENTRY(struct virgl_hw_res, curr, head);
         ret = virgl_is_res_compat(vtws, curr_res, size, bind, format);
         if (ret > 0) {
            res = curr_res;
            break;
         }
         if (ret == -1)
            break;
         curr = next;
         next = curr->next;
      }
   }

   if (res) {
      LIST_DEL(&res->head);
      --vtws->num_delayed;
      mtx_unlock(&vtws->mutex);
      pipe_reference_init(&res->reference, 1);
      return res;
   }

   mtx_unlock(&vtws->mutex);

alloc:
   res = virgl_vtest_winsys_resource_create(vws, target, format, bind,
                                            width, height, depth, array_size,
                                            last_level, nr_samples, size);
   if (bind == VIRGL_BIND_CONSTANT_BUFFER || bind == VIRGL_BIND_INDEX_BUFFER ||
       bind == VIRGL_BIND_VERTEX_BUFFER)
      res->cacheable = TRUE;
   return res;
}

static struct virgl_cmd_buf *virgl_vtest_cmd_buf_create(struct virgl_winsys *vws)
{
   struct virgl_vtest_cmd_buf *cbuf;

   cbuf = CALLOC_STRUCT(virgl_vtest_cmd_buf);
   if (!cbuf)
      return NULL;

   cbuf->nres = 512;
   cbuf->res_bo = CALLOC(cbuf->nres, sizeof(struct virgl_hw_buf*));
   if (!cbuf->res_bo) {
      FREE(cbuf);
      return NULL;
   }
   cbuf->ws = vws;
   cbuf->base.buf = cbuf->buf;
   return &cbuf->base;
}

static void virgl_vtest_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct virgl_vtest_cmd_buf *cbuf = virgl_vtest_cmd_buf(_cbuf);

   FREE(cbuf->res_bo);
   FREE(cbuf);
}

static boolean virgl_vtest_lookup_res(struct virgl_vtest_cmd_buf *cbuf,
                                      struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle & (sizeof(cbuf->is_handle_added)-1);
   int i;

   if (cbuf->is_handle_added[hash]) {
      i = cbuf->reloc_indices_hashlist[hash];
      if (cbuf->res_bo[i] == res)
         return true;

      for (i = 0; i < cbuf->cres; i++) {
         if (cbuf->res_bo[i] == res) {
            cbuf->reloc_indices_hashlist[hash] = i;
            return true;
         }
      }
   }
   return false;
}

static void virgl_vtest_release_all_res(struct virgl_vtest_winsys *vtws,
                                        struct virgl_vtest_cmd_buf *cbuf)
{
   int i;

   for (i = 0; i < cbuf->cres; i++) {
      p_atomic_dec(&cbuf->res_bo[i]->num_cs_references);
      virgl_vtest_resource_reference(vtws, &cbuf->res_bo[i], NULL);
   }
   cbuf->cres = 0;
}

static void virgl_vtest_add_res(struct virgl_vtest_winsys *vtws,
                                struct virgl_vtest_cmd_buf *cbuf,
                                struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle & (sizeof(cbuf->is_handle_added)-1);

   if (cbuf->cres >= cbuf->nres) {
      unsigned new_nres = cbuf->nres + 256;
      struct virgl_hw_res **new_re_bo = REALLOC(cbuf->res_bo,
                                                cbuf->nres * sizeof(struct virgl_hw_buf*),
                                                new_nres * sizeof(struct virgl_hw_buf*));
      if (!new_re_bo) {
          fprintf(stderr,"failure to add relocation %d, %d\n", cbuf->cres, cbuf->nres);
          return;
      }

      cbuf->res_bo = new_re_bo;
      cbuf->nres = new_nres;
   }

   cbuf->res_bo[cbuf->cres] = NULL;
   virgl_vtest_resource_reference(vtws, &cbuf->res_bo[cbuf->cres], res);
   cbuf->is_handle_added[hash] = TRUE;

   cbuf->reloc_indices_hashlist[hash] = cbuf->cres;
   p_atomic_inc(&res->num_cs_references);
   cbuf->cres++;
}

static int virgl_vtest_winsys_submit_cmd(struct virgl_winsys *vws,
                                         struct virgl_cmd_buf *_cbuf)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);
   struct virgl_vtest_cmd_buf *cbuf = virgl_vtest_cmd_buf(_cbuf);
   int ret;

   if (cbuf->base.cdw == 0)
      return 0;

   ret = virgl_vtest_submit_cmd(vtws, cbuf);

   virgl_vtest_release_all_res(vtws, cbuf);
   memset(cbuf->is_handle_added, 0, sizeof(cbuf->is_handle_added));
   cbuf->base.cdw = 0;
   return ret;
}

static void virgl_vtest_emit_res(struct virgl_winsys *vws,
                                 struct virgl_cmd_buf *_cbuf,
                                 struct virgl_hw_res *res, boolean write_buf)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);
   struct virgl_vtest_cmd_buf *cbuf = virgl_vtest_cmd_buf(_cbuf);
   boolean already_in_list = virgl_vtest_lookup_res(cbuf, res);

   if (write_buf)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   if (!already_in_list)
      virgl_vtest_add_res(vtws, cbuf, res);
}

static boolean virgl_vtest_res_is_ref(struct virgl_winsys *vws,
                                      struct virgl_cmd_buf *_cbuf,
                                      struct virgl_hw_res *res)
{
   if (!res->num_cs_references)
      return FALSE;

   return TRUE;
}

static int virgl_vtest_get_caps(struct virgl_winsys *vws,
                                struct virgl_drm_caps *caps)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);

   virgl_ws_fill_new_caps_defaults(caps);
   return virgl_vtest_send_get_caps(vtws, caps);
}

static struct pipe_fence_handle *
virgl_cs_create_fence(struct virgl_winsys *vws)
{
   struct virgl_hw_res *res;

   res = virgl_vtest_winsys_resource_cache_create(vws,
                                                PIPE_BUFFER,
                                                PIPE_FORMAT_R8_UNORM,
                                                PIPE_BIND_CUSTOM,
                                                8, 1, 1, 0, 0, 0, 8);

   return (struct pipe_fence_handle *)res;
}

static bool virgl_fence_wait(struct virgl_winsys *vws,
                             struct pipe_fence_handle *fence,
                             uint64_t timeout)
{
   struct virgl_vtest_winsys *vdws = virgl_vtest_winsys(vws);
   struct virgl_hw_res *res = virgl_hw_res(fence);

   if (timeout == 0)
      return 1;//!virgl_vtest_resource_is_busy(vdws, res);

   if (timeout != PIPE_TIMEOUT_INFINITE) {
      int64_t start_time = os_time_get();
      timeout /= 1000;
      while (virgl_vtest_resource_is_busy(vdws, res)) {
         if (os_time_get() - start_time >= timeout)
            return FALSE;
         os_time_sleep(10);
      }
      return TRUE;
   }
   virgl_vtest_resource_wait(vws, res);
   return TRUE;
}

static void virgl_fence_reference(struct virgl_winsys *vws,
                                  struct pipe_fence_handle **dst,
                                  struct pipe_fence_handle *src)
{
   struct virgl_vtest_winsys *vdws = virgl_vtest_winsys(vws);
   virgl_vtest_resource_reference(vdws, (struct virgl_hw_res **)dst,
                                  virgl_hw_res(src));
}


static void virgl_vtest_flush_frontbuffer(struct virgl_winsys *vws,
                                          struct virgl_hw_res *res,
                                          unsigned level, unsigned layer,
                                          void *winsys_drawable_handle,
                                          struct pipe_box *sub_box)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);
   struct pipe_box box;
   void *map;
   uint32_t size;
   uint32_t offset = 0, valid_stride;
   struct xlib_drawable *dr = (struct xlib_drawable*)winsys_drawable_handle;
   struct vtest_displaytarget *dt = res->dt;
   bool dt_sync_coords = (debug_get_option_dt_options() & VT_SYNC_COORDS);
   bool dt_visible;

   if (!dt)
      return;

   memset(&box, 0, sizeof(box));

   if (sub_box) {
      box = *sub_box;
      offset = box.y / util_format_get_blockheight(res->format) * res->stride +
               box.x / util_format_get_blockwidth(res->format) * util_format_get_blocksize(res->format);
   } else {
      box.z = layer;
      box.width = res->width;
      box.height = res->height;
      box.depth = 1;
   }

   size = vtest_get_transfer_size(res, &box, res->stride, 0, level, &valid_stride);

   virgl_vtest_busy_wait(vtws, res->res_handle, VCMD_BUSY_WAIT_FLAG_WAIT);


   //if( debug_get_option_dt_options() & VT_TRACK_EVENTS )
   {
      XEvent event;

      XSelectInput(dt_dpy, dr->drawable,StructureNotifyMask|VisibilityChangeMask);

      while(XPending(dt_dpy)) {
         XNextEvent(dt_dpy, &event);
         switch(event.type) {
            case ConfigureNotify:
		dt->w = event.xconfigure.width;
		dt->h = event.xconfigure.height;
		if(event.xconfigure.send_event)
		{
		dt->x = event.xconfigure.x;
		dt->y = event.xconfigure.y;
		};
		dt_sync_coords = true;
		//printf("configure %d %d\n", pos_track.x, pos_track.y);
		break;
            case VisibilityNotify:
		//printf("visibility %d\n",event.xvisibility.state);
		dt->vis = event.xvisibility.state;
		dt_sync_coords = true;
		break;
		/*switch(event.xvisibility.state)
		{
		case VisibilityUnobscured
		}*/
            default:
		break;
         }
      }
   }

// parent tracking code, use for debug wine drawables detection
#if 0

   Window parent = dr->drawable, *children;
   Drawable root; 
   while( parent && XQueryTree(dt_dpy, parent, &root, &parent, &children, &tmp) )
   {
       XWindowAttributes attrs;
       int x, y;

printf("parent %d\n", parent);
      if( !parent ) break;
      XGetWindowAttributes(dt_dpy, parent, &attrs);
      printf("attrs %d %d %d\n", attrs.map_state, attrs.x, attrs.y );
XTranslateCoordinates(dt_dpy,
                      parent,         // get position for this window
                      attrs.root,//DefaultRootWindow(pos_track.dpy), // something like macro: DefaultRootWindow(dpy)
                      attrs.x, attrs.y,        // local left top coordinates of the wnd
                      &x,     // these is position of wnd in root_window
                      &y,     // ...
                      &tmp);
    
      XFree((char *)children);
  }
#endif

   // get coordinates from x11 (slow)
   if( dt_sync_coords )
   {
      int tmp, x = 0, y = 0;
      XWindowAttributes attrs;

      XGetWindowAttributes(dt_dpy, dr->drawable, &attrs);
      if( !(debug_get_option_dt_options() & VT_IGNORE_ATTR_COORDS))
         x = attrs.x, y = attrs.y;
      // printf("attrs %d %d %d\n", attrs.map_state, attrs.x, attrs.y );
      // XGetGeometry(dt_dpy, dr->drawable, &root, &x, &y, &dt->w, &dt->h, &tmp, &tmp);
      XTranslateCoordinates(dt_dpy,
                      dr->drawable,         // get position for this window
                      attrs.root,//DefaultRootWindow(pos_track.dpy), // something like macro: DefaultRootWindow(dpy)
                      x, y,        // local left top coordinates of the wnd
                      &dt->x,     // these is position of wnd in root_window
                      &dt->y,     // ...
                      &tmp);

      //printf("translate %d %d\n", dt->x, dt->y );

      if( dt->x < 0 || dt->y < 0 )
      {
         if( x > 0 && y > 0 ) dt->x = x, dt->y = y;
         else dt->x = dt->y = 0;
      }

      if( attrs.width > 0 && attrs.height > 0 )
         dt->w = attrs.width, dt->h = attrs.height;

      if( dt->w <= 0|| dt->h <= 0 )
         dt->w = box.width, dt->h = box.height;

      dt->mapped = (attrs.map_state == 2) || (debug_get_option_dt_options() & VT_IGNORE_MAP);
   }
  // printf("vis %d %d\n", dt->vis, dt_visible);
   dt_visible =  (!dt->vis || ((dt->vis == 1) && (debug_get_option_dt_options() & VT_IGNORE_VIS))) && dt->mapped;

   if( dt_sync_coords )
      virgl_vtest_send_dt(virgl_vtest_winsys(vws), VCMD_DT_CMD_SET_RECT, dt->x, dt->y, dt->w, dt->h, dt->id, 0, dt_visible);
  

   if( dt_visible )
      virgl_vtest_send_dt(virgl_vtest_winsys(vws), VCMD_DT_CMD_FLUSH, box.x, box.y, box.width, box.height, dt->id, res->res_handle, (uint32_t)dr->drawable);

   if( ( dt_visible || !dt->mapped || dt->vis == 2 ) && dt->drawable && !(debug_get_option_dt_options() & VT_ALWAYS_READBACK)  )
      return;

   dt->drawable = dr->drawable;

   map = vtws->sws->displaytarget_map(vtws->sws, res->dt->sws_dt, 0);

   /* execute a transfer */
   virgl_vtest_send_transfer_cmd(vtws, VCMD_TRANSFER_GET, res->res_handle,
                                 level, res->stride, 0, &box, size);

   virgl_vtest_recv_transfer_get_data(vtws, map + offset, size, valid_stride,
                                      &box, res->format);
   vtws->sws->displaytarget_unmap(vtws->sws, res->dt->sws_dt);

   vtws->sws->displaytarget_display(vtws->sws, res->dt->sws_dt, winsys_drawable_handle,
                                    sub_box);
}

static void
virgl_vtest_winsys_destroy(struct virgl_winsys *vws)
{
   struct virgl_vtest_winsys *vtws = virgl_vtest_winsys(vws);

   virgl_cache_flush(vtws);

   mtx_destroy(&vtws->mutex);
   if( vtws->ring )
      FREE(vtws->ring);
   FREE(vtws);
}

struct virgl_winsys *
virgl_vtest_winsys_wrap(struct sw_winsys *sws)
{
   struct virgl_vtest_winsys *vtws;

   vtws = CALLOC_STRUCT(virgl_vtest_winsys);
   if (!vtws)
      return NULL;

   while(virgl_vtest_connect(vtws));
      usleep(100000);
   vtws->sws = sws;

   vtws->usecs = 1000000;
   LIST_INITHEAD(&vtws->delayed);
   (void) mtx_init(&vtws->mutex, mtx_plain);

   vtws->base.destroy = virgl_vtest_winsys_destroy;

   vtws->base.transfer_put = virgl_vtest_transfer_put;
   vtws->base.transfer_get = virgl_vtest_transfer_get;

   vtws->base.resource_create = virgl_vtest_winsys_resource_cache_create;
   vtws->base.resource_unref = virgl_vtest_winsys_resource_unref;
   vtws->base.resource_map = virgl_vtest_resource_map;
   vtws->base.resource_wait = virgl_vtest_resource_wait;
   vtws->base.cmd_buf_create = virgl_vtest_cmd_buf_create;
   vtws->base.cmd_buf_destroy = virgl_vtest_cmd_buf_destroy;
   vtws->base.submit_cmd = virgl_vtest_winsys_submit_cmd;

   vtws->base.emit_res = virgl_vtest_emit_res;
   vtws->base.res_is_referenced = virgl_vtest_res_is_ref;
   vtws->base.get_caps = virgl_vtest_get_caps;

   vtws->base.cs_create_fence = virgl_cs_create_fence;
   vtws->base.fence_wait = virgl_fence_wait;
   vtws->base.fence_reference = virgl_fence_reference;

   vtws->base.flush_frontbuffer = virgl_vtest_flush_frontbuffer;

   return &vtws->base;
}
