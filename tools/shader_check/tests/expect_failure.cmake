# Runs vxe_shader_check on one shader and passes only if the check fails with
# a message matching EXPECTED (a plain substring, not a regular expression).
# Arguments: -DTOOL=<checker> -DROOT=<shader root> -DSHADER=<path> -DEXPECTED=<text>

execute_process(
    COMMAND "${TOOL}" --root "${ROOT}" "${SHADER}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
if(result EQUAL 0)
    message(FATAL_ERROR "vxe_shader_check accepted the broken shader ${SHADER}:\n${output}")
endif()
string(FIND "${output}" "${EXPECTED}" position)
if(position EQUAL -1)
    message(FATAL_ERROR "vxe_shader_check failed, but its message does not contain '${EXPECTED}':\n${output}")
endif()
message(STATUS "vxe_shader_check rejected ${SHADER} as expected:\n${output}")
