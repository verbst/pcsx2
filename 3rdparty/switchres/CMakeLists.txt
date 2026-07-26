add_library(switchres STATIC
	switchres.cpp
	monitor.cpp
	modeline.cpp
	display.cpp
	custom_video.cpp
	log.cpp
	edid.cpp
	switchres_wrapper.cpp
)

# SR_CALC_ONLY: build switchres as a pure modeline *calculator*. It drops every
# real host-display backend (display_linux / display_windows / display_sdl2 and
# their X11 / DRM / ADL / ATI custom_video dependencies) and keeps only the
# "dummy" backend. We never switch the host's display — the computed modeline is
# sent to the MiSTer over UDP. Do not remove this; without it switchres links
# display-switching machinery that could reprogram the user's own monitor.
#
# SR_WIN32_STATIC: non-dllexport wrapper symbols, for static linking.
target_compile_definitions(switchres PUBLIC
	SR_CALC_ONLY
	SR_WIN32_STATIC
)

target_include_directories(switchres SYSTEM PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
)

disable_compiler_warnings_for_target(switchres)
