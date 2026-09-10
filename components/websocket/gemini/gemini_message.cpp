#include "gemini_message.h"

 gemini_message_type_t gemini_message_classify(const char *json, size_t len)
{
    (void)json;
    (void)len;
    return GEMINI_MESSAGE_UNKNOWN;
}
