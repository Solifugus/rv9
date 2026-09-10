# The native kernel depends on nothing: no KAL, no host headers. It is
# handed a time source at init and has no other outside reference.
idf_component_register(SRCS "src/sched.c" "src/switch.S"
                       INCLUDE_DIRS "include")
