# Patched Raw Input

Project: Consolation includes a patched raw mouse input feature for the PC version of *007: Quantum of Solace*.

## Credits

This feature was adapted for *007: Quantum of Solace* from the raw mouse input work in IW3SP Mod by JerryALT and contributors:

- [IW3SP Mod RawMouse.cpp](https://gitea.com/JerryALT/iw3sp_mod/src/branch/main/src/Components/Modules/RawMouse.cpp)

## What It Does

- Adds a custom `m_rawInput` dvar
- Lets the client use Windows raw mouse input instead of relying entirely on the stock path
- Aims to improve mouse accuracy and polling behavior on modern systems

## Dvar

- `m_rawInput`
  - `0`: disabled
  - `1`: enabled

## Notes

- This feature is still experimental and may change as the mouse path is refined
- It is intended to preserve the stock engine flow as much as possible while improving mouse handling
