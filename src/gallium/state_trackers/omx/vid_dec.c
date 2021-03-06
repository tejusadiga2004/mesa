/**************************************************************************
 *
 * Copyright 2013 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/*
 * Authors:
 *      Christian König <christian.koenig@amd.com>
 *
 */


#include <assert.h>

#include <OMX_Video.h>

/* bellagio defines a DEBUG macro that we don't want */
#ifndef DEBUG
#include <bellagio/omxcore.h>
#undef DEBUG
#else
#include <bellagio/omxcore.h>
#endif

#include <bellagio/omx_base_video_port.h>

#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "vl/vl_vlc.h"

#include "entrypoint.h"
#include "vid_dec.h"

static OMX_ERRORTYPE vid_dec_Constructor(OMX_COMPONENTTYPE *comp, OMX_STRING name);
static OMX_ERRORTYPE vid_dec_Destructor(OMX_COMPONENTTYPE *comp);
static OMX_ERRORTYPE vid_dec_SetParameter(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR param);
static OMX_ERRORTYPE vid_dec_GetParameter(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR param);
static OMX_ERRORTYPE vid_dec_MessageHandler(OMX_COMPONENTTYPE *comp, internalRequestMessageType *msg);
static OMX_ERRORTYPE vid_dec_DecodeBuffer(omx_base_PortType *port, OMX_BUFFERHEADERTYPE *buf);
static OMX_ERRORTYPE vid_dec_FreeDecBuffer(omx_base_PortType *port, OMX_U32 idx, OMX_BUFFERHEADERTYPE *buf);
static void vid_dec_FrameDecoded(OMX_COMPONENTTYPE *comp, OMX_BUFFERHEADERTYPE* input, OMX_BUFFERHEADERTYPE* output);

static void vid_dec_name(char str[OMX_MAX_STRINGNAME_SIZE])
{
   snprintf(str, OMX_MAX_STRINGNAME_SIZE, OMX_VID_DEC_BASE_NAME, driver_descriptor.name);
}

static void vid_dec_name_mpeg2(char str[OMX_MAX_STRINGNAME_SIZE])
{
   snprintf(str, OMX_MAX_STRINGNAME_SIZE, OMX_VID_DEC_MPEG2_NAME, driver_descriptor.name);
}

static void vid_dec_name_avc(char str[OMX_MAX_STRINGNAME_SIZE])
{
   snprintf(str, OMX_MAX_STRINGNAME_SIZE, OMX_VID_DEC_AVC_NAME, driver_descriptor.name);
}

OMX_ERRORTYPE vid_dec_LoaderComponent(stLoaderComponentType *comp)
{
   comp->componentVersion.s.nVersionMajor = 0;
   comp->componentVersion.s.nVersionMinor = 0;
   comp->componentVersion.s.nRevision = 0;
   comp->componentVersion.s.nStep = 1;
   comp->name_specific_length = 2;

   comp->name = CALLOC(1, OMX_MAX_STRINGNAME_SIZE);
   if (comp->name == NULL)
      goto error;

   comp->name_specific = CALLOC(comp->name_specific_length, sizeof(char *));
   if (comp->name_specific == NULL)
      goto error;

   comp->name_specific[0] = CALLOC(1, OMX_MAX_STRINGNAME_SIZE);
   if (comp->name_specific[0] == NULL)
      goto error;

   comp->name_specific[1] = CALLOC(1, OMX_MAX_STRINGNAME_SIZE);
   if (comp->name_specific[1] == NULL)
      goto error;

   comp->role_specific = CALLOC(comp->name_specific_length, sizeof(char *));
   if (comp->role_specific == NULL)
      goto error;

   comp->role_specific[0] = CALLOC(1, OMX_MAX_STRINGNAME_SIZE);
   if (comp->role_specific[0] == NULL)
      goto error;

   comp->role_specific[1] = CALLOC(1, OMX_MAX_STRINGNAME_SIZE);
   if (comp->role_specific[1] == NULL)
      goto error;

   vid_dec_name(comp->name);
   vid_dec_name_mpeg2(comp->name_specific[0]);
   vid_dec_name_avc(comp->name_specific[1]);

   strcpy(comp->role_specific[0], OMX_VID_DEC_MPEG2_ROLE);
   strcpy(comp->role_specific[1], OMX_VID_DEC_AVC_ROLE);

   comp->constructor = vid_dec_Constructor;
 
   return OMX_ErrorNone;

error:

   FREE(comp->name);

   if (comp->name_specific) {
      FREE(comp->name_specific[0]);
      FREE(comp->name_specific[1]);
      FREE(comp->name_specific);
   }

   if (comp->role_specific) {
      FREE(comp->role_specific[0]);
      FREE(comp->role_specific[1]);
      FREE(comp->role_specific);
   }

   return OMX_ErrorInsufficientResources;
}

static OMX_ERRORTYPE vid_dec_Constructor(OMX_COMPONENTTYPE *comp, OMX_STRING name)
{
   char tmpstr[OMX_MAX_STRINGNAME_SIZE];
   vid_dec_PrivateType *priv;
   omx_base_video_PortType *port;
   struct pipe_screen *screen;
   OMX_ERRORTYPE r;
   int i;

   assert(!comp->pComponentPrivate);

   priv = comp->pComponentPrivate = CALLOC(1, sizeof(vid_dec_PrivateType));
   if (!priv)
      return OMX_ErrorInsufficientResources;

   r = omx_base_filter_Constructor(comp, name);
   if (r)
	return r;

   priv->profile = PIPE_VIDEO_PROFILE_UNKNOWN;

   vid_dec_name_mpeg2(tmpstr);
   if (!strcmp(name, tmpstr))
      priv->profile = PIPE_VIDEO_PROFILE_MPEG2_MAIN;

   vid_dec_name_avc(tmpstr);
   if (!strcmp(name, tmpstr))
      priv->profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;

   priv->BufferMgmtCallback = vid_dec_FrameDecoded;
   priv->messageHandler = vid_dec_MessageHandler;
   priv->destructor = vid_dec_Destructor;

   comp->SetParameter = vid_dec_SetParameter;
   comp->GetParameter = vid_dec_GetParameter;

   priv->screen = omx_get_screen();
   if (!priv->screen)
      return OMX_ErrorInsufficientResources;

   screen = priv->screen->pscreen;
   priv->pipe = screen->context_create(screen, priv->screen);
   if (!priv->pipe)
      return OMX_ErrorInsufficientResources;

   priv->sPortTypesParam[OMX_PortDomainVideo].nStartPortNumber = 0;
   priv->sPortTypesParam[OMX_PortDomainVideo].nPorts = 2;
   priv->ports = CALLOC(2, sizeof(omx_base_PortType *));
   if (!priv->ports)
      return OMX_ErrorInsufficientResources;

   for (i = 0; i < 2; ++i) {
      priv->ports[i] = CALLOC(1, sizeof(omx_base_video_PortType));
      if (!priv->ports[i])
         return OMX_ErrorInsufficientResources;

      base_video_port_Constructor(comp, &priv->ports[i], i, i == 0);
   }

   port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_INPUTPORT_INDEX];
   strcpy(port->sPortParam.format.video.cMIMEType,"video/MPEG2");
   port->sPortParam.nBufferCountMin = 8;
   port->sPortParam.nBufferCountActual = 8;
   port->sPortParam.nBufferSize = DEFAULT_OUT_BUFFER_SIZE;
   port->sPortParam.format.video.nFrameWidth = 176;
   port->sPortParam.format.video.nFrameHeight = 144;
   port->sPortParam.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG2;
   port->sVideoParam.eCompressionFormat = OMX_VIDEO_CodingMPEG2;
   port->Port_SendBufferFunction = vid_dec_DecodeBuffer;
   port->Port_FreeBuffer = vid_dec_FreeDecBuffer;

   port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_OUTPUTPORT_INDEX];
   port->sPortParam.nBufferCountActual = 8;
   port->sPortParam.nBufferCountMin = 4;
   port->sPortParam.format.video.nFrameWidth = 176;
   port->sPortParam.format.video.nFrameHeight = 144;
   port->sPortParam.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
   port->sVideoParam.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_dec_Destructor(OMX_COMPONENTTYPE *comp)
{
   vid_dec_PrivateType* priv = comp->pComponentPrivate;
   int i;

   if (priv->ports) {
      for (i = 0; i < priv->sPortTypesParam[OMX_PortDomainVideo].nPorts; ++i) {
         if(priv->ports[i])
            priv->ports[i]->PortDestructor(priv->ports[i]);
      }
      FREE(priv->ports);
      priv->ports=NULL;
   }

   if (priv->pipe)
      priv->pipe->destroy(priv->pipe);

   if (priv->screen)
      omx_put_screen();

   return omx_workaround_Destructor(comp);
}

static OMX_ERRORTYPE vid_dec_SetParameter(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR param)
{
   OMX_COMPONENTTYPE *comp = handle;
   vid_dec_PrivateType *priv = comp->pComponentPrivate;
   OMX_ERRORTYPE r;

   if (!param)
      return OMX_ErrorBadParameter;

   switch(idx) {
   case OMX_IndexParamPortDefinition: {
      OMX_PARAM_PORTDEFINITIONTYPE *def = param;

      r = omx_base_component_SetParameter(handle, idx, param);
      if (r)
         return r;

      if (def->nPortIndex == OMX_BASE_FILTER_INPUTPORT_INDEX) {
         omx_base_video_PortType *port;
         unsigned framesize = def->format.video.nFrameWidth * def->format.video.nFrameHeight;

         port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_INPUTPORT_INDEX];
         port->sPortParam.nBufferSize = framesize * 512 / (16*16);

         port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_OUTPUTPORT_INDEX];
         port->sPortParam.format.video.nFrameWidth = def->format.video.nFrameWidth;
         port->sPortParam.format.video.nFrameHeight = def->format.video.nFrameHeight;
         port->sPortParam.format.video.nStride = def->format.video.nFrameWidth;
         port->sPortParam.format.video.nSliceHeight = def->format.video.nFrameHeight;
         port->sPortParam.nBufferSize = framesize*3/2;

         priv->callbacks->EventHandler(comp, priv->callbackData, OMX_EventPortSettingsChanged,
                                       OMX_BASE_FILTER_OUTPUTPORT_INDEX, 0, NULL);
      }
      break;
   }
   case OMX_IndexParamStandardComponentRole: {
      OMX_PARAM_COMPONENTROLETYPE *role = param;

      r = checkHeader(param, sizeof(OMX_PARAM_COMPONENTROLETYPE));
      if (r)
         return r;
 
      if (!strcmp((char *)role->cRole, OMX_VID_DEC_MPEG2_ROLE)) {
         priv->profile = PIPE_VIDEO_PROFILE_MPEG2_MAIN;
      } else if (!strcmp((char *)role->cRole, OMX_VID_DEC_AVC_ROLE)) {
         priv->profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
      } else {
         return OMX_ErrorBadParameter;
      }

      break;
   }
   case OMX_IndexParamVideoPortFormat: {
      OMX_VIDEO_PARAM_PORTFORMATTYPE *format = param;
      omx_base_video_PortType *port;

      r = checkHeader(param, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
      if (r)
         return r;

      if (format->nPortIndex > 1)
         return OMX_ErrorBadPortIndex;

      port = (omx_base_video_PortType *)priv->ports[format->nPortIndex];
      memcpy(&port->sVideoParam, format, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
      break;
   }
   default:
      return omx_base_component_SetParameter(handle, idx, param);
   }
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_dec_GetParameter(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR param)
{
   OMX_COMPONENTTYPE *comp = handle;
   vid_dec_PrivateType *priv = comp->pComponentPrivate;
   OMX_ERRORTYPE r;

   if (!param)
      return OMX_ErrorBadParameter;

   switch(idx) {
   case OMX_IndexParamStandardComponentRole: {
      OMX_PARAM_COMPONENTROLETYPE *role = param;

      r = checkHeader(param, sizeof(OMX_PARAM_COMPONENTROLETYPE));
      if (r)
         return r;

      if (priv->profile == PIPE_VIDEO_PROFILE_MPEG2_MAIN)
         strcpy((char *)role->cRole, OMX_VID_DEC_MPEG2_ROLE);
      else if (priv->profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH)
         strcpy((char *)role->cRole, OMX_VID_DEC_AVC_ROLE);
 
      break;
   }

   case OMX_IndexParamVideoInit:
      r = checkHeader(param, sizeof(OMX_PORT_PARAM_TYPE));
      if (r)
         return r;

      memcpy(param, &priv->sPortTypesParam[OMX_PortDomainVideo], sizeof(OMX_PORT_PARAM_TYPE));
      break;

   case OMX_IndexParamVideoPortFormat: {
      OMX_VIDEO_PARAM_PORTFORMATTYPE *format = param;
      omx_base_video_PortType *port;

      r = checkHeader(param, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
      if (r)
         return r;

      if (format->nPortIndex > 1)
         return OMX_ErrorBadPortIndex;

      port = (omx_base_video_PortType *)priv->ports[format->nPortIndex];
      memcpy(format, &port->sVideoParam, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
      break;
   }

   default:
      return omx_base_component_GetParameter(handle, idx, param);

   }
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_dec_MessageHandler(OMX_COMPONENTTYPE* comp, internalRequestMessageType *msg)
{
   vid_dec_PrivateType* priv = comp->pComponentPrivate;

   if (msg->messageType == OMX_CommandStateSet) {
      if ((msg->messageParam == OMX_StateIdle ) && (priv->state == OMX_StateLoaded)) {

         struct pipe_video_codec templat = {};
         omx_base_video_PortType *port;

         port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_INPUTPORT_INDEX];

         templat.profile = priv->profile;
         templat.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
         templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
         templat.width = port->sPortParam.format.video.nFrameWidth;
         templat.height = port->sPortParam.format.video.nFrameHeight;
         templat.max_references = 2;
         templat.expect_chunked_decode = true;

         priv->codec = priv->pipe->create_video_codec(priv->pipe, &templat);

         if (priv->profile == PIPE_VIDEO_PROFILE_MPEG2_MAIN)
            vid_dec_mpeg12_Init(priv);
         else if (priv->profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH)
            vid_dec_h264_Init(priv);

      } else if ((msg->messageParam == OMX_StateLoaded) && (priv->state == OMX_StateIdle)) {
         if (priv->shadow) {
            priv->shadow->destroy(priv->shadow);
            priv->shadow = NULL;
         }
         if (priv->codec) {
            priv->codec->destroy(priv->codec);
            priv->codec = NULL;
         }
      }
   }

   return omx_base_component_MessageHandler(comp, msg);
}

void vid_dec_NeedTarget(vid_dec_PrivateType *priv)
{
   struct pipe_video_buffer templat = {};
   omx_base_video_PortType *port;

   port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_INPUTPORT_INDEX];

   if (!priv->target) {
      templat.buffer_format = PIPE_FORMAT_NV12;
      templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
      templat.width = port->sPortParam.format.video.nFrameWidth;
      templat.height = port->sPortParam.format.video.nFrameHeight;
      templat.interlaced = false;
      priv->target = priv->pipe->create_video_buffer(priv->pipe, &templat);
   }
}

static void vid_dec_FreeInputPortPrivate(OMX_BUFFERHEADERTYPE *buf)
{
   struct pipe_video_buffer *vbuf = buf->pInputPortPrivate;
   if (!vbuf)
      return;

   vbuf->destroy(vbuf);
   buf->pInputPortPrivate = NULL;
}

static OMX_ERRORTYPE vid_dec_DecodeBuffer(omx_base_PortType *port, OMX_BUFFERHEADERTYPE *buf)
{
   OMX_COMPONENTTYPE* comp = port->standCompContainer;
   vid_dec_PrivateType *priv = comp->pComponentPrivate;
   unsigned i = priv->num_in_buffers++;
   OMX_ERRORTYPE r;

   priv->in_buffers[i] = buf;
   priv->sizes[i] = buf->nFilledLen;
   priv->inputs[i] = buf->pBuffer;

   while (priv->num_in_buffers > (!!(buf->nFlags & OMX_BUFFERFLAG_EOS) ? 0 : 1)) {
      bool eos = !!(priv->in_buffers[0]->nFlags & OMX_BUFFERFLAG_EOS);
      unsigned min_bits_left = eos ? 32 : MAX2(buf->nFilledLen * 8, 32);
      struct vl_vlc vlc;

      vl_vlc_init(&vlc, priv->num_in_buffers, priv->inputs, priv->sizes);

      if (priv->slice)
         priv->bytes_left = vl_vlc_bits_left(&vlc) / 8;

      while (vl_vlc_bits_left(&vlc) > min_bits_left) {
         priv->Decode(priv, &vlc, min_bits_left);
         vl_vlc_fillbits(&vlc);
      }

      if (priv->slice) {
         unsigned bytes = priv->bytes_left - vl_vlc_bits_left(&vlc) / 8;

         priv->codec->decode_bitstream(priv->codec, priv->target, &priv->picture.base,
                                       1, &priv->slice, &bytes);

         if (priv->num_in_buffers)
            priv->slice = priv->inputs[1];
         else
            priv->slice = NULL;
      }

      if (eos && priv->frame_started)
         priv->EndFrame(priv);

      if (priv->frame_finished) {
         priv->frame_finished = false;
         priv->in_buffers[0]->nFilledLen = priv->in_buffers[0]->nAllocLen;
         r = base_port_SendBufferFunction(port, priv->in_buffers[0]);
      } else if (eos) {
         vid_dec_FreeInputPortPrivate(priv->in_buffers[0]);
         priv->in_buffers[0]->nFilledLen = priv->in_buffers[0]->nAllocLen;
         r = base_port_SendBufferFunction(port, priv->in_buffers[0]);
      } else {
         priv->in_buffers[0]->nFilledLen = 0;
         r = port->ReturnBufferFunction(port, priv->in_buffers[0]);
      }

      if (--priv->num_in_buffers) {
         unsigned delta = MIN2((min_bits_left - vl_vlc_bits_left(&vlc)) / 8, priv->sizes[1]);

         priv->in_buffers[0] = priv->in_buffers[1];
         priv->sizes[0] = priv->sizes[1] - delta;
         priv->inputs[0] = priv->inputs[1] + delta;
      }

      if (r)
         return r;
   }
 
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_dec_FreeDecBuffer(omx_base_PortType *port, OMX_U32 idx, OMX_BUFFERHEADERTYPE *buf)
{
   vid_dec_FreeInputPortPrivate(buf);
   return base_port_FreeBuffer(port, idx, buf);
}

static void vid_dec_FillOutput(vid_dec_PrivateType *priv, struct pipe_video_buffer *buf,
                               OMX_BUFFERHEADERTYPE* output)
{
   omx_base_PortType *port = priv->ports[OMX_BASE_FILTER_OUTPUTPORT_INDEX];
   OMX_VIDEO_PORTDEFINITIONTYPE *def = &port->sPortParam.format.video;

   struct pipe_sampler_view **views;
   struct pipe_transfer *transfer;
   struct pipe_box box = { };
   uint8_t *src, *dst;

   views = buf->get_sampler_view_planes(buf);

   dst = output->pBuffer;

   box.width = def->nFrameWidth;
   box.height = def->nFrameHeight;
   box.depth = 1;

   src = priv->pipe->transfer_map(priv->pipe, views[0]->texture, 0,
                                  PIPE_TRANSFER_READ, &box, &transfer);
   util_copy_rect(dst, views[0]->texture->format, def->nStride, 0, 0,
                  box.width, box.height, src, transfer->stride, 0, 0);
   pipe_transfer_unmap(priv->pipe, transfer);

   dst = ((uint8_t*)output->pBuffer) + (def->nStride * box.height);

   box.width = def->nFrameWidth / 2;
   box.height = def->nFrameHeight / 2;
 
   src = priv->pipe->transfer_map(priv->pipe, views[1]->texture, 0,
                                  PIPE_TRANSFER_READ, &box, &transfer);
   util_copy_rect(dst, views[1]->texture->format, def->nStride, 0, 0,
                  box.width, box.height, src, transfer->stride, 0, 0);
   pipe_transfer_unmap(priv->pipe, transfer);
}

static void vid_dec_FrameDecoded(OMX_COMPONENTTYPE *comp, OMX_BUFFERHEADERTYPE* input,
                                 OMX_BUFFERHEADERTYPE* output)
{
   vid_dec_PrivateType *priv = comp->pComponentPrivate;
   bool eos = !!(input->nFlags & OMX_BUFFERFLAG_EOS);

   if (!input->pInputPortPrivate)
      input->pInputPortPrivate = priv->Flush(priv);

   if (input->pInputPortPrivate) {
      if (output->pInputPortPrivate) {
         struct pipe_video_buffer *tmp = output->pOutputPortPrivate;
         output->pOutputPortPrivate = input->pInputPortPrivate;
         input->pInputPortPrivate = tmp;
      } else {
         vid_dec_FillOutput(priv, input->pInputPortPrivate, output);
      }
      output->nFilledLen = output->nAllocLen;
   }

   if (eos && input->pInputPortPrivate)
      vid_dec_FreeInputPortPrivate(input);
   else
      input->nFilledLen = 0;
}
