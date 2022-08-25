project('howdy', 'cpp', version: '3.0.0', default_options: ['cpp_std=c++2a'])

inih_cpp = dependency('INIReader', fallback: ['inih', 'INIReader_dep'])
dlib = dependency('dlib-1')
opencv = dependency('opencv4')
libevdev = dependency('libevdev')
add_global_arguments(['-Wno-unused', '-Wno-deprecated-enum-enum-conversion', '-Wno-sign-compare'], language: 'cpp')

executable(
	'howdy',
	'cli.cpp',
	'add.cpp',
	'clear.cpp',
	'config.cpp',
	'disable.cpp',
	'list.cpp',
	'remove.cpp',
	'set.cpp',
	'snap.cpp',
	'test.cpp',
	'../models.cpp',
	'../video_capture.cpp',
	'../snapshot.cpp',
	dependencies: [
		inih_cpp,
		dlib,
		opencv,
	]
)
