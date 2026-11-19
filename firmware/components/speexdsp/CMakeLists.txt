idf_component_register(
    SRCS "orl_speex.c" "upstream/libspeexdsp/preprocess.c"
         "upstream/libspeexdsp/fftwrap.c" "upstream/libspeexdsp/filterbank.c"
         "upstream/libspeexdsp/kiss_fft.c" "upstream/libspeexdsp/kiss_fftr.c"
    INCLUDE_DIRS "." "upstream/include"
    PRIV_INCLUDE_DIRS "upstream/libspeexdsp")
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H)
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage -Wno-unused-parameter)
