# Define the project metadata
project('LAB5', 'C', 'CPP',
	version: run_command(find_program('cat'),
		files('/opt/mellanox/doca/applications/VERSION'), 
		check: true).stdout().strip(),
	license: 'Proprietary',
	default_options: ['buildtype=debug'],
	meson_version: '>= 0.61.2'
)

LAB_NAME = 'lab5'

add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', 
language: ['c', 'cpp'])

# Declare dependencies
lab5_dependencies = []

lab5_dependencies += dependency('doca-common')

lab5_dependencies += dependency('doca-dma')

lab5_dependencies += dependency('doca-compress')

# Define source files
lab_srcs = [
	'logic.c',
	'main.c',
	'callbacks.c',
	'utils.c'
]

# Define include directories
lab_inc_dirs  = []

# Define build executable	
executable('doca_' + LAB_NAME, lab_srcs,
	c_args : '-Wno-missing-braces',
	dependencies : lab5_dependencies,
	include_directories: lab_inc_dirs,
	install: false)

