// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#ifndef TEXTURE_GL_H_
#define TEXTURE_GL_H_

#include <flutter_linux/flutter_linux.h>

#include "video_output.h"

#define TEXTURE_GL_TYPE (texture_gl_get_type())

G_DECLARE_FINAL_TYPE(TextureGL, texture_gl, TEXTURE_GL, TEXTURE_GL, FlTextureGL)

#define TEXTURE_GL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), texture_gl_get_type(), TextureGL))

TextureGL* texture_gl_new(VideoOutput* video_output);

/**
 * @brief Marks every texture for rebuild the next time it is drawn into.
 *
 * Safe from any thread, including mpv's update callback.
 */
void texture_gl_recreate_after_idle(TextureGL* self);

/**
 * @brief Starts the thread that renders mpv's frames.
 *
 * Call on the main thread once the mpv render context exists and mpv's EGL
 * context is no longer current anywhere. The thread makes it current and keeps
 * it until |texture_gl_stop|.
 */
void texture_gl_start(TextureGL* self);

/**
 * @brief Asks the render thread for a frame. Safe from any thread, including
 * mpv's update callback.
 */
void texture_gl_request_render(TextureGL* self);

/**
 * @brief Like |texture_gl_request_render|, but only once mpv has produced a
 * frame. For redraws the app asks for, such as a new size.
 */
void texture_gl_request_redraw(TextureGL* self);

/**
 * @brief Stops the render thread and frees the GL objects it owns. Returns once
 * mpv's EGL context is no longer current on it. Call on the main thread before
 * freeing the mpv render context.
 */
void texture_gl_stop(TextureGL* self);

/**
 * @brief Populates texture with video frame.
 */
gboolean texture_gl_populate_texture(FlTextureGL* texture,
                                     guint32* target,
                                     guint32* name,
                                     guint32* width,
                                     guint32* height,
                                     GError** error);

#endif  // TEXTURE_GL_H_
