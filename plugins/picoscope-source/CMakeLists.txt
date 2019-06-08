project(picoscope-source)

if(MSVC)
	set(picoscope-source_PLATFORM_DEPS
		w32-pthreads)
endif()

set(picoscope-source_SOURCES
	picoscope-source.c)

add_library(picoscope-source MODULE
	${picoscope-source_SOURCES})
target_link_libraries(picoscope-source
	libobs
	${picoscope-source_PLATFORM_DEPS})

install_obs_plugin(picoscope-source)
