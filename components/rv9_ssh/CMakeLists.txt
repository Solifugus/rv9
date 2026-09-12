idf_component_register(SRCS "src/drv_ssh.c" "src/ssh_trans.c" "src/ssh_crypto.c"
                       INCLUDE_DIRS "include"
                       REQUIRES rv9_io rv9_kal rv9_module
                       PRIV_REQUIRES mbedtls nvs_flash)
