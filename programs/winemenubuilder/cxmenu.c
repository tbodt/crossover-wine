/*
 * Invoke the CrossOver menu management scripts.
 *
 * Copyright 2012 Francois Gouget
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdio.h>

#include <windows.h>
#include <shlobj.h>
#include "wine/debug.h"
#include "cxmenu.h"


WINE_DEFAULT_DEBUG_CHANNEL(menubuilder);


int cx_mode = 1;
int cx_dump_menus = 0;


/*
 * Functions to invoke the CrossOver menu management scripts.
 */

static int cx_wineshelllink(const char* link, int is_desktop, const char* root,
                            const char* path, const char* args,
                            const char* icon_name, const char* description)
{
    const char *argv[20];
    int pos = 0;
    int retcode;

    argv[pos++] = "wineshelllink";
    argv[pos++] = "--utf8";
    argv[pos++] = "--root";
    argv[pos++] = root;
    argv[pos++] = "--link";
    argv[pos++] = link;
    argv[pos++] = "--path";
    argv[pos++] = path;
    argv[pos++] = is_desktop ? "--desktop" : "--menu";
    if (args && strlen(args))
    {
        argv[pos++] = "--args";
        argv[pos++] = args;
    }
    if (icon_name)
    {
        argv[pos++] = "--icon";
        argv[pos++] = icon_name;
    }
    if (description && strlen(description))
    {
        argv[pos++] = "--descr";
        argv[pos++] = description;
    }
    argv[pos] = NULL;

    retcode = _spawnvp(_P_WAIT, argv[0], argv);
    if (retcode!=0)
        WINE_ERR("%s returned %d\n", argv[0], retcode);
    return retcode;
}

static char* cx_escape_string(const char* src)
{
    const char* s;
    char *dst, *d;
    DWORD len;

    len=1;
    for (s=src; *s; s++)
    {
        switch (*s)
        {
        case '\"':
        case '\\':
            len+=2;
            break;
        default:
            len+=1;
        }
    }

    dst=d=HeapAlloc(GetProcessHeap(), 0, len);
    for (s=src; *s; s++)
    {
        switch (*s)
        {
        case '\"':
            *d='\\';
            d++;
            *d='\"';
            d++;
            break;
        case '\\':
            *d='\\';
            d++;
            *d='\\';
            d++;
            break;
        default:
            *d=*s;
            d++;
        }
    }
    *d='\0';

    return dst;
}

static void cx_print_value(const char* name, const char* value)
{
    if (value)
    {
        char* str = cx_escape_string(value);
        printf("\"%s\" = \"%s\"\n", name, str);
        HeapFree(GetProcessHeap(), 0, str);
    }
}

static int cx_dump_menu(const char* link, int is_desktop, const char* root,
                        const char* path, const char* args,
                        const char* icon_name, const char* description)
{
    printf("[%s]\n", link);
    cx_print_value("IsMenu", (is_desktop ? "0" : "1"));
    cx_print_value("Root", root);
    cx_print_value("Path", path);
    cx_print_value("Args", args);
    cx_print_value("Icon", icon_name);
    cx_print_value("Description", description);
    printf("\n");
    return 0;
}

int cx_process_menu(LPCWSTR linkW, BOOL is_desktop, DWORD root_csidl,
                    LPCWSTR pathW, LPCWSTR argsW,
                    LPCSTR icon_name, LPCSTR description)
{
    WCHAR rootW[MAX_PATH];
    char *link, *root, *path, *args;
    int rc;

    link = wchars_to_utf8_chars(linkW);
    SHGetSpecialFolderPathW(NULL, rootW, root_csidl, FALSE);
    root = wchars_to_utf8_chars(rootW);
    path = pathW ? wchars_to_utf8_chars(pathW) : NULL;
    args = argsW ? wchars_to_utf8_chars(argsW) : NULL;

    WINE_TRACE("link='%s' %s: '%s' path='%s' args='%s' icon='%s' desc='%s'\n",
               link, is_desktop ? "desktop" : "menu", root,
               path, args, icon_name, description);

    if (cx_dump_menus)
        rc = cx_dump_menu(link, is_desktop, root, path, args, icon_name, description);
    else
        rc = cx_wineshelllink(link, is_desktop, root, path, args, icon_name, description);

    HeapFree(GetProcessHeap(), 0, link);
    HeapFree(GetProcessHeap(), 0, root);
    HeapFree(GetProcessHeap(), 0, path);
    HeapFree(GetProcessHeap(), 0, args);
    return rc;
}

/*
 * A CrossOver winemenubuilder extension.
 */

static BOOL cx_process_dir(WCHAR* dir)
{
    static const WCHAR wWILD[]={'\\','*',0};
    static const WCHAR wDOT[]={'.',0};
    static const WCHAR wDOTDOT[]={'.','.',0};
    static const WCHAR wLNK[]={'.','l','n','k',0};
    static const WCHAR wURL[]={'.','u','r','l',0};
    HANDLE hFind;
    WIN32_FIND_DATAW item;
    int lendir, len;
    WCHAR* path;
    BOOL rc;

    WINE_TRACE("scanning directory %s\n", wine_dbgstr_w(dir));
    lendir = lstrlenW(dir);
    lstrcatW(dir, wWILD);
    hFind=FindFirstFileW(dir, &item);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        WINE_TRACE("unable to open the '%s' directory\n", wine_dbgstr_w(dir));
        return FALSE;
    }

    rc = TRUE;
    path = HeapAlloc(GetProcessHeap(), 0, (lendir+1+MAX_PATH+2+1)*sizeof(WCHAR));
    lstrcpyW(path, dir);
    path[lendir] = '\\';
    while (1)
    {
        if (lstrcmpW(item.cFileName, wDOT) && lstrcmpW(item.cFileName, wDOTDOT))
        {
            WINE_TRACE("  %s\n", wine_dbgstr_w(item.cFileName));
            len=lstrlenW(item.cFileName);
            if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (len >= 5 && lstrcmpiW(item.cFileName+len-4, wLNK) == 0) ||
                (len >= 5 && lstrcmpiW(item.cFileName+len-4, wURL) == 0))
            {
                lstrcpyW(path+lendir+1, item.cFileName);
                if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    if (!(item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))  /* skip symlinks */
                    {
                        if (!cx_process_dir(path))
                            rc = FALSE;
                    }
                }
                else if (len >= 5 && lstrcmpiW(item.cFileName+len-4, wURL) == 0)
                {
                    WINE_TRACE("  url %s\n", wine_dbgstr_w(path));

                    if (!Process_URL(path, FALSE))
                        rc = FALSE;
                }
                else
                {
                    WINE_TRACE("  link %s\n", wine_dbgstr_w(path));

                    if (!Process_Link(path, FALSE))
                        rc=FALSE;
                }
            }
        }

        if (!FindNextFileW(hFind, &item))
        {
            if (GetLastError() != ERROR_NO_MORE_FILES)
            {
                WINE_TRACE("got error %d while scanning the '%s' directory\n", GetLastError(), wine_dbgstr_w(dir));
                rc = FALSE;
            }
            FindClose(hFind);
            break;
        }
    }

    HeapFree(GetProcessHeap(), 0, path);
    return rc;
}

BOOL cx_process_all_menus(void)
{
    static const DWORD locations[] = {
        /* CSIDL_STARTUP, Not interested in this one */
        CSIDL_DESKTOPDIRECTORY, CSIDL_STARTMENU,
        /* CSIDL_COMMON_STARTUP, Not interested in this one */
        CSIDL_COMMON_DESKTOPDIRECTORY, CSIDL_COMMON_STARTMENU };
    WCHAR dir[MAX_PATH+2]; /* +2 for cx_process_dir() */
    DWORD i, len;
    BOOL rc;

    rc = TRUE;
    for (i = 0; i < sizeof(locations)/sizeof(locations[0]); i++)
    {
        if (!SHGetSpecialFolderPathW(0, dir, locations[i], FALSE))
        {
            WINE_TRACE("unable to get the path of folder %08x\n", locations[i]);
            /* Some special folders are not defined in some bottles
             * so this is not an error
             */
            continue;
        }

        len = lstrlenW(dir);
        if (len >= MAX_PATH)
        {
            /* We've just trashed memory! Hopefully we are OK */
            WINE_TRACE("Ignoring special folder %08x because its path is too long: %s\n", locations[i], wine_dbgstr_w(dir));
            rc = FALSE;
            continue;
        }

        if (!cx_process_dir(dir))
            rc = FALSE;
    }
    return rc;
}
