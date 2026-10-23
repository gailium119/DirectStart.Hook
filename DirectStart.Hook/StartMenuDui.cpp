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
    else if (!DS_HookSymbols(hDui70, rgDuiHooks, _countof(rgDuiHooks)))
    {
        DS_Log(L"Initialization Error: Target hooks mapping bindings failed within dui70.dll context frame.");
        return FALSE;
    }
    else
    {
        DS_Log(L"Initialization Success: Standard DirectUI mapping layout variables hooked and online.");
    }
    return TRUE;
}

// tile diagnostics
