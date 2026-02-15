idf_component_register(SRCS "snapclient.c" "connection_handler.c"
                       INCLUDE_DIRS "include"

                       PRIV_REQUIRES esp_timer net_functions opus flac network_interface settings_manager dsp_processor lightsnapcast
                       )
