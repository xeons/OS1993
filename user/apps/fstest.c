/*
 * fstest.c -- tests of the VMM's own disks: FAT32 (D:), FAT16 (E:),
 * exFAT (F:), ext2 (G:, 1 KB blocks; H:, 8 KB), ext3 needing recovery (I:)
 * on one disk, and a 5 GB exFAT disk with 4 KB sectors (J:), reached directly through
 * KERNEL.DLL's file calls, long names and all.
 *
 * tools/qemu-fat-test.py builds the disk (tools/mkdata.sh), lets a DOS
 * session work on it through the redirector first (E:\DOSTEST.BAT), then
 * runs this: it reads what mtools and DOS wrote, writes its own, and
 * leaves D:\FROMGUI.TXT and E:\GUIDIR for the script to find on the image
 * afterwards. One line per failure and a total in the debug log.
 */
#include <stdio.h>
#include <string.h>
#include "os1993.h"

static int failures, checks;

#define CHECK(cond, what) \
    do { checks++; if (!(cond)) { failures++; printf("FS: FAIL %s (line %d, error %lu)\n", what, __LINE__, \
                                                   (unsigned long)GetLastError()); } } while (0)

static uint8_t big[300000], buf[300000];

static uint8_t pattern(uint32_t i) { return (uint8_t)((i * 7 + i / 251) & 255); }

/* The whole file, or -1. */
static int slurp(const char *path, void *out, uint32_t max)
{
    HANDLE h = CreateFile(path, GENERIC_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    uint32_t got = 0;
    bool ok = ReadFile(h, out, max, &got);
    CloseHandle(h);
    return ok ? (int)got : -1;
}

static bool spit(const char *path, const void *data, uint32_t n)
{
    HANDLE h = CreateFile(path, GENERIC_WRITE, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    uint32_t put = 0;
    bool ok = WriteFile(h, data, n, &put) && put == n;
    return CloseHandle(h) && ok;
}

static bool text_is(const char *path, const char *want)
{
    char t[256];
    int n = slurp(path, t, sizeof t - 1);
    if (n < 0)
        return false;
    t[n] = '\0';
    return strcmp(t, want) == 0;
}

/* Is there an entry called name (and with this alias, if given)? */
static bool listed(const char *pattern, const char *name, const char *alias, uint32_t *attr)
{
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    do {
        if (!strcmp(fd.name, name) && (!alias || !strcmp(fd.short_name, alias))) {
            found = true;
            if (attr)
                *attr = fd.attrib;
        }
    } while (!found && FindNextFile(f, &fd));
    FindClose(f);
    return found;
}

static int count(const char *pattern)
{
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pattern, &fd);
    int n = 0;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do
        n += strcmp(fd.name, ".") && strcmp(fd.name, "..");
    while (FindNextFile(f, &fd));
    FindClose(f);
    return n;
}

static void test_reading(void)
{
    CHECK(GetFileAttributes("D:\\") & FILE_ATTRIBUTE_DIRECTORY, "D:\\ is a directory");
    CHECK(text_is("D:\\HELLO.TXT", "Hello from FAT32.\r\n"), "read D:\\HELLO.TXT (mtools wrote it)");
    CHECK(text_is("D:\\A long file name.txt", "A file with a long name.\r\n"), "read a file by its long name");
    CHECK(text_is("d:\\a LONG file NAME.TXT", "A file with a long name.\r\n"), "... in any case");
    CHECK(text_is("D:\\ALONGF~1.TXT", "A file with a long name.\r\n"), "... and by its 8.3 alias");
    CHECK(text_is("D:\\Sub Folder\\Deeper\\Inside File.txt", "Deep inside.\r\n"), "a long path, three deep");
    CHECK(listed("D:\\*", "A long file name.txt", "ALONGF~1.TXT", NULL), "FindFirstFile: the long name and its alias");
    uint32_t attr = 0;
    CHECK(listed("D:\\*.*", "Sub Folder", NULL, &attr) && (attr & FILE_ATTRIBUTE_DIRECTORY), "... a folder with a long name");
    CHECK(listed("D:\\Sub*", "Sub Folder", NULL, NULL) && !listed("D:\\Sub*", "HELLO.TXT", NULL, NULL),
          "... patterns match long names");
    CHECK(text_is("E:\\README.TXT", "FAT16 here.\r\n"), "read E:\\README.TXT (FAT16)");

    for (uint32_t i = 0; i < sizeof big; i++)
        big[i] = pattern(i);
    HANDLE h = CreateFile("D:\\BIG.BIN", GENERIC_READ, OPEN_EXISTING);
    CHECK(h != INVALID_HANDLE_VALUE && GetFileSize(h) == sizeof big, "D:\\BIG.BIN: 300000 bytes");
    uint32_t at = 0, got;
    bool same = true;
    while (at < sizeof big) {                           /* odd sizes: partial and whole clusters */
        uint32_t want = at % 3 ? 7001 : 65536;
        if (!ReadFile(h, buf + at, want, &got) || !got)
            break;
        at += got;
    }
    same = at == sizeof big && !memcmp(buf, big, sizeof big);
    CHECK(same, "... read in pieces, every byte right");
    SetFilePointer(h, 123457, FILE_BEGIN);
    uint8_t few[5];
    CHECK(ReadFile(h, few, 5, &got) && got == 5 && !memcmp(few, big + 123457, 5), "... after a seek");
    CHECK(ReadFile(h, few, 5, &got) && got == 5 && !memcmp(few, big + 123462, 5), "... and on from there");
    SetFilePointer(h, 0, FILE_END);
    CHECK(ReadFile(h, few, 5, &got) && got == 0, "... nothing past the end");
    CloseHandle(h);
}

static void test_dos_results(void)
{
    /* E:\DOSTEST.BAT ran in a DOS session before the GUI started */
    int n = slurp("E:\\BIG2.BIN", buf, sizeof buf);
    CHECK(n == (int)sizeof big && !memcmp(buf, big, sizeof big), "DOS's COPY D:\\BIG.BIN E:\\BIG2.BIN: the same bytes");
    CHECK(text_is("E:\\DOSDIR\\RENAMED.TXT", "made by DOS\r\n"), "DOS made a folder, a file, and renamed it");
    CHECK(GetFileAttributes("D:\\GONE.TXT") == INVALID_FILE_ATTRIBUTES, "DOS deleted D:\\GONE.TXT");
    CHECK(text_is("D:\\Written by a DOS program with LFN.txt", "hello lfn\r\n"),
          "a DOS program made a long name (INT 21h AX=716Ch)");
    CHECK(GetFileAttributes("D:\\A DOS long dir") == INVALID_FILE_ATTRIBUTES, "... and removed its long-named folder");
    CHECK(text_is("D:\\Sub Folder\\FROMDOS.TXT", "DOS wrote this\r\n"), "DOS wrote into a long-named folder (by its alias)");
    if (!text_is("D:\\Sub Folder\\FROMDOS.TXT", "DOS wrote this\r\n")) {
        char t[64];
        int n = slurp("D:\\Sub Folder\\FROMDOS.TXT", t, sizeof t - 1);
        printf("FS: diag: read %d, attr %lx, error %lu\n", n, (unsigned long)GetFileAttributes("D:\\Sub Folder\\FROMDOS.TXT"),
               (unsigned long)GetLastError());
        WIN32_FIND_DATA fd;
        HANDLE f = FindFirstFile("D:\\Sub Folder\\*", &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do printf("FS: diag: [%s] [%s] %lx %lu\n", fd.name, fd.short_name, (unsigned long)fd.attrib, (unsigned long)fd.size);
            while (FindNextFile(f, &fd));
            FindClose(f);
        }
    }
}

static void test_writing(void)
{
    for (uint32_t i = 0; i < 100000; i++)
        buf[i] = (uint8_t)(i * 13 + 5);
    CHECK(spit("D:\\New Long Name Document.txt", buf, 100000), "write 100000 bytes to a new long-named file");
    static uint8_t back[100000];
    CHECK(slurp("D:\\New Long Name Document.txt", back, sizeof back) == 100000 && !memcmp(back, buf, 100000),
          "... and read them back");
    CHECK(listed("D:\\*", "New Long Name Document.txt", "NEWLON~1.TXT", NULL), "... alias NEWLON~1.TXT");
    CHECK(spit("D:\\New Long Name Doc 2.txt", "2", 1) && listed("D:\\*", "New Long Name Doc 2.txt", "NEWLON~2.TXT", NULL),
          "a second one gets NEWLON~2.TXT");
    CHECK(spit("D:\\lower.txt", "x", 1) && listed("D:\\*", "lower.txt", "", NULL),
          "lower.txt: no long name (8.3 with the case bits)");
    CHECK(spit("D:\\Mixed.Txt", "y", 1) && listed("D:\\*", "Mixed.Txt", "MIXED.TXT", NULL),
          "Mixed.Txt: a long name for the case, alias MIXED.TXT");
    CHECK(CreateFile("D:\\NEW LONG NAME DOCUMENT.TXT", GENERIC_WRITE, CREATE_NEW) == INVALID_HANDLE_VALUE &&
          GetLastError() == ERROR_FILE_EXISTS, "CREATE_NEW on a name that exists in another case fails");

    /* a gap reads as zeros */
    HANDLE h = CreateFile("D:\\gap.bin", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
    uint32_t put, got;
    SetFilePointer(h, 10000, FILE_BEGIN);
    CHECK(WriteFile(h, "0123456789", 10, &put) && GetFileSize(h) == 10010, "write past the end: the file grows");
    SetFilePointer(h, 0, FILE_BEGIN);
    static uint8_t z[10010];
    memset(z, 0xAA, sizeof z);
    bool zero = ReadFile(h, z, sizeof z, &got) && got == sizeof z;
    for (int i = 0; i < 10000 && zero; i++)
        zero = z[i] == 0;
    CHECK(zero && !memcmp(z + 10000, "0123456789", 10), "... the gap reads as zeros");
    SetFilePointer(h, 5000, FILE_BEGIN);
    CHECK(SetEndOfFile(h) && GetFileSize(h) == 5000, "SetEndOfFile: 5000 bytes");
    CloseHandle(h);
    CHECK(DeleteFile("D:\\gap.bin") && GetFileAttributes("D:\\gap.bin") == INVALID_FILE_ATTRIBUTES, "DeleteFile");

    /* locks between two handles */
    HANDLE l1 = CreateFile("D:\\HELLO.TXT", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
    HANDLE l2 = CreateFile("D:\\HELLO.TXT", GENERIC_READ, OPEN_EXISTING);
    char lb[8];
    CHECK(LockFile(l1, 0, 0, 5, 0), "LockFile: the first five bytes");
    CHECK(!ReadFile(l2, lb, 4, &got) && GetLastError() == ERROR_LOCK_VIOLATION, "... another handle can't read them");
    CHECK(UnlockFile(l1, 0, 0, 5, 0) && ReadFile(l2, lb, 4, &got) && got == 4, "UnlockFile: now it can");
    CloseHandle(l1);
    CloseHandle(l2);

    /* folders, moving, renaming */
    CHECK(CreateDirectory("D:\\My Stuff"), "CreateDirectory D:\\My Stuff");
    CHECK(spit("D:\\My Stuff\\note.txt", "note\r\n", 6), "a file in it");
    CHECK(MoveFile("D:\\My Stuff\\note.txt", "D:\\My Stuff\\Renamed Note.text"), "rename it to a long name");
    CHECK(MoveFile("D:\\My Stuff", "D:\\Your Stuff"), "rename the folder");
    CHECK(text_is("D:\\Your Stuff\\Renamed Note.text", "note\r\n"), "... the file is there");
    CHECK(CreateDirectory("D:\\Your Stuff\\Inner") && MoveFile("D:\\Your Stuff\\Renamed Note.text", "D:\\Your Stuff\\Inner\\moved.txt"),
          "move the file into a subfolder");
    CHECK(MoveFile("D:\\Your Stuff\\Inner", "D:\\Inner Moved") && text_is("D:\\Inner Moved\\moved.txt", "note\r\n"),
          "move the subfolder up to the root");
    CHECK(!MoveFile("D:\\Your Stuff", "D:\\Your Stuff\\Below"), "a folder can't move into itself");
    CHECK(!RemoveDirectory("D:\\Inner Moved"), "RemoveDirectory fails on a folder with a file");
    CHECK(DeleteFile("D:\\Inner Moved\\moved.txt") && RemoveDirectory("D:\\Inner Moved") &&
          RemoveDirectory("D:\\Your Stuff"), "... then works");
    CHECK(!MoveFile("D:\\HELLO.TXT", "E:\\HELLO.TXT") && GetLastError() == ERROR_NOT_SAME_DEVICE,
          "MoveFile across volumes: not the same device");
    CHECK(CopyFile("D:\\HELLO.TXT", "E:\\Hello Copy.txt", true) && text_is("E:\\Hello Copy.txt", "Hello from FAT32.\r\n"),
          "CopyFile D: to E:");

    /* read-only */
    CHECK(SetFileAttributes("D:\\lower.txt", FILE_ATTRIBUTE_READONLY), "SetFileAttributes: read-only");
    CHECK(CreateFile("D:\\lower.txt", GENERIC_WRITE, OPEN_EXISTING) == INVALID_HANDLE_VALUE &&
          GetLastError() == ERROR_ACCESS_DENIED, "... can't be opened for writing");
    CHECK(!DeleteFile("D:\\lower.txt"), "... or deleted");
    CHECK(SetFileAttributes("D:\\lower.txt", FILE_ATTRIBUTE_ARCHIVE) && DeleteFile("D:\\lower.txt"), "... until it isn't");

    /* current directory */
    CHECK(SetCurrentDirectory("D:\\Sub Folder"), "SetCurrentDirectory to a long-named folder");
    char cwd[MAX_PATH];
    GetCurrentDirectory(sizeof cwd, cwd);
    CHECK(!strcmp(cwd, "D:\\Sub Folder"), "... GetCurrentDirectory keeps its case");
    CHECK(text_is("Deeper\\Inside File.txt", "Deep inside.\r\n"), "... relative paths start there");
    SetCurrentDirectory("C:\\OS1993");

    /* FAT16: a directory that outgrows its first cluster */
    CHECK(CreateDirectory("E:\\GUIDIR"), "E:\\GUIDIR");
    bool made = true;
    for (int i = 0; i < 40 && made; i++) {
        char name[64];
        snprintf(name, sizeof name, "E:\\GUIDIR\\A rather long file name number %d.txt", i);
        made = spit(name, name, (uint32_t)strlen(name));
    }
    CHECK(made && count("E:\\GUIDIR\\*") == 40, "40 long names (160 entries: past one cluster)");
    CHECK(text_is("E:\\GUIDIR\\A rather long file name number 39.txt", "E:\\GUIDIR\\A rather long file name number 39.txt"),
          "... the last one reads back");
    for (int i = 0; i < 40; i += 2) {
        char name[64];
        snprintf(name, sizeof name, "E:\\GUIDIR\\A rather long file name number %d.txt", i);
        DeleteFile(name);
    }
    CHECK(count("E:\\GUIDIR\\*") == 20, "... delete every other one: 20 left");

    uint32_t spc, bps, freec, total;
    CHECK(GetDiskFreeSpace("D:\\", &spc, &bps, &freec, &total) && spc == 1 && bps == 512 && total == 96760 &&
          freec > 0 && freec < total, "GetDiskFreeSpace D:");
    CHECK(spit("D:\\FROMGUI.TXT", "Written by an OS1993 program.\r\n", 31), "leave D:\\FROMGUI.TXT");
}

static bool alias_form(const char *s)                   /* "XXhhhh~1.EXT" */
{
    size_t n = strlen(s);
    return n >= 8 && s[6] == '~' && s[7] == '1' && (n == 8 || s[8] == '.');
}

static void test_exfat(void)
{
    CHECK(text_is("F:\\Hello exFAT.txt", "Hello from exFAT.\r\n"), "exFAT: read a file a host tool wrote");
    CHECK(text_is("f:\\HELLO EXFAT.TXT", "Hello from exFAT.\r\n"), "exFAT: ... names compare in any case");
    CHECK(text_is("F:\\Long Folder Name\\A file in a folder with a long name.txt", "Inside exFAT.\r\n"),
          "exFAT: a long path");
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile("F:\\Hello*", &fd);
    CHECK(f != INVALID_HANDLE_VALUE && !strcmp(fd.name, "Hello exFAT.txt") && alias_form(fd.short_name),
          "exFAT: FindFirstFile, with a DOS alias");
    char alias[MAX_PATH];
    snprintf(alias, sizeof alias, "F:\\%s", f != INVALID_HANDLE_VALUE ? fd.short_name : "?");
    FindClose(f);
    CHECK(text_is(alias, "Hello from exFAT.\r\n"), "exFAT: ... the alias opens it");
    f = FindFirstFile("F:\\Long Folder Name\\*", &fd);
    CHECK(f != INVALID_HANDLE_VALUE && !strcmp(fd.name, ".") && FindNextFile(f, &fd) && !strcmp(fd.name, ".."),
          "exFAT: a folder lists . and .. first");
    FindClose(f);
    CHECK(listed("F:\\*", "README.TXT", NULL, NULL), "exFAT: an 8.3 name");
    int n = slurp("F:\\big.bin", buf, sizeof buf);
    CHECK(n == (int)sizeof big && !memcmp(buf, big, sizeof big), "exFAT: 300000 bytes, every one right");
    n = slurp("F:\\BIGDOS.BIN", buf, sizeof buf);
    CHECK(n == (int)sizeof big && !memcmp(buf, big, sizeof big), "exFAT: DOS's COPY of it");
    CHECK(text_is("F:\\DOSX\\DOS.TXT", "exfat from DOS\r\n"), "exFAT: DOS made a folder and a file");

    for (uint32_t i = 0; i < 100000; i++)
        buf[i] = (uint8_t)(i * 31 + 7);
    CHECK(spit("F:\\A new file with a long name.dat", buf, 100000), "exFAT: write 100000 bytes");
    static uint8_t back[100000];
    CHECK(slurp("F:\\A new file with a long name.dat", back, sizeof back) == 100000 && !memcmp(back, buf, 100000),
          "exFAT: ... read them back");

    /* a file that can't stay in one piece gets a FAT chain */
    CHECK(spit("F:\\frag1.bin", buf, 4096) && spit("F:\\frag2.bin", buf, 4096), "exFAT: two small files");
    HANDLE h = CreateFile("F:\\frag1.bin", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
    uint32_t put;
    SetFilePointer(h, 0, FILE_END);
    CHECK(WriteFile(h, buf + 4096, 20000, &put) && put == 20000 && GetFileSize(h) == 24096,
          "exFAT: the first grows past the second");
    CloseHandle(h);
    CHECK(slurp("F:\\frag1.bin", back, sizeof back) == 24096 && !memcmp(back, buf, 24096),
          "exFAT: ... and reads back across the pieces");
    h = CreateFile("F:\\frag1.bin", GENERIC_WRITE, OPEN_EXISTING);
    SetFilePointer(h, 6000, FILE_BEGIN);
    CHECK(SetEndOfFile(h) && GetFileSize(h) == 6000, "exFAT: truncate it to 6000");
    CloseHandle(h);
    CHECK(slurp("F:\\frag1.bin", back, sizeof back) == 6000 && !memcmp(back, buf, 6000), "exFAT: ... still right");
    CHECK(DeleteFile("F:\\frag2.bin"), "exFAT: delete the other");

    CHECK(CreateDirectory("F:\\New Folder") && spit("F:\\New Folder\\x.txt", "x", 1), "exFAT: a folder and a file");
    CHECK(MoveFile("F:\\New Folder\\x.txt", "F:\\New Folder\\Renamed to something longer.txt") &&
          MoveFile("F:\\New Folder", "F:\\Long Folder Name\\Moved In"), "exFAT: rename, and move a folder");
    CHECK(text_is("F:\\Long Folder Name\\Moved In\\Renamed to something longer.txt", "x"), "exFAT: ... the file came along");
    CHECK(!MoveFile("F:\\Long Folder Name", "F:\\Long Folder Name\\Moved In\\Deeper"), "exFAT: not into itself");
    CHECK(!RemoveDirectory("F:\\Long Folder Name\\Moved In"), "exFAT: RemoveDirectory: not empty");
    CHECK(DeleteFile("F:\\Long Folder Name\\Moved In\\Renamed to something longer.txt") &&
          RemoveDirectory("F:\\Long Folder Name\\Moved In"), "exFAT: ... emptied, it goes");

    CHECK(CreateDirectory("F:\\GUIDIR"), "exFAT: F:\\GUIDIR");
    bool made = true;
    for (int i = 0; i < 80 && made; i++) {
        char name[80];
        snprintf(name, sizeof name, "F:\\GUIDIR\\Entry number %d of a folder that grows.txt", i);
        made = spit(name, name, (uint32_t)strlen(name));
    }
    CHECK(made && count("F:\\GUIDIR\\*") == 80, "exFAT: 80 long names (400 entries: the folder grows)");
    CHECK(text_is("F:\\GUIDIR\\Entry number 79 of a folder that grows.txt", "F:\\GUIDIR\\Entry number 79 of a folder that grows.txt"),
          "exFAT: ... the last reads back");
    for (int i = 1; i < 80; i += 2) {
        char name[80];
        snprintf(name, sizeof name, "F:\\GUIDIR\\Entry number %d of a folder that grows.txt", i);
        DeleteFile(name);
    }
    CHECK(count("F:\\GUIDIR\\*") == 40, "exFAT: delete every other one: 40 left");
    CHECK(SetFileAttributes("F:\\README.TXT", FILE_ATTRIBUTE_READONLY) && !DeleteFile("F:\\README.TXT") &&
          SetFileAttributes("F:\\README.TXT", FILE_ATTRIBUTE_ARCHIVE), "exFAT: read-only protects");
    uint32_t spc, bps, freec, total;
    CHECK(GetDiskFreeSpace("F:\\", &spc, &bps, &freec, &total) && spc == 8 && bps == 512 && freec > 0 && freec < total,
          "exFAT: GetDiskFreeSpace");
    CHECK(spit("F:\\FROMGUI.TXT", "exFAT, written by an OS1993 program.\r\n", 38), "exFAT: leave F:\\FROMGUI.TXT");
}

static void test_ext2(void)
{
    CHECK(text_is("G:\\Hello ext2.txt", "Hello from ext2.\n"), "ext2: read a file debugfs wrote");
    CHECK(text_is("g:\\HELLO EXT2.TXT", "Hello from ext2.\n"), "ext2: ... in any case");
    CHECK(text_is("G:\\Long Directory Name\\A file in a directory with a long name.txt", "Inside ext2.\n"),
          "ext2: a long path");
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile("G:\\Hello*", &fd);
    CHECK(f != INVALID_HANDLE_VALUE && !strcmp(fd.name, "Hello ext2.txt") && alias_form(fd.short_name),
          "ext2: FindFirstFile, with a DOS alias");
    char alias[MAX_PATH];
    snprintf(alias, sizeof alias, "G:\\%s", f != INVALID_HANDLE_VALUE ? fd.short_name : "?");
    FindClose(f);
    CHECK(text_is(alias, "Hello from ext2.\n"), "ext2: ... the alias opens it");
    uint32_t attr = 0;
    CHECK(listed("G:\\*", "lost+found", NULL, &attr) && (attr & FILE_ATTRIBUTE_DIRECTORY), "ext2: lost+found is a folder");
    CHECK(listed("G:\\*", "link-to-hello", NULL, &attr) && (attr & FILE_ATTRIBUTE_SYSTEM) && (attr & FILE_ATTRIBUTE_READONLY),
          "ext2: a symbolic link shows as a read-only system file");
    CHECK(text_is("G:\\link-to-hello", "Hello ext2.txt"), "ext2: ... and reads as its target");
    f = FindFirstFile("G:\\Long Directory Name\\*", &fd);
    CHECK(f != INVALID_HANDLE_VALUE && !strcmp(fd.name, ".") && FindNextFile(f, &fd) && !strcmp(fd.name, ".."),
          "ext2: a folder lists . and ..");
    FindClose(f);
    int n = slurp("G:\\big.bin", buf, sizeof buf);
    CHECK(n == (int)sizeof big && !memcmp(buf, big, sizeof big), "ext2: 300000 bytes through double indirect blocks");
    n = slurp("G:\\BIGDOS.BIN", buf, sizeof buf);
    CHECK(n == (int)sizeof big && !memcmp(buf, big, sizeof big), "ext2: DOS's COPY of it");
    CHECK(text_is("G:\\DOSX\\DOS.TXT", "ext2 from DOS\r\n"), "ext2: DOS made a folder and a file");

    uint32_t spc, bps, free0, free1, total;
    GetDiskFreeSpace("G:\\", &spc, &bps, &free0, &total);
    for (uint32_t i = 0; i < sizeof big; i++)
        buf[i] = (uint8_t)(i * 17 + 3);
    CHECK(spit("G:\\A large new file.dat", buf, sizeof big), "ext2: write 300000 bytes (indirect blocks allocated)");
    CHECK(slurp("G:\\A large new file.dat", big, sizeof big) == (int)sizeof big && !memcmp(big, buf, sizeof big),
          "ext2: ... and read them back");
    HANDLE h = CreateFile("G:\\A large new file.dat", GENERIC_WRITE, OPEN_EXISTING);
    SetFilePointer(h, 5000, FILE_BEGIN);
    CHECK(SetEndOfFile(h) && GetFileSize(h) == 5000, "ext2: truncate it to 5000");
    CloseHandle(h);
    GetDiskFreeSpace("G:\\", &spc, &bps, &free1, &total);
    CHECK(free0 - free1 == 5, "ext2: ... its other blocks and indirect blocks are free again");
    CHECK(slurp("G:\\A large new file.dat", big, sizeof big) == 5000 && !memcmp(big, buf, 5000), "ext2: ... still right");
    for (uint32_t i = 0; i < sizeof big; i++)
        big[i] = pattern(i);

    CHECK(spit("G:\\CaseTest.txt", "c", 1), "ext2: CaseTest.txt");
    CHECK(CreateFile("G:\\casetest.txt", GENERIC_WRITE, CREATE_NEW) == INVALID_HANDLE_VALUE &&
          GetLastError() == ERROR_FILE_EXISTS, "ext2: casetest.txt is the same name here");
    CHECK(spit("G:\\.hidden", "h", 1) && listed("G:\\*", ".hidden", NULL, &attr) && (attr & FILE_ATTRIBUTE_HIDDEN),
          "ext2: a dot file is hidden");

    CHECK(CreateDirectory("G:\\New Folder") && spit("G:\\New Folder\\x.txt", "x", 1), "ext2: a folder and a file");
    CHECK(MoveFile("G:\\New Folder\\x.txt", "G:\\New Folder\\Renamed to something longer.txt") &&
          MoveFile("G:\\New Folder", "G:\\Long Directory Name\\Moved In"), "ext2: rename, and move a folder");
    CHECK(text_is("G:\\Long Directory Name\\Moved In\\Renamed to something longer.txt", "x"), "ext2: ... the file came along");
    CHECK(text_is("G:\\Long Directory Name\\Moved In\\..\\A file in a directory with a long name.txt", "Inside ext2.\n"),
          "ext2: ... its .. is the new parent");
    CHECK(!MoveFile("G:\\Long Directory Name", "G:\\Long Directory Name\\Moved In\\Deeper"), "ext2: not into itself");
    CHECK(!RemoveDirectory("G:\\Long Directory Name\\Moved In"), "ext2: RemoveDirectory: not empty");
    CHECK(DeleteFile("G:\\Long Directory Name\\Moved In\\Renamed to something longer.txt") &&
          RemoveDirectory("G:\\Long Directory Name\\Moved In"), "ext2: ... emptied, it goes");

    CHECK(CreateDirectory("G:\\GUIDIR"), "ext2: G:\\GUIDIR");
    bool made = true;
    for (int i = 0; i < 80 && made; i++) {
        char name[80];
        snprintf(name, sizeof name, "G:\\GUIDIR\\Entry number %d of a directory that grows.txt", i);
        made = spit(name, name, (uint32_t)strlen(name));
    }
    CHECK(made && count("G:\\GUIDIR\\*") == 80, "ext2: 80 long names (the directory takes more blocks)");
    for (int i = 1; i < 80; i += 2) {
        char name[80];
        snprintf(name, sizeof name, "G:\\GUIDIR\\Entry number %d of a directory that grows.txt", i);
        DeleteFile(name);
    }
    CHECK(count("G:\\GUIDIR\\*") == 40, "ext2: delete every other one: 40 left");
    CHECK(SetFileAttributes("G:\\README.TXT", FILE_ATTRIBUTE_READONLY) && !DeleteFile("G:\\README.TXT") &&
          SetFileAttributes("G:\\README.TXT", FILE_ATTRIBUTE_ARCHIVE), "ext2: read-only (the write bit) protects");
    CHECK(GetDiskFreeSpace("G:\\", &spc, &bps, &free1, &total) && spc == 2 && bps == 512 && free1 > 0 && free1 < total,
          "ext2: GetDiskFreeSpace");
    CHECK(spit("G:\\FROMGUI.TXT", "ext2, written by an OS1993 program.\r\n", 37), "ext2: leave G:\\FROMGUI.TXT");
}

/* Two long names whose aliases would be the same ("SA1E68~1.TXT"): the
 * second becomes ~2, and each alias opens its own file. */
static void test_alias_clash(const char *drive, const char *a, const char *b)
{
    char pa[MAX_PATH], pb[MAX_PATH], pat[MAX_PATH], what[80];
    snprintf(pa, sizeof pa, "%s\\%s", drive, a);
    snprintf(pb, sizeof pb, "%s\\%s", drive, b);
    snprintf(pat, sizeof pat, "%s\\Same start *", drive);
    spit(pa, "first", 5);
    spit(pb, "second", 6);
    char sa[14] = "", sb[14] = "";
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.name, a)) strcpy(sa, fd.short_name);
            if (!strcmp(fd.name, b)) strcpy(sb, fd.short_name);
        } while (FindNextFile(f, &fd));
        FindClose(f);
    }
    snprintf(what, sizeof what, "%s two names, one alias: ~1 and ~2 (%s, %s)", drive, sa, sb);
    size_t la = strlen(sa), lb = strlen(sb);
    CHECK(la == 12 && lb == 12 && !strncmp(sa, sb, 6) && sa[7] == '1' && sb[7] == '2' && !strcmp(sa + 8, sb + 8), what);
    snprintf(pa, sizeof pa, "%s\\%s", drive, sa);
    snprintf(pb, sizeof pb, "%s\\%s", drive, sb);
    snprintf(what, sizeof what, "%s ... each alias opens its own file", drive);
    CHECK(text_is(pa, "first") && text_is(pb, "second"), what);
}

static void test_big(void)
{
    /* I: exFAT with 4 KB sectors, a 4.5 GB file tools/exfat.py made */
    int64_t size = 0, at = 0;
    HANDLE h = CreateFile("J:\\Huge file.bin", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
    CHECK(h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && size == 4608LL * 1024 * 1024,
          "exFAT, 4 KB sectors: a 4.5 GB file (GetFileSizeEx)");
    char b[32] = "";
    uint32_t got = 0, put = 0;
    CHECK(ReadFile(h, b, 22, &got) && got == 22 && !memcmp(b, "start of the huge file", 22), "... its first bytes");
    CHECK(SetFilePointerEx(h, 4LL * 1024 * 1024 * 1024 + 16, &at, FILE_BEGIN) && at == 4LL * 1024 * 1024 * 1024 + 16 &&
          ReadFile(h, b, 11, &got) && got == 11 && !memcmp(b, "beyond 4 GB", 11), "... and bytes past 4 GB (SetFilePointerEx)");
    SetFilePointerEx(h, 4LL * 1024 * 1024 * 1024 + 1024, NULL, FILE_BEGIN);
    CHECK(WriteFile(h, "written past 4 GB", 17, &put) && put == 17, "... write past 4 GB");
    SetFilePointerEx(h, -17, NULL, FILE_CURRENT);
    CHECK(ReadFile(h, b, 17, &got) && got == 17 && !memcmp(b, "written past 4 GB", 17), "... and read it back");
    CHECK(SetFilePointer(h, 0, FILE_END) == INVALID_SET_FILE_POINTER, "SetFilePointer (32-bit) refuses a position past 2 GB");
    CloseHandle(h);
    WIN32_FIND_DATA fd;
    h = FindFirstFile("J:\\Huge*", &fd);
    CHECK(h != INVALID_HANDLE_VALUE && fd.size_high == 1 && fd.size == 512u * 1024 * 1024, "FindFirstFile: the size's high part");
    FindClose(h);
    CHECK(text_is("J:\\small.txt", "a small file on a big disk\r\n"), "... and a small file beside it");

    /* H: ext2 with 8 KB blocks */
    CHECK(text_is("H:\\hello8k.txt", "ext2 with 8 KB blocks.\n"), "ext2, 8 KB blocks: read");
    int n = slurp("H:\\big.bin", buf, sizeof buf);
    CHECK(n == (int)sizeof big && !memcmp(buf, big, sizeof big), "... 300000 bytes through an indirect block");
    for (uint32_t i = 0; i < 200000; i++)
        buf[i] = (uint8_t)(i * 3 + 1);
    static uint8_t back[200000];
    CHECK(spit("H:\\written.bin", buf, 200000) && slurp("H:\\written.bin", back, sizeof back) == 200000 &&
          !memcmp(back, buf, 200000), "... write 200000 bytes and read them back");

    /* G: ext2, a sparse file past 4 GB (triple indirect blocks) */
    h = CreateFile("G:\\sparse.bin", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
    CHECK(SetFilePointerEx(h, 5LL * 1024 * 1024 * 1024, NULL, FILE_BEGIN) && WriteFile(h, "tail", 4, &put) && put == 4 &&
          GetFileSizeEx(h, &size) && size == 5LL * 1024 * 1024 * 1024 + 4, "ext2: a sparse file of 5 GB");
    SetFilePointerEx(h, 1LL * 1024 * 1024 * 1024, NULL, FILE_BEGIN);
    memset(b, 0x55, sizeof b);
    CHECK(ReadFile(h, b, 8, &got) && got == 8 && !b[0] && !b[7], "... its hole reads as zeros");
    SetFilePointerEx(h, -4, NULL, FILE_END);
    CHECK(ReadFile(h, b, 4, &got) && got == 4 && !memcmp(b, "tail", 4), "... and its end is there");
    CloseHandle(h);

    /* the time zone: UTC = local + bias */
    TIME_ZONE_INFORMATION tz = { -60, "" };
    CHECK(SetTimeZoneInformation(&tz), "SetTimeZoneInformation (UTC+1)");
    TIME_ZONE_INFORMATION back_tz;
    GetTimeZoneInformation(&back_tz);
    CHECK(back_tz.Bias == -60, "GetTimeZoneInformation");
    SYSTEMTIME st;
    GetLocalTime(&st);
    char when[64];
    snprintf(when, sizeof when, "%04u-%02u-%02u %02u:%02u:%02u", st.year, st.month, st.day, st.hour, st.minute, st.second);
    CHECK(spit("G:\\tz.txt", when, (uint32_t)strlen(when)) && spit("F:\\tz.txt", when, (uint32_t)strlen(when)),
          "files written in UTC+1 on ext2 and exFAT");
}

static void test_links(void)
{
    CHECK(!CreateHardLink("D:\\hard.txt", "D:\\HELLO.TXT", NULL) && GetLastError() == ERROR_NOT_SUPPORTED,
          "CreateHardLink on FAT: not supported");
    CHECK(CreateHardLink("G:\\Long Directory Name\\linked.txt", "G:\\README.TXT", NULL) &&
          text_is("G:\\Long Directory Name\\linked.txt", "ext2 here.\r\n"), "ext2: CreateHardLink, another name for a file");
    CHECK(CreateHardLink("G:\\hard.txt", "G:\\README.TXT", NULL) && DeleteFile("G:\\hard.txt") &&
          text_is("G:\\README.TXT", "ext2 here.\r\n"), "ext2: ... deleting one name leaves the file");
    CHECK(!CreateHardLink("G:\\x", "G:\\Long Directory Name", NULL), "ext2: ... not for a folder");
    CHECK(CreateSymbolicLink("G:\\sym-short", "Hello ext2.txt", 0) && text_is("G:\\sym-short", "Hello ext2.txt"),
          "ext2: CreateSymbolicLink (short: kept in the inode)");
    const char *far = "Long Directory Name/A file in a directory with a long name.txt";
    CHECK(CreateSymbolicLink("G:\\sym-long", "Long Directory Name\\A file in a directory with a long name.txt", 0) &&
          text_is("G:\\sym-long", far), "ext2: ... a long one (a block of its own; \\ becomes /)");
}

/* I: ext3, left with two committed transactions in its journal: the
 * second revokes a block the first logged. */
static void test_ext3(void)
{
    CHECK(text_is("I:\\victim.txt", "REPLAYED!!!!\n"), "ext3: a block from the journal's first transaction");
    CHECK(text_is("I:\\second.txt", "NEW SECOND\n"), "ext3: ... and from its second");
    CHECK(text_is("I:\\revoked.txt", "not replayed\n"), "ext3: a revoked block isn't replayed");
    CHECK(text_is("I:\\before.txt", "written before the crash\n"), "ext3: the rest as it was");
    CHECK(!spit("I:\\new.txt", "x", 1) && GetLastError() == ERROR_WRITE_PROTECT, "ext3: read-only (no journalled writes yet)");
}

int main(void)
{
    test_reading();
    test_dos_results();
    test_writing();
    test_exfat();
    test_ext2();
    test_alias_clash("F:", "Same start 11.txt", "Same start 50.txt");
    test_alias_clash("G:", "Same start 11.txt", "Same start 30.txt");
    test_links();
    test_ext3();
    test_big();
    printf("FS: %d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
