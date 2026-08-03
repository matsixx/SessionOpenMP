# UE4SS.lib (generated, not vendored)

`UE4SS.lib` here is an **import library generated from the installed UE4SS.dll**, because the RE-UE4SS
source tree ships no prebuilt lib and building it from source needs its git submodules.

Regenerate (from an installed UE4SS.dll):

```sh
python3 - <<'PY'
import pefile
pe = pefile.PE(r"<path>\ue4ss\UE4SS.dll", fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
syms = [e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name]
open("UE4SS.def","w").write("LIBRARY UE4SS\nEXPORTS\n" + "".join(f"    {s}\n" for s in syms))
PY
lib.exe /def:UE4SS.def /out:third_party/ue4ss/UE4SS.lib /machine:x64
```

Only two symbols actually matter to us: `??0CppUserModBase@RC@@QEAA@XZ` (ctor) and
`??1CppUserModBase@RC@@UEAA@XZ` (dtor). Everything else is along for the ride.

The class declaration we link against is `third_party/ue4ss_abi/ue4ss_abi.h` — read its header comment
before touching it, and re-verify the virtual order if UE4SS is ever upgraded.
