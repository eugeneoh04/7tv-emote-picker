// Decoding previews: WebP with libwebp (third_party/libwebp; Windows can't read WebP
// everywhere), PNG and GIF with Windows' own image decoders (WIC). Runs in the download threads.
// Pictures are scaled (high quality) to show `height` pixels tall, but never wider or taller
// than room x room; 0 means as-is / no limit.
#pragma once

#include "common.h"

// Premultiplied BGRA pixels, w per row.
struct Image {
    int w = 0, h = 0;
    bool by_room = false;  // the room limited its size, not the height asked for
    std::vector<uint32_t> px;
};

// False for a WebP that can't be read (anything else: true; whether it decodes shows later).
bool image_readable(const Bytes& data);

// A still preview (WebP or PNG; a WebP that moves gives its first frame).
bool decode_still(const Bytes& data, int height, int room, Image& out);

// An animated GIF: all its frames, layered the way the GIF says and scaled, laid out in one
// picture (atlas) so the screen can show any frame cheaply.
struct Animation {
    int w = 0, h = 0;          // one frame
    bool by_room = false;      // the room limited its size, not the height asked for
    int count = 0, cols = 1;   // frames, frames per row of the atlas
    Image atlas;
    std::vector<int> ends;     // when each frame ends, in ms from the start
};
// An animated WebP or GIF. False if it doesn't move (or it's too big to show).
bool decode_animation(const Bytes& data, int height, int room, int max_texture, Animation& out);
// The same for GIFs only: false if it's not a GIF that moves.
bool decode_gif(const Bytes& data, int height, int room, int max_texture, Animation& out);

// File mode's "auto" size: a GIF (every frame, with its timing) or PNG made height pixels tall,
// keeping its shape, as the same kind of file. False if it's neither, or already no taller.
bool resize_image(const Bytes& data, int height, Bytes& out);
