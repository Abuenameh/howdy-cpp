project('howdy-auth', 'cpp', version: '3.0.0', default_options: ['cpp_std=c++2a'])

inih_cpp = dependency('INIReader', fallback: ['inih', 'INIReader_dep'])
dlib = dependency('dlib-1')
opencv = dependency('opencv4')
libevdev = dependency('libevdev')
add_global_arguments(['-Wno-unused', '-Wno-deprecated-enum-enum-conversion', '-Wno-sign-compare', '-Wno-bidi-chars'], language: 'cpp')

executable(
	'howdy-auth',
	'compare.cpp',
	'video_capture.cpp',
	'models.cpp',
	'snapshot.cpp',
	'rubber_stamps.cpp',
	'process/process.cpp',
	'process/process_unix.cpp',
	'keyboard/canonical_names.cpp',
	'keyboard/generic.cpp',
	'keyboard/keyboard_event.cpp',
	'keyboard/nix_common.cpp',
	'keyboard/nix_keyboard.cpp',
	'keyboard/keyboard.cpp',
	dependencies: [
		inih_cpp,
		dlib,
		opencv,
	]
)
