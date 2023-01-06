#include "aducpal/stdio.h"

#ifdef ADUCPAL_USE_PAL

#    include <direct.h> // _rmdir
#    include <io.h> // _unlink

#    include <errno.h> // ENOTDIR, _get_errno

FILE* ADUCPAL_popen(const char* command, const char* type)
{
    __debugbreak();
    return NULL;
}

int ADUCPAL_pclose(FILE* stream)
{
    __debugbreak();
    return -1;
}

int ADUCPAL_remove(const char* pathname)
{
    int ret = 0;

    // Unlike Windows implementation, remove() deletes a name from the file system.
    // It calls unlink(2) for files, and rmdir(2) for directories.

    if (_rmdir(pathname) == 0)
    {
        ret = 0;
        goto done;
    }

    errno_t errno;
    _get_errno(&errno);
    if (errno != ENOTDIR)
    {
        // Failed deleting directory
        ret = -1;
        goto done;
    }

    // Assume it's a file.

    if (_unlink(pathname) == 0)
    {
        ret = 0;
        goto done;
    }

    // Failed
    ret = -1;

done:
    return ret;
}

int ADUCPAL_rename(const char* old_f, const char* new_f)
{
    // If the link named by the new argument exists, it shall be removed and old renamed to new.
    int ret = _unlink(new_f);
    errno_t errno;
    _get_errno(&errno);

    return rename(old_f, new_f);
}

#endif // #ifdef ADUCPAL_USE_PAL