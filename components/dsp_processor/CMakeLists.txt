set(COMPONENT_REQUIRES dsp_processor_settings)
set(COMPONENT_PRIV_REQUIRES esp-dsp lightsnapcast)

list(APPEND COMPONENT_ADD_INCLUDEDIRS ./include)
list(APPEND COMPONENT_ADD_INCLUDEDIRS ../dsp_processor_settings/include)
set(COMPONENT_SRCS ./dsp_processor.c)
register_component()
