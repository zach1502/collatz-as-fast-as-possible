# collatz_embed_source_expr(<file> <out_var> [PART_DIR <dir> PART_PREFIX <prefix>]):
# sets <out_var> in the caller's scope to a C++ expression whose value is the
# exact bytes of <file> as a std::string. The bytes are split into raw string
# literals of at most COLLATZ_EMBED_PIECE_MAX bytes, joined with operator+ at
# run time. Adjacent literals would be concatenated into one, so operator+
# keeps every literal below the 65536-byte limit of -Woverlength-strings
# (implied by -Wpedantic -Werror).
#
# With PART_DIR and PART_PREFIX, each line of <file> that reads
# `#include "<prefix>NAME"` is replaced by the bytes of <dir>/NAME, so one
# program text lives in several part files: the OpenCL compiler, which gets no
# include path, sees the expanded text, and host C++ includes the same parts.
# The parts are every file in <dir> with the extension of <file>, each included
# once and in name order, and they include no further parts. A part is a run of
# whole lines; the blank line before its first item opens the part, so the
# include lines stay one block, which clang-format sorts by name.

# Bytes per piece, below 16 KiB.
set(COLLATZ_EMBED_PIECE_MAX 16000)

# Sets <out_var> to the text of <file> with its part includes replaced, as
# described above, and stops the configure on any rule it breaks.
function(_collatz_embed_expand_parts file_path part_dir prefix out_var)
    file(READ "${file_path}" _rest)
    set(_needle "#include \"${prefix}")
    string(LENGTH "${_needle}" _needle_len)
    set(_text "")
    set(_names "")
    while(TRUE)
        string(FIND "${_rest}" "${_needle}" _pos)
        if(_pos EQUAL -1)
            break()
        endif()
        # A match that does not start a line is ordinary text.
        set(_at_line_start TRUE)
        if(_pos GREATER 0)
            math(EXPR _prev "${_pos} - 1")
            string(SUBSTRING "${_rest}" ${_prev} 1 _prev_char)
            if(NOT _prev_char STREQUAL "\n")
                set(_at_line_start FALSE)
            endif()
        endif()
        if(NOT _at_line_start)
            math(EXPR _skip "${_pos} + ${_needle_len}")
            string(SUBSTRING "${_rest}" 0 ${_skip} _before)
            string(APPEND _text "${_before}")
            string(SUBSTRING "${_rest}" ${_skip} -1 _rest)
            continue()
        endif()
        string(SUBSTRING "${_rest}" 0 ${_pos} _before)
        string(APPEND _text "${_before}")
        string(SUBSTRING "${_rest}" ${_pos} -1 _rest)
        string(FIND "${_rest}" "\n" _eol)
        if(_eol EQUAL -1)
            message(FATAL_ERROR "'${file_path}': the last part include has no line end")
        endif()
        math(EXPR _tail_len "${_eol} - ${_needle_len}")
        string(SUBSTRING "${_rest}" ${_needle_len} ${_tail_len} _tail)
        if(NOT _tail MATCHES "^([A-Za-z0-9_.-]+)\"$")
            message(FATAL_ERROR "'${file_path}': malformed part include '${_needle}${_tail}'")
        endif()
        set(_name "${CMAKE_MATCH_1}")
        set(_part "${part_dir}/${_name}")
        if(NOT EXISTS "${_part}")
            message(FATAL_ERROR "'${file_path}' includes '${_name}', which is not in '${part_dir}'")
        endif()
        file(READ "${_part}" _part_text)
        string(FIND "${_part_text}" "${_needle}" _nested)
        if(NOT _nested EQUAL -1)
            message(FATAL_ERROR "part '${_part}' includes another part; parts expand one level deep")
        endif()
        string(APPEND _text "${_part_text}")
        list(APPEND _names "${_name}")
        # Reconfigure, and so re-embed, when a part changes.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_part}")
        math(EXPR _next "${_eol} + 1")
        string(SUBSTRING "${_rest}" ${_next} -1 _rest)
    endwhile()
    string(APPEND _text "${_rest}")
    # CONFIGURE_DEPENDS reruns the check when a part file is added or removed.
    get_filename_component(_ext "${file_path}" LAST_EXT)
    file(GLOB _on_disk LIST_DIRECTORIES false RELATIVE "${part_dir}" CONFIGURE_DEPENDS
        "${part_dir}/*${_ext}")
    list(SORT _on_disk)
    if(NOT "${_names}" STREQUAL "${_on_disk}")
        message(FATAL_ERROR
            "'${file_path}' includes the parts [${_names}], but '${part_dir}' holds "
            "[${_on_disk}]; include every part once, in name order")
    endif()
    set(${out_var} "${_text}" PARENT_SCOPE)
endfunction()

function(collatz_embed_source_expr file_path out_var)
    cmake_parse_arguments(PARSE_ARGV 2 _arg "" "PART_DIR;PART_PREFIX" "")
    if(DEFINED _arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "collatz_embed_source_expr: unknown arguments ${_arg_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED _arg_PART_DIR OR DEFINED _arg_PART_PREFIX)
        if(NOT DEFINED _arg_PART_DIR OR NOT DEFINED _arg_PART_PREFIX)
            message(FATAL_ERROR "collatz_embed_source_expr: PART_DIR and PART_PREFIX go together")
        endif()
    endif()
    if(COLLATZ_EMBED_PIECE_MAX GREATER 16384)
        message(FATAL_ERROR
            "COLLATZ_EMBED_PIECE_MAX (${COLLATZ_EMBED_PIECE_MAX}) exceeds the 16 KiB piece limit")
    endif()
    if(DEFINED _arg_PART_DIR)
        _collatz_embed_expand_parts("${file_path}" "${_arg_PART_DIR}" "${_arg_PART_PREFIX}" _content)
    else()
        file(READ "${file_path}" _content)
    endif()
    # Reconfigure, and so re-embed, when the source file changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file_path}")
    # The raw-string delimiter must not occur in the source, where it would end
    # a literal early.
    string(FIND "${_content}" ")CLSRC\"" _delim_pos)
    if(NOT _delim_pos EQUAL -1)
        message(FATAL_ERROR
            "source '${file_path}' contains the raw-string delimiter )CLSRC\"")
    endif()
    string(LENGTH "${_content}" _len)
    set(_expr "")
    set(_pos 0)
    set(_first TRUE)
    while(_pos LESS _len)
        math(EXPR _remain "${_len} - ${_pos}")
        if(_remain LESS COLLATZ_EMBED_PIECE_MAX)
            set(_take ${_remain})
        else()
            set(_take ${COLLATZ_EMBED_PIECE_MAX})
        endif()
        # Stops the build if a piece exceeds the cap.
        if(_take GREATER COLLATZ_EMBED_PIECE_MAX)
            message(FATAL_ERROR
                "embed piece of ${_take} bytes from '${file_path}' exceeds "
                "COLLATZ_EMBED_PIECE_MAX (${COLLATZ_EMBED_PIECE_MAX})")
        endif()
        string(SUBSTRING "${_content}" ${_pos} ${_take} _piece)
        if(_first)
            set(_expr "::std::string(R\"CLSRC(${_piece})CLSRC\")")
            set(_first FALSE)
        else()
            set(_expr "${_expr}\n    + R\"CLSRC(${_piece})CLSRC\"")
        endif()
        math(EXPR _pos "${_pos} + ${_take}")
    endwhile()
    if(_first)
        set(_expr "::std::string()")
    endif()
    set(${out_var} "${_expr}" PARENT_SCOPE)
endfunction()
