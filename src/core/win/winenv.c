#include "winenv.h"

#include <windows.h>

int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name) != NULL)
        return 0;
    if (SetEnvironmentVariableA(name, value))
        return 0;
    return -1;
}

int unsetenv(const char *name) {
    if (SetEnvironmentVariableA(name, NULL))
        return 0;
    return -1;
}