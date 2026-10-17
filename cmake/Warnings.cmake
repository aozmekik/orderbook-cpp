# Strict warning flags for the project's own targets (never for third-party code).

function(ob_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(OB_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    return()
  endif()

  target_compile_options(
    ${target}
    PRIVATE -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wold-style-cast
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wcast-align
            -Wimplicit-fallthrough
            -Wmissing-declarations
            -Wundef)
  if(OB_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
