set(KALAM_LEGACY_QT_SOURCE_DIR "" CACHE PATH "Path to the legacy Qt editor/qt source directory")
if(NOT EXISTS "${KALAM_LEGACY_QT_SOURCE_DIR}/neovim_grid_widget.cpp")
  message(FATAL_ERROR "Set KALAM_LEGACY_QT_SOURCE_DIR to the legacy Qt editor/qt directory")
endif()
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Gui Widgets)
add_executable(kalam-editor main.cpp entity_inspector.cpp json_views.cpp ../native/host.c ../native/render.c)
target_compile_definitions(kalam-editor PRIVATE KALAM_EMBEDDED)
target_compile_features(kalam-editor PRIVATE cxx_std_20 c_std_11)
target_include_directories(kalam-editor PRIVATE "${KALAM_LEGACY_QT_SOURCE_DIR}")
foreach(unit grid_widget grid_model input_translation msgpack rpc_client font_names)
  target_sources(kalam-editor PRIVATE
    "${KALAM_LEGACY_QT_SOURCE_DIR}/neovim_${unit}.cpp"
    "${KALAM_LEGACY_QT_SOURCE_DIR}/neovim_${unit}.h")
endforeach()
target_link_libraries(kalam-editor PRIVATE Qt6::Widgets SDL3::SDL3 ${kh_bgfx}
  "${KALAM_RUST_LIB}" ws2_32 userenv bcrypt ntdll synchronization)
set_target_properties(kalam-editor PROPERTIES
  AUTOMOC ON RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
qt_add_resources(kalam-editor "kalam_editor" PREFIX "/kalam" FILES viewport.lua entity-controls.json editor.qss)
