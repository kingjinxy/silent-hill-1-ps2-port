#ifndef STRDEC_H
#define STRDEC_H

/** Decodes one STR video frame (its demuxed data: the video sectors' payloads in order) into RGB
 * (`width` x `height`, rows top to bottom; `bpp` 3: R, G, B; 4: R, G, B, 0x80 as the GS's 32-bit
 * format). Returns the bits used (>= 0) or a negative error: -1 not a frame, -2 unsupported version,
 * -3 bad bitstream. */
int StrDec_Frame(const unsigned char* data, int size, int width, int height, unsigned char* rgb, int bpp);

#endif
