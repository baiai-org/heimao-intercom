/*
 * mp3.c -- single translation unit that instantiates the minimp3 decoder
 * (public domain, https://github.com/lieff/minimp3).  Used by voice_tail.c
 * to convert uploaded MP3 files into the 8 kHz mu-law custom tail tone.
 */
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
