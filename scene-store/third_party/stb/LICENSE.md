stb_image.h (v2.30) and stb_image_write.h (v1.16)
from https://github.com/nothings/stb, vendored verbatim (MIT-style
dual license, see below; no modifications).

Adoption justification (per the project's create-our-own rule, which
demands a written justification for every third-party adoption):

The OS owns image decoding at the seam where user files enter the
scene: iso-photo, open-with, wallpapers. A production-grade decoder
for PNG (zlib/Deflate + filter chain + interlace), JPEG (DCT,
huffman, IDCT scaling) and GIF (LZW) is a real-media boundary:
these formats are defined by mature international standards and their
trustworthy implementations are the product of years of security
hardening (libpng CVE history is a textbook case). Rebuilding a
parser that must round-trip every conformant file in the wild at
equivalent trust is not achievable by an OS project whose point is
the semantic scene — this is the same class of boundary as a kernel
driver or a codec engine, where the rule itself says adoption is the
honest choice. stb_image is chosen because it is single-file (no
build machinery, no dynamic linking into the OS), public-domain/MIT
(no licensing drag on the ISO), and its failure surface is minimal:
it is used ONLY host-side in scene_image.c, never rides the wire,
never runs inside an app process, and feeds the same ARGB contract
as the in-house BMP/TGA decoders (which stay first-party, tested).
BMP/TGA remain in-house; stb handles exactly the world we cannot
reasonably own: PNG, JPEG, GIF.

License (covers both files):

This software is available under 2 licenses -- choose whichever you prefer.
--------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
--------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.