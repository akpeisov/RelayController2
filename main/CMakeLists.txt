idf_component_register(SRCS "hardware.c" "main.c"
                            "core.c"        
                            "rs485.c"  
                            "hardware.c"
                            "bconfig.c"                  
                       INCLUDE_DIRS "."
                       EMBED_FILES ../certs/jwt.pem
                                   ../html/config.html)
