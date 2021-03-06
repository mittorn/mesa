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

#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <unistd.h>

#include <os/os_process.h>
#include <util/u_format.h>
#include "util/u_memory.h"
/* connect to remote socket */
#define VTEST_SOCKET_NAME "/tmp/.virgl_test"

#include "virgl_vtest_winsys.h"
#include "virgl_vtest_public.h"
#include "ring.h"


/* block read/write routines */
static int virgl_block_write(struct virgl_vtest_winsys *vws, void *buf, int size)
{
   void *ptr = buf;
   int left;
   int ret;

   if(vws->ring)
      return ring_write(vws->ring, buf, size);

   left = size;
   do { 
      ret = write(vws->sock_fd, ptr, left);
      if (ret < 0)
         return -errno;
      left -= ret;
      ptr += ret;
   } while (left);
   return size;
}

static int virgl_block_read(struct virgl_vtest_winsys *vws, void *buf, int size)
{
   void *ptr = buf;
   int left = size;
   int ret;
   if(vws->ring)
      return ring_read(vws->ring, buf, size);

   left = size;
   do {
      ret = read(vws->sock_fd, ptr, left);
      if (ret <= 0) {
         fprintf(stderr,
                 "lost connection to rendering server on %d read %d %d\n",
                 size, ret, errno);
         abort();
         return ret < 0 ? -errno : 0;
      }
      left -= ret;
      ptr += ret;
   } while (left);
   return size;
}

static int virgl_vtest_send_init(struct virgl_vtest_winsys *vws)
{
   uint32_t buf[VTEST_HDR_SIZE];
   const char *nstr = "virtest";
   char cmdline[64];
   int ret;

   ret = os_get_process_name(cmdline, 63);
   if (ret == FALSE)
      strcpy(cmdline, nstr);
#if defined(__GLIBC__) || defined(__CYGWIN__)
   if (!strcmp(cmdline, "shader_runner")) {
      const char *name;
      /* hack to get better testname */
      name = program_invocation_short_name;
      name += strlen(name) + 1;
      strncpy(cmdline, name, 63);
   }
#endif
   buf[VTEST_CMD_LEN] = strlen(cmdline) + 1;
   buf[VTEST_CMD_ID] = VCMD_CREATE_RENDERER;

   virgl_block_write(vws, &buf, sizeof(buf));
   virgl_block_write(vws, (void *)cmdline, strlen(cmdline) + 1);
   return 0;
}

int virgl_vtest_connect(struct virgl_vtest_winsys *vws)
{
   int sock, ret;
   const char *path = getenv("VTEST_SOCK"), *port;
   if(!path) path = VTEST_SOCKET_NAME;

   if((port = strchr(path,':')))
   {
      char host[32] = "";
      struct sockaddr_in in;

      sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

      if (sock < 0)
         return -1;

      memset(&in, 0, sizeof(in));
      in.sin_family = AF_INET;
      in.sin_port = htons(atoi(port + 1));
      memcpy( host, path, port - path);
      inet_pton(AF_INET, host, &in.sin_addr);

      do {
         ret = 0;
         if (connect(sock, (struct sockaddr *)&in, sizeof(in)) < 0) {
            ret = -errno;
         }
      } while (ret == -EINTR);
   }
   else
   {
      struct sockaddr_un un;

      sock = socket(PF_UNIX, SOCK_STREAM, 0);

      if (sock < 0)
         return -1;

      memset(&un, 0, sizeof(un));
      un.sun_family = AF_UNIX;
      snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);

      do {
         ret = 0;
         if (connect(sock, (struct sockaddr *)&un, sizeof(un)) < 0) {
            ret = -errno;
         }
      } while (ret == -EINTR);
   }

   vws->sock_fd = sock;
   const char *ring = getenv("VTEST_RING");
   if(ring && ring[0] != '0')
   {
      if(!strcmp(ring,"1"))
         ring = "/dev/shm";
      vws->ring = CALLOC_STRUCT(ring_s);
      ring_setup(vws->ring, sock, ring);
      ring_client_handshake(vws->ring, "glshim");
   }

   virgl_vtest_send_init(vws);
   return 0;
}

int virgl_vtest_send_get_caps(struct virgl_vtest_winsys *vws,
                              struct virgl_drm_caps *caps)
{
   uint32_t get_caps_buf[VTEST_HDR_SIZE * 2];
   uint32_t resp_buf[VTEST_HDR_SIZE];
   uint32_t caps_size = sizeof(struct virgl_caps_v2);
   int ret;
   get_caps_buf[VTEST_CMD_LEN] = 0;
   get_caps_buf[VTEST_CMD_ID] = VCMD_GET_CAPS2;
   get_caps_buf[VTEST_CMD_LEN + 2] = 0;
   get_caps_buf[VTEST_CMD_ID + 2] = VCMD_GET_CAPS;

   virgl_block_write(vws, &get_caps_buf, sizeof(get_caps_buf));

   ret = virgl_block_read(vws, resp_buf, sizeof(resp_buf));
   if (ret <= 0)
      return 0;

   if (resp_buf[1] == 2) {
       struct virgl_caps_v1 dummy;
       uint32_t resp_size = resp_buf[0] - 1;
       uint32_t dummy_size = 0;
       if (resp_size > caps_size) {
	   dummy_size = resp_size - caps_size;
	   resp_size = caps_size;
       }

       ret = virgl_block_read(vws, &caps->caps, resp_size);

       if (dummy_size)
	   ret = virgl_block_read(vws, &dummy, dummy_size);

       /* now read back the pointless caps v1 we requested */
       ret = virgl_block_read(vws, resp_buf, sizeof(resp_buf));
       if (ret <= 0)
	   return 0;
       ret = virgl_block_read(vws, &dummy, sizeof(struct virgl_caps_v1));
   } else
       ret = virgl_block_read(vws, &caps->caps, sizeof(struct virgl_caps_v1));

   return 0;
}

int virgl_vtest_send_resource_create(struct virgl_vtest_winsys *vws,
                                     uint32_t handle,
                                     enum pipe_texture_target target,
                                     uint32_t format,
                                     uint32_t bind,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t depth,
                                     uint32_t array_size,
                                     uint32_t last_level,
                                     uint32_t nr_samples)
{
   uint32_t res_create_buf[VCMD_RES_CREATE_SIZE], vtest_hdr[VTEST_HDR_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_RES_CREATE_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_RESOURCE_CREATE;

   res_create_buf[VCMD_RES_CREATE_RES_HANDLE] = handle;
   res_create_buf[VCMD_RES_CREATE_TARGET] = target;
   res_create_buf[VCMD_RES_CREATE_FORMAT] = format;
   res_create_buf[VCMD_RES_CREATE_BIND] = bind;
   res_create_buf[VCMD_RES_CREATE_WIDTH] = width;
   res_create_buf[VCMD_RES_CREATE_HEIGHT] = height;
   res_create_buf[VCMD_RES_CREATE_DEPTH] = depth;
   res_create_buf[VCMD_RES_CREATE_ARRAY_SIZE] = array_size;
   res_create_buf[VCMD_RES_CREATE_LAST_LEVEL] = last_level;
   res_create_buf[VCMD_RES_CREATE_NR_SAMPLES] = nr_samples;

   virgl_block_write(vws, &vtest_hdr, sizeof(vtest_hdr));
   virgl_block_write(vws, &res_create_buf, sizeof(res_create_buf));

   return 0;
}

int virgl_vtest_send_dt(struct virgl_vtest_winsys *vws,
					uint32_t cmd,
					uint32_t x,
					uint32_t y,
					uint32_t w,
					uint32_t h,
					uint32_t id,
					uint32_t handle,
					uint32_t drawable)
{
   uint32_t flush_buf[VCMD_DT_SIZE], vtest_hdr[VTEST_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_DT_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_DT_COMMAND;

   flush_buf[VCMD_DT_CMD] = cmd;
   flush_buf[VCMD_DT_X] = x;
   flush_buf[VCMD_DT_Y] = y;
   flush_buf[VCMD_DT_WIDTH] = w;
   flush_buf[VCMD_DT_HEIGHT] = h;
   flush_buf[VCMD_DT_ID] = id;
   flush_buf[VCMD_DT_HANDLE] = handle;
   flush_buf[VCMD_DT_DRAWABLE] = drawable;

   virgl_block_write(vws, &vtest_hdr, sizeof(vtest_hdr));
   virgl_block_write(vws, &flush_buf, sizeof(flush_buf));

   return 0;
}

int virgl_vtest_submit_cmd(struct virgl_vtest_winsys *vws,
                           struct virgl_vtest_cmd_buf *cbuf)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = cbuf->base.cdw;
   vtest_hdr[VTEST_CMD_ID] = VCMD_SUBMIT_CMD;

   virgl_block_write(vws, &vtest_hdr, sizeof(vtest_hdr));
   virgl_block_write(vws, cbuf->buf, cbuf->base.cdw * 4);
   return 0;
}

int virgl_vtest_send_resource_unref(struct virgl_vtest_winsys *vws,
                                    uint32_t handle)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t cmd[1];
   vtest_hdr[VTEST_CMD_LEN] = 1;
   vtest_hdr[VTEST_CMD_ID] = VCMD_RESOURCE_UNREF;

   cmd[0] = handle;
   virgl_block_write(vws, &vtest_hdr, sizeof(vtest_hdr));
   virgl_block_write(vws, &cmd, sizeof(cmd));
   return 0;
}

int virgl_vtest_send_transfer_cmd(struct virgl_vtest_winsys *vws,
                                  uint32_t vcmd,
                                  uint32_t handle,
                                  uint32_t level, uint32_t stride,
                                  uint32_t layer_stride,
                                  const struct pipe_box *box,
                                  uint32_t data_size)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t cmd[VCMD_TRANSFER_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_TRANSFER_HDR_SIZE;
   vtest_hdr[VTEST_CMD_ID] = vcmd;

   if (vcmd == VCMD_TRANSFER_PUT)
      vtest_hdr[VTEST_CMD_LEN] += data_size + 3 / 4;

   cmd[0] = handle;
   cmd[1] = level;
   cmd[2] = stride;
   cmd[3] = layer_stride;
   cmd[4] = box->x;
   cmd[5] = box->y;
   cmd[6] = box->z;
   cmd[7] = box->width;
   cmd[8] = box->height;
   cmd[9] = box->depth;
   cmd[10] = data_size;
   virgl_block_write(vws, &vtest_hdr, sizeof(vtest_hdr));
   virgl_block_write(vws, &cmd, sizeof(cmd));

   return 0;
}

int virgl_vtest_send_transfer_put_data(struct virgl_vtest_winsys *vws,
                                       void *data,
                                       uint32_t data_size)
{
   return virgl_block_write(vws, data, data_size);
}

int virgl_vtest_recv_transfer_get_data(struct virgl_vtest_winsys *vws,
                                       void *data,
                                       uint32_t data_size,
                                       uint32_t stride,
                                       const struct pipe_box *box,
                                       uint32_t format)
{
   void *line;
   void *ptr = data;
   int hblocks = util_format_get_nblocksy(format, box->height);

   line = malloc(stride);
   while (hblocks) {
      virgl_block_read(vws, line, stride);
      if(vws->ring)
         ring_sync_write(vws->ring);
      memcpy(ptr, line, util_format_get_stride(format, box->width));
      ptr += stride;
      hblocks--;
   }
   free(line);
   return 0;
}

int virgl_vtest_busy_wait(struct virgl_vtest_winsys *vws, int handle,
                          int flags)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t cmd[VCMD_BUSY_WAIT_SIZE];
   uint32_t result[1];
   int ret;
   vtest_hdr[VTEST_CMD_LEN] = VCMD_BUSY_WAIT_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_RESOURCE_BUSY_WAIT;
   cmd[VCMD_BUSY_WAIT_HANDLE] = handle;
   cmd[VCMD_BUSY_WAIT_FLAGS] = flags;

   virgl_block_write(vws, &vtest_hdr, sizeof(vtest_hdr));
   virgl_block_write(vws, &cmd, sizeof(cmd));

   ret = virgl_block_read(vws, vtest_hdr, sizeof(vtest_hdr));
   assert(ret);
   ret = virgl_block_read(vws, result, sizeof(result));
   assert(ret);
   return result[0];
}
