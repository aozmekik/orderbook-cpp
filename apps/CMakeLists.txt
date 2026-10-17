foreach(tool IN ITEMS obreplay obgen)
  add_executable(${tool} ${tool}.cpp)
  target_link_libraries(${tool} PRIVATE orderbook::orderbook)
  ob_enable_warnings(${tool})
endforeach()

if(OB_INSTALL)
  install(TARGETS obreplay obgen)
endif()
