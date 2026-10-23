#include "StartMenuHooks.h"

typedef void (__thiscall *Element_FireEvent_t)(void* pThis, void* pEvent, bool fFull, bool fBubble);
Element_FireEvent_t pOriginalElement_FireEvent = nullptr;

void __thiscall Hook_Element_FireEvent(void* pThis, void* pEvent, bool fFull, bool fBubble)
{
    if (!pThis)
    {
        static volatile LONG s_nullFireEventLogged = 0;
        if (InterlockedIncrement(&s_nullFireEventLogged) <= 10)
        {
            DS_Log(L"Element::FireEvent: blocked call on null element (event=%p).", pEvent);
        }
        return;
    }
    pOriginalElement_FireEvent(pThis, pEvent, fFull, fBubble);
}

BOOL InitializeDUI70Hooks()
{
    DS_SYMBOL_HOOK rgDuiHooks[] = {
        {
            { L"public: long __cdecl DirectUI::DUIXmlParser::CreateElement(unsigned short const *,class DirectUI::Element *,class DirectUI::Element *,unsigned long *,class DirectUI::Element * *)" },
            &pOriginalCDUIResourceManager_CreateElement,
            Hook_CDUIResourceManager_CreateElement,
            false
        },
        {
            { L"public: void __cdecl DirectUI::Element::FireEvent(struct DirectUI::Event *,bool,bool)" },
            &pOriginalElement_FireEvent,
            Hook_Element_FireEvent,
            true
        },
        {
            { L"public: long __cdecl DirectUI::Element::SetClass(unsigned short const *)" },
            &pfnElement_SetClass,
            nullptr,
            false
        },
        {
            { L"public: class DirectUI::DynamicArray<class DirectUI::Element *,0> * __cdecl DirectUI::Element::GetChildren(class DirectUI::Value * *)" },
            &pfnElement_GetChildren,
            nullptr,
            false
        },
        {
            { L"public: void __cdecl DirectUI::Value::Release(void)" },
            &pfnValue_Release,
            nullptr,
            false
        },
        {
            { L"public: long __cdecl DirectUI::RichText::SetBaseline(int)" },
            &pfnRichText_SetBaseline,
            nullptr,
            false
        },
        {
            { L"public: long __cdecl DirectUI::RichText::SetLineSpacing(int)" },
            &pfnRichText_SetLineSpacing,
            nullptr,
            false
        }
    };

    HMODULE hDui70 = LoadLibraryW(L"dui70.dll");
    if (!hDui70)
    {
        DS_Log(L"Initialization Warning: Could not find or access dui70.dll context registry.");
        return FALSE;
    }

    // Prefer dui70's exported decorated names for the element helpers. They are
    // the exact ABI entry points (independent of PDB availability) and are what
    // the system-tile SetClass pass relies on.
    if (!pfnElement_SetClass)
    {
        pfnElement_SetClass = reinterpret_cast<Element_SetClass_t>(
            GetProcAddress(hDui70, "?SetClass@Element@DirectUI@@QEAAJPEBG@Z"));
    }
    if (!pfnElement_GetChildren)
    {
        pfnElement_GetChildren = reinterpret_cast<Element_GetChildren_t>(
            GetProcAddress(
                hDui70,
                "?GetChildren@Element@DirectUI@@QEAAPEAV?$DynamicArray@PEAVElement@DirectUI@@$0A@@2@PEAPEAVValue@2@@Z"));
    }
    if (!pfnValue_Release)
    {
        pfnValue_Release = reinterpret_cast<Value_Release_t>(
            GetProcAddress(hDui70, "?Release@Value@DirectUI@@QEAAXXZ"));
    }
    if (!pfnRichText_SetBaseline)
    {
        pfnRichText_SetBaseline = reinterpret_cast<RichText_SetBaseline_t>(
            GetProcAddress(hDui70, "?SetBaseline@RichText@DirectUI@@QEAAJH@Z"));
    }
    if (!pfnRichText_SetLineSpacing)
    {
        pfnRichText_SetLineSpacing = reinterpret_cast<RichText_SetLineSpacing_t>(
            GetProcAddress(hDui70, "?SetLineSpacing@RichText@DirectUI@@QEAAJH@Z"));
    }

    if (!DS_HookSymbols(hDui70, rgDuiHooks, _countof(rgDuiHooks)))
    {
        DS_Log(L"Initialization Warning: DirectUI symbol lookup incomplete; trying exports.");
    }
    // dui70 exports the DirectUI methods with stable C++ decorated names. Use
    // them as a fallback when the PDB lookup is unavailable or ambiguous; this
    // is what keeps the system tile styled (and its font correct) on servicing
    // builds whose public symbols differ.
    if (!pfnElement_SetClass)
    {
        pfnElement_SetClass = reinterpret_cast<Element_SetClass_t>(
            GetProcAddress(hDui70, "?SetClass@Element@DirectUI@@QEAAJPEBG@Z"));
    }
    if (!pfnElement_GetChildren)
    {
        pfnElement_GetChildren = reinterpret_cast<Element_GetChildren_t>(
            GetProcAddress(
                hDui70,
                "?GetChildren@Element@DirectUI@@QEAAPEAV?$DynamicArray@PEAVElement@DirectUI@@$0A@@2@PEAPEAVValue@2@@Z"));
    }
    if (!pfnValue_Release)
    {
        pfnValue_Release = reinterpret_cast<Value_Release_t>(
            GetProcAddress(hDui70, "?Release@Value@DirectUI@@QEAAXXZ"));
    }
    if (!pfnRichText_SetBaseline)
    {
        pfnRichText_SetBaseline = reinterpret_cast<RichText_SetBaseline_t>(
            GetProcAddress(hDui70, "?SetBaseline@RichText@DirectUI@@QEAAJH@Z"));
    }
    if (!pfnRichText_SetLineSpacing)
    {
        pfnRichText_SetLineSpacing = reinterpret_cast<RichText_SetLineSpacing_t>(
            GetProcAddress(hDui70, "?SetLineSpacing@RichText@DirectUI@@QEAAJH@Z"));
    }
    if (!pOriginalCDUIResourceManager_CreateElement)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(
            hDui70,
            "?CreateElement@DUIXmlParser@DirectUI@@QEAAJPEBGPEAVElement@2@1PEAKPEAPEAV32@@Z"));
        if (target && DS_HookFunction(
                target,
                reinterpret_cast<void*>(Hook_CDUIResourceManager_CreateElement),
                reinterpret_cast<void**>(&pOriginalCDUIResourceManager_CreateElement)))
        {
            DS_Log(L"DUI70: CreateElement hooked through export fallback.");
        }
    }
    if (!pOriginalElement_FireEvent)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(
            hDui70,
            "?FireEvent@Element@DirectUI@@QEAAXPEAUEvent@2@_N1@Z"));
        if (target && DS_HookFunction(
                target,
                reinterpret_cast<void*>(Hook_Element_FireEvent),
                reinterpret_cast<void**>(&pOriginalElement_FireEvent)))
        {
            DS_Log(L"DUI70: FireEvent hooked through export fallback.");
        }
    }

    if (!pOriginalCDUIResourceManager_CreateElement ||
        !pfnElement_SetClass ||
        !pfnElement_GetChildren ||
        !pfnValue_Release ||
        !pfnRichText_SetBaseline ||
        !pfnRichText_SetLineSpacing)
    {
        DS_Log(L"Initialization Error: essential DirectUI functions are still missing.");
        return FALSE;
    }

    DS_Log(L"Initialization Success: DirectUI hooks and element helpers are online.");
    return TRUE;
}

// tile diagnostics
