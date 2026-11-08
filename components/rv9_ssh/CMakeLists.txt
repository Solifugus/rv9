set(SSH_SRCS "")
if(CONFIG_RV9_DRV_SSH)
    set(SSH_SRCS "src/drv_ssh.c" "src/ssh_trans.c" "src/ssh_crypto.c"
                 "src/ssh_auth.c")
endif()

idf_component_register(SRCS ${SSH_SRCS}
                       INCLUDE_DIRS "include"
                       REQUIRES rv9_io rv9_kal rv9_module
                       PRIV_REQUIRES mbedtls nvs_flash)
