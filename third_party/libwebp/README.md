libwebp 1.6.0, decoder only, unchanged: the files upstream's Makefile.vc builds into
libwebpdecoder (DEC_OBJS, DSP_DEC_OBJS, UTILS_DEC_OBJS) and libwebpdemux (DEMUX_OBJS), with
their headers. The picker uses it to read 7TV's WebP previews (Windows' own decoders can't
read WebP everywhere).

- Source: https://chromium.googlesource.com/webm/libwebp (mirror
  https://github.com/webmproject/libwebp), tag v1.6.0, commit
  4fa21912338357f89e4fd51cf2368325b59e9bd9 (2025-07-07).
- License: BSD 3-clause (COPYING), plus Google's patent grant (PATENTS).
- build.cmd compiles it once into build\webp.lib; delete that file to compile it again.
