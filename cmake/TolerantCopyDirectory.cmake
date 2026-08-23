# 容错目录拷贝：开发机上输入法 DLL 常驻宿主进程，其打开的皮肤/词库文件
# 无法被覆盖，整目录拷贝会因此失败并阻塞整个构建。逐文件复制，单个文件
# 被占用（或内容一致无需覆盖）时仅提示、不让构建失败。
#
# 用法：
#   file(COPY_FILE 语法要求 CMake >= 3.21)
#   include(${CMAKE_CURRENT_LIST_DIR}/TolerantCopyDirectory.cmake) 前需设置：
#     TOLERANT_COPY_SRC  源目录
#     TOLERANT_COPY_DST  目标目录

file(GLOB_RECURSE _tolerant_files RELATIVE "${TOLERANT_COPY_SRC}" "${TOLERANT_COPY_SRC}/*")
foreach(_rel_path IN LISTS _tolerant_files)
  set(_src_file "${TOLERANT_COPY_SRC}/${_rel_path}")
  set(_dst_file "${TOLERANT_COPY_DST}/${_rel_path}")
  if(IS_DIRECTORY "${_src_file}")
    continue()
  endif()
  get_filename_component(_dst_dir "${_dst_file}" DIRECTORY)
  file(MAKE_DIRECTORY "${_dst_dir}")
  if(EXISTS "${_dst_file}")
    file(SIZE "${_src_file}" _src_size)
    file(SIZE "${_dst_file}" _dst_size)
    if(_src_size EQUAL _dst_size)
      file(TIMESTAMP "${_src_file}" _src_mtime)
      file(TIMESTAMP "${_dst_file}" _dst_mtime)
      if(_src_mtime STREQUAL _dst_mtime)
        continue()
      endif()
    endif()
  endif()
  file(COPY_FILE "${_src_file}" "${_dst_file}" RESULT _copy_error)
  if(NOT _copy_error STREQUAL "")
    message(STATUS "runtime data copy: skipped in-use file ${_rel_path}")
  endif()
endforeach()
