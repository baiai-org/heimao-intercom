#pragma once
/* Only used by the isolated SpeexDSP component. */
#define OVERRIDE_SPEEX_ALLOC 1
#define OVERRIDE_SPEEX_FREE 1
#define speex_alloc orl_speex_alloc
void *orl_speex_alloc(int size);
static inline void speex_free(void *ptr) { (void)ptr; } /* arena ownership */
