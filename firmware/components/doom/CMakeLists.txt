# The Doom engine, vendored from doomgeneric and stripped for the badge.
#
# Excluded outright, because the hardware cannot use them:
#   sound + music backends  - no speaker, amp or DAC anywhere on the board
#   mus2mid, memio, gusconf - MUS->MIDI conversion feeding a synth we do not have
#   i_joystick              - eight buttons on a shift register, no analogue axes
#   i_endoom                - DOS text screen shown on exit
#   i_scale                 - software scaler; we scale during palette conversion
#   sha1, w_checksum        - demo and netgame consistency checks
#   statdump                - end-of-level stats dumping for demo verification
#   w_file_stdc             - file I/O; the WAD is mapped from flash instead
#   icon, dummy             - SDL window icon and placeholder
#   doomgeneric_*           - the upstream platform backends; ours lives in main/
#
# badge_stubs.c keeps the call sites intact so the engine above is untouched.

file(GLOB DOOM_SRCS "src/*.c")

idf_component_register(
    SRCS ${DOOM_SRCS}
    INCLUDE_DIRS "src"
    REQUIRES badge esp_partition esp_timer
)

# 8bpp output at Doom's native resolution: the engine's paletted buffer is the
# one the SPI driver scans out, so there is no format conversion in between.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CMAP256
    DOOMGENERIC_RESX=320
    DOOMGENERIC_RESY=200
)

# Doom is 1993 C. These are upstream's warts, not ours, and burying our own
# diagnostics under thousands of lines of them helps nobody.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-implicit-fallthrough
    -Wno-sign-compare
    -Wno-format
    -Wno-char-subscripts
    -Wno-misleading-indentation
    -Wno-maybe-uninitialized
    -Wno-stringop-truncation
    -Wno-array-bounds
    -Wno-unused-but-set-parameter
    -Wno-dangling-pointer
    -Wno-old-style-declaration
    -Wno-discarded-qualifiers
    -Wno-int-conversion
    -Wno-incompatible-pointer-types
)
