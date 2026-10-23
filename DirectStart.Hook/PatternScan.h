#ifndef DIRECTSTART_PATTERN_SCAN_H_
#define DIRECTSTART_PATTERN_SCAN_H_
//
// PatternScan.h
//
// Small header-only signature scanner written in the same spirit as the
// libvalinet headers: no dependencies beyond <Windows.h>, stable Vn* names and
// a caller-owned pattern object.
//
//   VnPattern pattern;
//   if (VnPatternParse("48 89 5C 24 ? 57", &pattern))
//   {
//       PVOID address = VnPatternFindUnique(module, "48 89 5C 24 ? 57", &count);
//   }
//
// "??" and "?" are wildcards. Parsing is intentionally strict: every token
// must be a hex byte or a wildcard, and at most VN_PATTERN_MAX_LENGTH bytes are
// accepted.
//

#include <windows.h>
#include <stdint.h>
#include <stdlib.h>

#define VN_PATTERN_MAX_LENGTH 256

typedef struct _VnPattern
{
    BYTE Values[VN_PATTERN_MAX_LENGTH];
    BYTE Masks[VN_PATTERN_MAX_LENGTH];
    SIZE_T Length;
} VnPattern;

static __inline BOOL VnPatternParse(const char* text, VnPattern* pattern)
{
    const char* cursor;
    SIZE_T length;

    if (text == NULL || pattern == NULL)
    {
        return FALSE;
    }

    pattern->Length = 0;
    length = 0;
    cursor = text;

    while (*cursor != '\0')
    {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
            *cursor == '\n')
        {
            ++cursor;
        }
        if (*cursor == '\0')
        {
            break;
        }
        if (length >= VN_PATTERN_MAX_LENGTH)
        {
            return FALSE;
        }

        if (*cursor == '?')
        {
            pattern->Values[length] = 0;
            pattern->Masks[length] = 0;
            while (*cursor == '?')
            {
                ++cursor;
            }
        }
        else
        {
            char* end = NULL;
            unsigned long value = strtoul(cursor, &end, 16);
            if (end == cursor || value > 0xFF)
            {
                return FALSE;
            }
            pattern->Values[length] = (BYTE)value;
            pattern->Masks[length] = 0xFF;
            cursor = end;
        }
        ++length;
    }

    pattern->Length = length;
    return length != 0;
}

static __inline const BYTE* VnPatternFindInRange(
    const BYTE* start,
    SIZE_T size,
    const VnPattern* pattern,
    SIZE_T* matchCount)
{
    SIZE_T count = 0;
    const BYTE* match = NULL;
    SIZE_T offset;

    if (matchCount != NULL)
    {
        *matchCount = 0;
    }
    if (start == NULL || pattern == NULL || pattern->Length == 0 ||
        size < pattern->Length)
    {
        return NULL;
    }

    for (offset = 0; offset <= size - pattern->Length; ++offset)
    {
        SIZE_T index;
        BOOL matches = TRUE;

        for (index = 0; index < pattern->Length; ++index)
        {
            if ((start[offset + index] & pattern->Masks[index]) !=
                (pattern->Values[index] & pattern->Masks[index]))
            {
                matches = FALSE;
                break;
            }
        }

        if (!matches)
        {
            continue;
        }

        ++count;
        if (count == 1)
        {
            match = start + offset;
        }
    }

    if (matchCount != NULL)
    {
        *matchCount = count;
    }
    return match;
}

static __inline const BYTE* VnPatternFindInModule(
    HMODULE module,
    const VnPattern* pattern,
    SIZE_T* matchCount)
{
    const BYTE* base;
    const IMAGE_DOS_HEADER* dos;
    const IMAGE_NT_HEADERS* nt;
    const IMAGE_SECTION_HEADER* section;
    SIZE_T total = 0;
    const BYTE* first = NULL;
    WORD index;

    if (matchCount != NULL)
    {
        *matchCount = 0;
    }
    if (module == NULL || pattern == NULL || pattern->Length == 0)
    {
        return NULL;
    }

    base = (const BYTE*)module;
    dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
    {
        return NULL;
    }

    nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return NULL;
    }

    section = IMAGE_FIRST_SECTION(nt);
    for (index = 0; index < nt->FileHeader.NumberOfSections;
        ++index, ++section)
    {
        const BYTE* sectionStart;
        SIZE_T sectionSize;
        SIZE_T count = 0;
        const BYTE* found;

        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
        {
            continue;
        }

        sectionSize = section->Misc.VirtualSize != 0
            ? section->Misc.VirtualSize
            : section->SizeOfRawData;
        if (sectionSize < pattern->Length)
        {
            continue;
        }

        sectionStart = base + section->VirtualAddress;
        found = VnPatternFindInRange(
            sectionStart, sectionSize, pattern, &count);
        if (found != NULL && first == NULL)
        {
            first = found;
        }
        total += count;
    }

    if (matchCount != NULL)
    {
        *matchCount = total;
    }
    return first;
}

// Returns the address only when the pattern occurs exactly once.
static __inline PVOID VnPatternFindUnique(
    HMODULE module,
    const char* text,
    SIZE_T* matchCount)
{
    VnPattern pattern;
    SIZE_T count = 0;
    const BYTE* found;

    if (!VnPatternParse(text, &pattern))
    {
        if (matchCount != NULL)
        {
            *matchCount = 0;
        }
        return NULL;
    }

    found = VnPatternFindInModule(module, &pattern, &count);
    if (matchCount != NULL)
    {
        *matchCount = count;
    }
    return count == 1 ? (PVOID)found : NULL;
}

#endif // DIRECTSTART_PATTERN_SCAN_H_
