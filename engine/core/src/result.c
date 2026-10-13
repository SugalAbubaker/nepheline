// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/result.h>

const char *nph_result_string(NphResult r)
{
    switch (r) {
    case NPH_RESULT_OK:
        return "OK";
    case NPH_RESULT_OUT_OF_MEMORY:
        return "OUT_OF_MEMORY";
    case NPH_RESULT_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case NPH_RESULT_NOT_FOUND:
        return "NOT_FOUND";
    case NPH_RESULT_ALREADY_EXISTS:
        return "ALREADY_EXISTS";
    case NPH_RESULT_FULL:
        return "FULL";
    case NPH_RESULT_EMPTY:
        return "EMPTY";
    case NPH_RESULT_OVERFLOW:
        return "OVERFLOW";
    case NPH_RESULT_UNSUPPORTED:
        return "UNSUPPORTED";
    case NPH_RESULT_CORRUPT:
        return "CORRUPT";
    case NPH_RESULT_VERSION_MISMATCH:
        return "VERSION_MISMATCH";
    case NPH_RESULT_TYPE_MISMATCH:
        return "TYPE_MISMATCH";
    case NPH_RESULT_PARSE_ERROR:
        return "PARSE_ERROR";
    case NPH_RESULT_END_OF_STREAM:
        return "END_OF_STREAM";
    case NPH_RESULT_IO_ERROR:
        return "IO_ERROR";
    case NPH_RESULT_ACCESS_DENIED:
        return "ACCESS_DENIED";
    case NPH_RESULT_BUSY:
        return "BUSY";
    case NPH_RESULT_DEVICE_LOST:
        return "DEVICE_LOST";
    case NPH_RESULT_UNKNOWN:
        return "UNKNOWN";
    case NPH_RESULT_COUNT:
        break;
    }
    return "INVALID_RESULT";
}
