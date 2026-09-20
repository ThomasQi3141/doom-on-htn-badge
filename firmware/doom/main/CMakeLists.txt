idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES badge doom
)

# Bench override for co-op role selection, so a two-badge pairing test can be
# driven entirely over USB instead of needing a finger on a button at power-on.
#   idf.py -DBADGE_NET_FORCE_ROLE=1 build   # host
#   idf.py -DBADGE_NET_FORCE_ROLE=2 build   # join
#   idf.py -DBADGE_NET_FORCE_ROLE=0 build   # back to normal (the cache is sticky)
if(DEFINED BADGE_NET_FORCE_ROLE AND NOT BADGE_NET_FORCE_ROLE STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        BADGE_NET_FORCE_ROLE=${BADGE_NET_FORCE_ROLE})
endif()
