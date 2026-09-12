# Warning policy: high on every compiler, errors by default.

function(engine_apply_warnings target)
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /Zc:__cplusplus
      /Zc:preprocessor
      /utf-8
      /w14242   # conversion, possible loss of data
      /w14254   # operator conversion, possible loss of data
      /w14263   # member function does not override any base class virtual member function
      /w14265   # class has virtual functions but destructor is not virtual
      /w14287   # unsigned/negative constant mismatch
      /w14296   # expression is always true/false
      /w14311   # pointer truncation
      /w14545 /w14546 /w14547 /w14549 /w14555   # suspicious comma/expression usage
      /w14619   # pragma warning: unknown warning number
      /w14640   # thread-unsafe static member initialization
      /w14826   # sign-extended conversion
      /w14905 /w14906   # string literal casts
      /w14928   # illegal copy-initialization
    )
    if(ENGINE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough
    )
    if(ENGINE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
