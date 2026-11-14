// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/texture_gl.h"

#include <epoxy/gl.h>
#include <epoxy/egl.h>

// EGLImage extension function pointers
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy, EGLImageKHR image);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);

// Define the extension functions
#ifndef eglCreateImageKHR
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
#endif
#ifndef eglDestroyImageKHR
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
#endif
#ifndef glEGLImageTargetTexture2DOES
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;
#endif

static void init_egl_image_extensions() {
  static gboolean initialized = FALSE;
  if (!initialized) {
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    initialized = TRUE;
  }
}

/* mpv renders on its own thread instead of inside Flutter's populate callback.
   Rendering in populate tied mpv's frame timing to Flutter's frame schedule:
   mpv's video thread waits up to 200 ms for each frame to be rendered, and
   while it waits mpv's core cannot answer the main thread, which is the only
   thread that can ask Flutter for the next frame. A main-thread mpv call that
   needs mpv's video thread (a property such as vo-passes, a screenshot) then
   froze the picture for 200 ms per call.

   The render thread draws into one of three textures: never the one Flutter
   is showing, never the one waiting to be shown. Flutter picks up the newest
   finished one. EGL fences order the two contexts: Flutter waits until mpv has
   finished drawing a texture, and mpv waits until Flutter has stopped sampling
   one before drawing into it again. Under X11 the raster thread has no EGL
   context to fence in, so that second ordering is missing there and the three
   textures are what keeps mpv clear of the one Flutter samples. */
#define TEXTURE_GL_SLOT_COUNT 3

typedef struct {
  /* mpv's side, owned by the render thread. */
  guint32 fbo;
  guint32 mpv_texture;
  EGLImageKHR egl_image;
  gint32 width;
  gint32 height;
  guint generation; /* Changes whenever |egl_image| is replaced. */
  gboolean needs_rebuild;
  /* Handed between the threads under |lock|. */
  EGLSyncKHR ready_fence;   /* Signals when mpv finished drawing. */
  EGLSyncKHR release_fence; /* Signals when Flutter stopped sampling. */
  /* Flutter's side, owned by the raster thread. */
  guint32 flutter_name;
  guint flutter_generation;
} TextureGLSlot;

struct _TextureGL {
  FlTextureGL parent_instance;
  VideoOutput* video_output;
  TextureGLSlot slots[TEXTURE_GL_SLOT_COUNT];
  guint32 placeholder_name; /* 1 x 1 texture shown before the first frame. */
  /* Kept here so populate never reaches into a VideoOutput being disposed. */
  EGLDisplay egl_display;
  GMutex lock;
  GCond cond;
  /* Guarded by |lock|. */
  gint ready;     /* Slot waiting for Flutter, or -1. */
  gint displayed; /* Slot Flutter shows, or -1. */
  gboolean render_requested;
  gboolean quit;
  gint last_rendered; /* Render thread only. */
  gint rendered_once; /* Set after the first frame mpv asked for. */
  GThread* thread;
  gint recreate_after_idle; /* Set from mpv's update callback. */
  gboolean has_fence_sync;  /* EGL_KHR_fence_sync */
  gboolean has_wait_sync;   /* EGL_KHR_wait_sync */
};

G_DEFINE_TYPE(TextureGL, texture_gl, fl_texture_gl_get_type())

static void texture_gl_init(TextureGL* self) {
  for (gint i = 0; i < TEXTURE_GL_SLOT_COUNT; i++) {
    TextureGLSlot* slot = &self->slots[i];
    slot->fbo = 0;
    slot->mpv_texture = 0;
    slot->egl_image = EGL_NO_IMAGE_KHR;
    slot->width = 0;
    slot->height = 0;
    slot->generation = 0;
    slot->needs_rebuild = FALSE;
    slot->ready_fence = EGL_NO_SYNC_KHR;
    slot->release_fence = EGL_NO_SYNC_KHR;
    slot->flutter_name = 0;
    slot->flutter_generation = 0;
  }
  self->placeholder_name = 0;
  self->egl_display = EGL_NO_DISPLAY;
  g_mutex_init(&self->lock);
  g_cond_init(&self->cond);
  self->ready = -1;
  self->displayed = -1;
  self->render_requested = FALSE;
  self->quit = FALSE;
  self->last_rendered = -1;
  self->rendered_once = 0;
  self->thread = NULL;
  self->video_output = NULL;
  self->recreate_after_idle = 0;
  self->has_fence_sync = FALSE;
  self->has_wait_sync = FALSE;
}

void texture_gl_recreate_after_idle(TextureGL* self) {
  if (self == NULL) {
    return;
  }
  g_atomic_int_set(&self->recreate_after_idle, 1);
}

static void texture_gl_destroy_fence(TextureGL* self, EGLSyncKHR* fence) {
  if (*fence != EGL_NO_SYNC_KHR) {
    eglDestroySyncKHR(self->egl_display, *fence);
    *fence = EGL_NO_SYNC_KHR;
  }
}

/* Makes the GPU, or failing that the calling thread, wait for |fence|, then
   destroys it. A server-side wait needs an EGL context current on the calling
   thread, which Flutter's raster thread lacks under X11 (GTK uses GLX). */
static void texture_gl_wait_fence(TextureGL* self, EGLSyncKHR* fence) {
  if (*fence == EGL_NO_SYNC_KHR) {
    return;
  }
  if (self->has_wait_sync && eglGetCurrentContext() != EGL_NO_CONTEXT) {
    eglWaitSyncKHR(self->egl_display, *fence, 0);
  } else {
    eglClientWaitSyncKHR(self->egl_display, *fence,
                         EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                         100 * 1000 * 1000 /* 100 ms */);
  }
  texture_gl_destroy_fence(self, fence);
}

/* A fence after everything issued so far in the current context, or none when
   the current context isn't an EGL one. */
static EGLSyncKHR texture_gl_create_fence(TextureGL* self) {
  if (!self->has_fence_sync || eglGetCurrentContext() == EGL_NO_CONTEXT) {
    return EGL_NO_SYNC_KHR;
  }
  EGLSyncKHR fence =
      eglCreateSyncKHR(self->egl_display, EGL_SYNC_FENCE_KHR, NULL);
  glFlush();
  return fence;
}

/* Render thread only, with mpv's context current. */
static void texture_gl_free_slot_gl(TextureGL* self, TextureGLSlot* slot) {
  if (slot->egl_image != EGL_NO_IMAGE_KHR) {
    eglDestroyImageKHR(self->egl_display, slot->egl_image);
    slot->egl_image = EGL_NO_IMAGE_KHR;
  }
  if (slot->mpv_texture != 0) {
    glDeleteTextures(1, &slot->mpv_texture);
    slot->mpv_texture = 0;
  }
  if (slot->fbo != 0) {
    glDeleteFramebuffers(1, &slot->fbo);
    slot->fbo = 0;
  }
  // The size stays: Flutter's texture keeps the image's storage, and a slot
  // still shown during teardown goes on reporting it.
}

/* Render thread only, with mpv's context current. */
static void texture_gl_create_slot_gl(TextureGL* self,
                                      TextureGLSlot* slot,
                                      gint32 width,
                                      gint32 height) {
  // Shared by every render thread, so atomic.
  static gint next_generation = 0;
  texture_gl_free_slot_gl(self, slot);

  glGenFramebuffers(1, &slot->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, slot->fbo);

  glGenTextures(1, &slot->mpv_texture);
  glBindTexture(GL_TEXTURE_2D, slot->mpv_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, NULL);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         slot->mpv_texture, 0);

  EGLint egl_image_attribs[] = {EGL_NONE};
  slot->egl_image = eglCreateImageKHR(
      self->egl_display, video_output_get_egl_context(self->video_output),
      EGL_GL_TEXTURE_2D_KHR,
      (EGLClientBuffer)(guintptr)slot->mpv_texture, egl_image_attribs);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  slot->width = width;
  slot->height = height;
  slot->generation = (guint)g_atomic_int_add(&next_generation, 1) + 1;
  slot->needs_rebuild = FALSE;
}

static gpointer texture_gl_render_thread(gpointer data) {
  TextureGL* self = TEXTURE_GL(data);
  VideoOutput* video_output = self->video_output;
  EGLDisplay display = video_output_get_egl_display(video_output);
  EGLContext context = video_output_get_egl_context(video_output);
  mpv_render_context* render_context =
      video_output_get_render_context(video_output);

  // The bound API is per thread.
  eglBindAPI(EGL_OPENGL_ES_API);
  if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
    g_printerr(
        "media_kit: TextureGL: Render thread could not make mpv's EGL context "
        "current. Error: 0x%x\n",
        eglGetError());
    return NULL;
  }

  gint32 reported_width = 0;
  gint32 reported_height = 0;

  while (TRUE) {
    g_mutex_lock(&self->lock);
    while (!self->render_requested && !self->quit) {
      g_cond_wait(&self->cond, &self->lock);
    }
    if (self->quit) {
      g_mutex_unlock(&self->lock);
      break;
    }
    self->render_requested = FALSE;
    // Three slots, so one is always neither shown nor waiting to be shown.
    // Going round them in turn leaves the one Flutter just let go of alone for
    // a frame, which is all the ordering X11 gets.
    gint index = self->last_rendered;
    do {
      index = (index + 1) % TEXTURE_GL_SLOT_COUNT;
    } while (index == self->displayed || index == self->ready);
    self->last_rendered = index;
    TextureGLSlot* slot = &self->slots[index];
    EGLSyncKHR release_fence = slot->release_fence;
    slot->release_fence = EGL_NO_SYNC_KHR;
    // Left over when the slot was waiting and a newer frame replaced it.
    EGLSyncKHR stale_ready_fence = slot->ready_fence;
    slot->ready_fence = EGL_NO_SYNC_KHR;
    g_mutex_unlock(&self->lock);

    texture_gl_wait_fence(self, &release_fence);
    texture_gl_destroy_fence(self, &stale_ready_fence);

    mpv_render_context_update(render_context);

    gint width = 0;
    gint height = 0;
    video_output_get_render_size(video_output, &width, &height);
    if (width <= 0 || height <= 0) {
      // mpv's core can only be asked on the main thread. Whatever mpv queued
      // waits for the next update, which the size lookup triggers.
      video_output_request_render_size(video_output);
      continue;
    }

    if (g_atomic_int_compare_and_exchange(&self->recreate_after_idle, 1, 0)) {
      g_print("media_kit: TextureGL: Recreating EGLImage after render idle.\n");
      // Each one is rebuilt the next time it is drawn into, including the
      // one Flutter shows now.
      for (gint i = 0; i < TEXTURE_GL_SLOT_COUNT; i++) {
        self->slots[i].needs_rebuild = TRUE;
      }
    }

    if (slot->fbo == 0 || slot->needs_rebuild || slot->width != width ||
        slot->height != height) {
      texture_gl_create_slot_gl(self, slot, width, height);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, slot->fbo);
    mpv_opengl_fbo fbo{(gint32)slot->fbo, width, height, 0};
    int flip_y = 0;
    int block_for_target_time = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    mpv_render_context_render(render_context, params);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    EGLSyncKHR ready_fence = texture_gl_create_fence(self);
    if (ready_fence == EGL_NO_SYNC_KHR) {
      // Without a fence Flutter could sample a half drawn frame.
      glFinish();
    }

    g_mutex_lock(&self->lock);
    const gboolean quitting = self->quit;
    if (!quitting) {
      slot->ready_fence = ready_fence;
      self->ready = index;
    }
    g_mutex_unlock(&self->lock);

    // Being stopped: populate may still run until the texture is unregistered,
    // so this frame isn't offered, and a post would take a reference to a
    // VideoOutput being disposed.
    if (quitting) {
      texture_gl_destroy_fence(self, &ready_fence);
      break;
    }
    g_atomic_int_set(&self->rendered_once, 1);
    if (width != reported_width || height != reported_height) {
      reported_width = width;
      reported_height = height;
      video_output_post_texture_update(video_output);
    }
    video_output_post_frame_available(video_output);
  }

  // Populate can still run until the texture is unregistered. Nothing is left
  // for it to pick up, and the fences are taken out of its reach first.
  EGLSyncKHR release_fences[TEXTURE_GL_SLOT_COUNT];
  EGLSyncKHR ready_fences[TEXTURE_GL_SLOT_COUNT];
  g_mutex_lock(&self->lock);
  self->ready = -1;
  for (gint i = 0; i < TEXTURE_GL_SLOT_COUNT; i++) {
    release_fences[i] = self->slots[i].release_fence;
    self->slots[i].release_fence = EGL_NO_SYNC_KHR;
    ready_fences[i] = self->slots[i].ready_fence;
    self->slots[i].ready_fence = EGL_NO_SYNC_KHR;
  }
  g_mutex_unlock(&self->lock);
  for (gint i = 0; i < TEXTURE_GL_SLOT_COUNT; i++) {
    texture_gl_wait_fence(self, &release_fences[i]);
    texture_gl_destroy_fence(self, &ready_fences[i]);
    texture_gl_free_slot_gl(self, &self->slots[i]);
  }
  glFinish();
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return NULL;
}

void texture_gl_start(TextureGL* self) {
  if (self->thread != NULL) {
    return;
  }
  self->egl_display = video_output_get_egl_display(self->video_output);
  self->has_fence_sync =
      epoxy_has_egl_extension(self->egl_display, "EGL_KHR_fence_sync");
  self->has_wait_sync =
      epoxy_has_egl_extension(self->egl_display, "EGL_KHR_wait_sync");
  if (!self->has_fence_sync) {
    g_print("media_kit: TextureGL: No EGL_KHR_fence_sync, using glFinish.\n");
  }
  self->thread =
      g_thread_new("media_kit_video_render", texture_gl_render_thread, self);
}

void texture_gl_request_render(TextureGL* self) {
  g_mutex_lock(&self->lock);
  self->render_requested = TRUE;
  g_cond_signal(&self->cond);
  g_mutex_unlock(&self->lock);
}

void texture_gl_request_redraw(TextureGL* self) {
  // Before mpv's first frame a render would draw a blank one and report it to
  // Dart as the first frame rendered.
  if (g_atomic_int_get(&self->rendered_once)) {
    texture_gl_request_render(self);
  }
}

void texture_gl_stop(TextureGL* self) {
  if (self->thread == NULL) {
    return;
  }
  g_mutex_lock(&self->lock);
  self->quit = TRUE;
  g_cond_signal(&self->cond);
  g_mutex_unlock(&self->lock);
  g_thread_join(self->thread);
  self->thread = NULL;
}

static void texture_gl_dispose(GObject* object) {
  TextureGL* self = TEXTURE_GL(object);
  texture_gl_stop(self);

  // Flutter's textures. The EGLImages behind them went with the render thread.
  for (gint i = 0; i < TEXTURE_GL_SLOT_COUNT; i++) {
    // Populate can still hand back a fence between the stop and unregister.
    texture_gl_destroy_fence(self, &self->slots[i].release_fence);
    texture_gl_destroy_fence(self, &self->slots[i].ready_fence);
    if (self->slots[i].flutter_name != 0) {
      glDeleteTextures(1, &self->slots[i].flutter_name);
      self->slots[i].flutter_name = 0;
    }
  }
  if (self->placeholder_name != 0) {
    glDeleteTextures(1, &self->placeholder_name);
    self->placeholder_name = 0;
  }

  self->video_output = NULL;
  G_OBJECT_CLASS(texture_gl_parent_class)->dispose(object);
}

static void texture_gl_finalize(GObject* object) {
  TextureGL* self = TEXTURE_GL(object);
  g_cond_clear(&self->cond);
  g_mutex_clear(&self->lock);
  G_OBJECT_CLASS(texture_gl_parent_class)->finalize(object);
}

static void texture_gl_class_init(TextureGLClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = texture_gl_populate_texture;
  G_OBJECT_CLASS(klass)->dispose = texture_gl_dispose;
  G_OBJECT_CLASS(klass)->finalize = texture_gl_finalize;
}

TextureGL* texture_gl_new(VideoOutput* video_output) {
  init_egl_image_extensions();
  TextureGL* self = TEXTURE_GL(g_object_new(texture_gl_get_type(), NULL));
  self->video_output = video_output;
  return self;
}

gboolean texture_gl_populate_texture(FlTextureGL* texture,
                                     guint32* target,
                                     guint32* name,
                                     guint32* width,
                                     guint32* height,
                                     GError** error) {
  TextureGL* self = TEXTURE_GL(texture);

  g_mutex_lock(&self->lock);
  EGLSyncKHR ready_fence = EGL_NO_SYNC_KHR;
  if (self->ready >= 0) {
    const gint previous = self->displayed;
    self->displayed = self->ready;
    self->ready = -1;
    ready_fence = self->slots[self->displayed].ready_fence;
    self->slots[self->displayed].ready_fence = EGL_NO_SYNC_KHR;
    if (previous >= 0) {
      // Everything that sampled the previous frame has been issued by now.
      // Made under the lock: once |displayed| moves on, the render thread may
      // pick that slot, and it has to find the fence there.
      self->slots[previous].release_fence = texture_gl_create_fence(self);
    }
  }
  const gint displayed = self->displayed;
  g_mutex_unlock(&self->lock);

  *target = GL_TEXTURE_2D;

  if (displayed < 0) {
    // First frame not yet available.
    if (self->placeholder_name == 0) {
      glGenTextures(1, &self->placeholder_name);
      glBindTexture(GL_TEXTURE_2D, self->placeholder_name);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, NULL);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
    *name = self->placeholder_name;
    *width = 1;
    *height = 1;
    return TRUE;
  }

  texture_gl_wait_fence(self, &ready_fence);

  // The render thread leaves a shown slot alone, so it is safe to read here.
  TextureGLSlot* slot = &self->slots[displayed];
  if (slot->flutter_name == 0 || slot->flutter_generation != slot->generation) {
    if (slot->flutter_name != 0) {
      glDeleteTextures(1, &slot->flutter_name);
    }
    glGenTextures(1, &slot->flutter_name);
    glBindTexture(GL_TEXTURE_2D, slot->flutter_name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, slot->egl_image);
    glBindTexture(GL_TEXTURE_2D, 0);
    slot->flutter_generation = slot->generation;
  }

  *name = slot->flutter_name;
  *width = slot->width;
  *height = slot->height;
  return TRUE;
}
