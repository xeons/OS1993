/*
 * strerror.c -- error numbers as text, and KERNEL.DLL's errors as errno.
 */
#include "crt.h"
#include <string.h>

static const struct { int e; const char *text; } texts[] = {
    { 0, "No error" },
    { EPERM, "Operation not permitted" },        { ENOENT, "No such file or directory" },
    { ESRCH, "No such process" },                { EINTR, "Interrupted" },
    { EIO, "Input/output error" },               { E2BIG, "Argument list too long" },
    { ENOEXEC, "Not an executable" },            { EBADF, "Bad file descriptor" },
    { EAGAIN, "Resource temporarily unavailable" }, { ENOMEM, "Not enough memory" },
    { EACCES, "Access denied" },                 { EFAULT, "Bad address" },
    { EBUSY, "Device or resource busy" },        { EEXIST, "File exists" },
    { ENOTDIR, "Not a directory" },              { EISDIR, "Is a directory" },
    { EINVAL, "Invalid argument" },              { ENFILE, "Too many open files in system" },
    { EMFILE, "Too many open files" },           { EFBIG, "File too large" },
    { ENOSPC, "No space left on device" },       { ESPIPE, "Illegal seek" },
    { EROFS, "Read-only file system" },          { EPIPE, "Broken pipe" },
    { EDOM, "Argument out of domain" },          { ERANGE, "Result out of range" },
    { ENAMETOOLONG, "File name too long" },      { ENOSYS, "Function not implemented" },
    { ENOTEMPTY, "Directory not empty" },        { EILSEQ, "Illegal byte sequence" },
    { EOVERFLOW, "Value too large" },            { ENOTSOCK, "Not a socket" },
    { EDESTADDRREQ, "Destination address required" }, { EMSGSIZE, "Message too long" },
    { EPROTOTYPE, "Wrong protocol type for socket" }, { ENOPROTOOPT, "Protocol not available" },
    { EPROTONOSUPPORT, "Protocol not supported" }, { EOPNOTSUPP, "Operation not supported" },
    { EAFNOSUPPORT, "Address family not supported" }, { EADDRINUSE, "Address in use" },
    { EADDRNOTAVAIL, "Address not available" },  { ENETDOWN, "Network is down" },
    { ENETUNREACH, "Network unreachable" },      { ECONNABORTED, "Connection aborted" },
    { ECONNRESET, "Connection reset" },          { ENOBUFS, "No buffer space available" },
    { EISCONN, "Already connected" },            { ENOTCONN, "Not connected" },
    { ETIMEDOUT, "Timed out" },                  { ECONNREFUSED, "Connection refused" },
    { EHOSTDOWN, "Host is down" },               { EHOSTUNREACH, "No route to host" },
    { EALREADY, "Operation already in progress" }, { EINPROGRESS, "Operation in progress" },
    { ECANCELED, "Canceled" },
};

static const char *text_of(int e)
{
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++)
        if (texts[i].e == e)
            return texts[i].text;
    return "Unknown error";
}

char *strerror(int e) { return (char *)text_of(e); }

size_t strerrorlen_s(errno_t e) { return strlen(text_of(e)); }

errno_t strerror_s(char *d, rsize_t dmax, errno_t e)
{
    if (!d || !dmax || dmax > RSIZE_MAX) {
        __crt_constraint("strerror_s: d is null or dmax is 0 or too big", EINVAL);
        return EINVAL;
    }
    const char *t = text_of(e);
    size_t n = strlen(t);
    if (n < dmax) {
        memcpy(d, t, n + 1);
    } else {                                    /* cut short, ending "..." where it fits */
        memcpy(d, t, dmax - 1);
        d[dmax - 1] = '\0';
        for (size_t i = dmax > 4 ? dmax - 4 : dmax - 1; i < dmax - 1; i++)
            d[i] = '.';
    }
    return 0;
}

int __crt_errno_from_win(uint32_t err)
{
    switch (err) {
    case 0:                         return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:      return ENOENT;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_ACCESS_DENIED:       return EACCES;
    case ERROR_INVALID_HANDLE:      return EBADF;
    case ERROR_FILE_EXISTS:         return EEXIST;
    case ERROR_INVALID_NAME:        return ENOENT;
    case ERROR_NOACCESS:            return EFAULT;
    case ERROR_INVALID_PARAMETER:   return EINVAL;
    }
    return EIO;
}
