#include "gemini_protocol.h"

bool gemini_protocol_build_setup(char **output, size_t *output_len)
{
    if (output) *output = nullptr;
    if (output_len) *output_len = 0;
    return false;
}

bool gemini_protocol_process_message(const char *json, size_t len)
{
    (void)json;
    (void)len;
    return false;
}
