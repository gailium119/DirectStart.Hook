; Lazy forwarding dispatch shared by all D3D11Proxy_* stubs.
; Adapted from the ExplorerPatcher 22000 export-forwarding approach: the real
; d3d11.dll is loaded on the first exported call, not from DllMain, so a
; complex d3d11 DllMain never runs under the loader lock of our proxy.

OPTION CASEMAP:NONE

EXTERN g_d3d11_procs:QWORD
EXTERN D3D11Proxy_Resolve:PROC

.code

D3D11Proxy_Dispatch PROC
    lea r10, [g_d3d11_procs]
    mov rax, qword ptr [r10 + r11 * 8]
    test rax, rax
    jz resolve
    jmp rax

resolve:
    ; Preserve the Windows x64 argument registers (RCX/RDX/R8/R9, XMM0-3).
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 58h
    movdqu xmmword ptr [rsp + 00h], xmm0
    movdqu xmmword ptr [rsp + 10h], xmm1
    movdqu xmmword ptr [rsp + 20h], xmm2
    movdqu xmmword ptr [rsp + 30h], xmm3

    mov rcx, r11
    call D3D11Proxy_Resolve

    movdqu xmm0, xmmword ptr [rsp + 00h]
    movdqu xmm1, xmmword ptr [rsp + 10h]
    movdqu xmm2, xmmword ptr [rsp + 20h]
    movdqu xmm3, xmmword ptr [rsp + 30h]
    add rsp, 58h
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx

    lea r10, [g_d3d11_procs]
    mov rax, qword ptr [r10 + r11 * 8]
    jmp rax
D3D11Proxy_Dispatch ENDP

END
