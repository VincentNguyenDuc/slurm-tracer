# Enabled plugins. An explicit list rather than a glob, so adding a plugin is a
# visible one-line change and CMake reconfigures when it happens.
#
# This is the only file outside a plugin's own directory that a new plugin
# touches. Nothing in src/core or src/runtime may name a plugin.

add_subdirectory(sinks/stdout_json)
