"""omp_offcheck -- verify the mod's struct OFFSETS against the game's PDB.

WHY THIS EXISTS
  omp_symcheck proves every byte signature still resolves. Nothing proved the other half of the
  contact surface. A signature that breaks is LOUD: the symbol table logs it by name and the feature
  disables itself. An offset that moves is SILENT -- the mod reads the wrong field, and the first
  anyone hears about it is a crash in somebody's game with no clue attached.

  The game's shipped PDB has the exact layout of every class, so the offsets are checkable against
  ground truth in seconds instead of being re-derived by hand after an update. That is the whole
  point: after a patch this names the handful that moved, and everything it does not name is still
  correct and does not need looking at.

  It READS game_syms.h and never writes to it. The expected symbol for each offset lives in
  offsets.map beside this file, so the mod's own source is untouched by this tooling.

USAGE
  python offcheck.py                     verify offsets.map against the PDB
  python offcheck.py --discover          propose map lines for unmapped offsets (self-verifying:
                                         it only proposes a class::member that IS at that offset)
  python offcheck.py --pdb <exe>         check against a different build's exe (PDB beside it)

EXIT CODE
  0 = every mapped offset agrees with the PDB. 1 = at least one moved, vanished, or the PDB is
  unreadable. Unmapped offsets are reported but never fail the run -- coverage is a number to grow,
  not a gate to trip over.
"""
import ctypes as C
import sys, os, re, io

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
HEADER = os.path.join(REPO, 'src', 'game', 'game_syms.h')
MAPFILE = os.path.join(HERE, 'offsets.map')
DEFAULT_EXE = r"C:\Program Files\Epic Games\SessionSkateSim\SessionGame\Binaries\Win64\SessionGame-Win64-Shipping.exe"


# ---------------------------------------------------------------- the PDB, via dbghelp
class Pdb(object):
    TI_GET_SYMNAME, TI_GET_LENGTH, TI_GET_TYPE = 1, 2, 3
    TI_GET_TYPEID, TI_FINDCHILDREN, TI_GET_DATAKIND = 4, 7, 8
    TI_GET_OFFSET, TI_GET_CHILDRENCOUNT, TI_GET_SYMTAG = 10, 13, 0
    DataIsMember, SymTagBaseClass = 7, 6

    def __init__(self, exe):
        p = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll"
        self.dbg = C.WinDLL(p if os.path.exists(p) else "dbghelp.dll")
        self.dbg.SymSetOptions(0x2 | 0x4)             # UNDNAME | DEFERRED_LOADS
        self.h = C.c_void_p(0x0FF0)
        if not self.dbg.SymInitializeW(self.h, None, False):
            raise RuntimeError("SymInitialize failed")
        self.dbg.SymLoadModuleExW.restype = C.c_ulonglong
        self.dbg.SymLoadModuleExW.argtypes = [C.c_void_p, C.c_void_p, C.c_wchar_p, C.c_wchar_p,
                                              C.c_ulonglong, C.c_ulong, C.c_void_p, C.c_ulong]
        self.base = self.dbg.SymLoadModuleExW(self.h, None, exe, None, 0, 0, None, 0)
        if not self.base:
            raise RuntimeError("SymLoadModuleEx failed -- is the .pdb next to the exe?")
        self.dbg.SymGetTypeInfo.argtypes = [C.c_void_p, C.c_ulonglong, C.c_ulong, C.c_int, C.c_void_p]
        self._cache = {}

    def _u32(self, tid, what):
        v = C.c_ulong(0)
        if self.dbg.SymGetTypeInfo(self.h, self.base, tid, what, C.byref(v)):
            return v.value
        return None

    def _name(self, tid):
        p = C.c_wchar_p()
        if self.dbg.SymGetTypeInfo(self.h, self.base, tid, self.TI_GET_SYMNAME, C.byref(p)) and p.value:
            s = p.value
            C.windll.kernel32.LocalFree(p)
            return s
        return None

    def find_type(self, name):
        class SI(C.Structure):
            _fields_ = [("SizeOfStruct", C.c_ulong), ("TypeIndex", C.c_ulong),
                        ("Reserved", C.c_ulonglong * 2), ("Index", C.c_ulong), ("Size", C.c_ulong),
                        ("ModBase", C.c_ulonglong), ("Flags", C.c_ulong), ("Value", C.c_ulonglong),
                        ("Address", C.c_ulonglong), ("Register", C.c_ulong), ("Scope", C.c_ulong),
                        ("Tag", C.c_ulong), ("NameLen", C.c_ulong), ("MaxNameLen", C.c_ulong),
                        ("Name", C.c_wchar * 1024)]
        si = SI()
        si.SizeOfStruct = C.sizeof(SI) - C.sizeof(C.c_wchar) * 1024 + C.sizeof(C.c_wchar)
        si.MaxNameLen = 1024
        if self.dbg.SymGetTypeFromNameW(self.h, C.c_ulonglong(self.base), name, C.byref(si)):
            return si.TypeIndex
        return None

    def _children(self, tid):
        n = self._u32(tid, self.TI_GET_CHILDRENCOUNT) or 0
        if not n:
            return []

        class P(C.Structure):
            _fields_ = [("Count", C.c_ulong), ("Start", C.c_ulong), ("ChildId", C.c_ulong * n)]
        p = P()
        p.Count = n
        p.Start = 0
        if not self.dbg.SymGetTypeInfo(self.h, self.base, tid, self.TI_FINDCHILDREN, C.byref(p)):
            return []
        return [p.ChildId[i] for i in range(n)]

    def layout(self, cls):
        """Member name -> ABSOLUTE offset, including inherited members at their real offsets.
        The base walk is not optional: a UE hierarchy is deep, and plenty of what the mod reads off
        a skater is really AActor's or USceneComponent's. Without it those read as 'member gone'."""
        if cls in self._cache:
            return self._cache[cls]
        tid = self.find_type(cls)
        out = {} if tid is None else self._walk(tid, 0, set())
        self._cache[cls] = out
        return out

    def member_type(self, cls, member):
        """The type NAME of one member, so a path can step into a nested struct."""
        tid = self.find_type(cls)
        if tid is None:
            return None
        for cid in self._children(tid):
            if self._u32(cid, self.TI_GET_SYMTAG) == self.SymTagBaseClass:
                bt = self._u32(cid, self.TI_GET_TYPE)
                if bt is not None:
                    n = self._name(bt)
                    if n:
                        got = self.member_type(n, member)
                        if got:
                            return got
                continue
            if self._u32(cid, self.TI_GET_DATAKIND) != self.DataIsMember:
                continue
            if (self._name(cid) or '') == member:
                mt = self._u32(cid, self.TI_GET_TYPE)
                return self._name(mt) if mt is not None else None
        return None

    def resolve(self, path):
        """'Class::member' or 'Class::member.sub.sub' -> absolute offset, or None.

        The dotted form matters more than it looks. Plenty of what the mod reads is a field of a
        struct EMBEDDED in a class -- the skater's FSkateboardingAnimParams, for one -- and the mod
        holds those as one offset from the skater. Without a path they could only be recorded as
        unverifiable, which is exactly the wrong answer for a field that moves whenever either the
        outer class OR the inner struct gains a member."""
        cls, _, rest = path.partition('::')
        if not rest:
            return None
        parts = rest.split('.')
        lay = self.layout(cls)
        if parts[0] not in lay:
            return None
        total = lay[parts[0]]
        cur_cls, cur_mem = cls, parts[0]
        for nxt in parts[1:]:
            t = self.member_type(cur_cls, cur_mem)
            if not t:
                return None
            sub = self.layout(t)
            if nxt not in sub:
                return None
            total += sub[nxt]
            cur_cls, cur_mem = t, nxt
        return total

    def _walk(self, tid, delta, seen):
        if tid in seen:
            return {}
        seen.add(tid)
        out = {}
        for cid in self._children(tid):
            tag = self._u32(cid, self.TI_GET_SYMTAG)
            off = self._u32(cid, self.TI_GET_OFFSET)
            if tag == self.SymTagBaseClass:
                bt = self._u32(cid, self.TI_GET_TYPE)
                if bt is not None:
                    out.update(self._walk(bt, delta + (off or 0), seen))
                continue
            if self._u32(cid, self.TI_GET_DATAKIND) != self.DataIsMember or off is None:
                continue
            out[self._name(cid) or "?"] = delta + off
        return out


# ---------------------------------------------------------------- the mod's table
def parse_offsets():
    """(name, value, comment, section) per entry. `section` is the class the surrounding block is
    about -- the header writes those out ('// UMenuPage:', '// FMenuPageItemDefinition -- 144 B'),
    and it is what lets a bare '_member' comment be resolved without guessing."""
    s = io.open(HEADER, encoding='utf-8').read()
    body = s[s.index('namespace off'):]
    ents, pending, section = [], [], None
    for ln in body.split('\n'):
        m = re.match(r'\s*constexpr int (k\w+)\s*=\s*(0x[0-9a-fA-F]+|-?\d+)\s*;(.*)', ln)
        if m:
            v = m.group(2)
            ents.append((m.group(1), int(v, 16) if v.lower().startswith('0x') else int(v),
                         (' '.join(pending) + ' ' + m.group(3)).strip(), section))
            pending = []                      # a comment belongs to the entry it precedes
            continue
        st = ln.strip()
        if st.startswith('//'):
            h = re.match(r'//\s*-*\s*([AUFIE][A-Za-z0-9_]{3,})\s*(?::|--|\()', st)
            if h:
                section = h.group(1)
            pending.append(st)
            if len(pending) > 6:
                pending.pop(0)
        elif not st:
            pending = []
    return ents


TWEAKS_DIR = os.path.join(REPO, 'src', 'tweaks')


def parse_tweaks_offsets():
    """SessionTweaks keeps its offsets as bare enum constants inside the module that uses them --
    `SK_MOVE_COMP = 0x550,` -- rather than in one table. That is fine for reading the code and bad
    for surviving an update: the same constant is declared in several modules at once (SK_MOVE_COMP
    in three, FTH_SKATER in four), so a field that moves has to be found and fixed in every copy,
    and nothing was checking any of them.

    Returns {NAME: [(value, file, line), ...]} so a name declared twice with DIFFERENT values shows
    up as the bug it already is, not just as an update risk."""
    out = {}
    if not os.path.isdir(TWEAKS_DIR):
        return out
    pat = re.compile(r'^\s*([A-Z][A-Z0-9_]{3,})\s*=\s*(0x[0-9a-fA-F]+)\s*,')
    for fn in sorted(os.listdir(TWEAKS_DIR)):
        if not fn.endswith(('.cpp', '.h')):
            continue
        p = os.path.join(TWEAKS_DIR, fn)
        for i, ln in enumerate(io.open(p, encoding='utf-8', errors='replace'), 1):
            m = pat.match(ln)
            if m:
                out.setdefault(m.group(1), []).append((int(m.group(2), 16), fn, i))
    return out


def parse_map():
    """kName<ws>Class::member    |    kName<ws>-    (deliberately not a struct offset)"""
    out = {}
    if not os.path.exists(MAPFILE):
        return out
    for ln in io.open(MAPFILE, encoding='utf-8'):
        ln = ln.split('#')[0].strip()
        if not ln:
            continue
        parts = ln.split()
        if len(parts) >= 2:
            out[parts[0]] = parts[1]
    return out


# ---------------------------------------------------------------- modes
def discover(pdb, ents, mapped):
    """Propose map lines for unmapped offsets.

    STRICT ON PURPOSE. The obvious version -- take every class-looking word in the comment, keep
    whichever one has SOME member at that offset -- writes confident nonsense: it proposed
    UMenuPage::_pageItemWidgets for the container's _menuPage because both sit at 0x2a0, and
    FName::ComparisonIndex for anything at offset 0. A map entry that agrees with the PDB for the
    wrong reason is worse than no entry, because it will still agree after the field it was supposed
    to be watching has moved.

    So a proposal is only made when the entry's own comment NAMES the member -- 'Class::member', or
    a bare '_member' under a section header that names the class -- and the PDB agrees that member is
    at that offset. Everything else is left for a human, which is the honest answer: the class an
    offset belongs to lives in the code that dereferences it, not in a pattern match."""
    print("-- proposals: the comment names the member AND the PDB agrees --")
    n = 0
    strong = []
    for name, val, cmt, sect in ents:
        if name in mapped:
            continue
        got = None
        # (a) the comment spells out Class::member
        for cls, mem in re.findall(r'\b([AUFI][A-Za-z0-9_]{2,})::(\w+)', cmt):
            lay = pdb.layout(cls)
            if lay.get(mem) == val:
                got = "%s::%s" % (cls, mem)
                break
        # (b) a bare _member, resolved against the class this section is about
        if not got and sect:
            for mem in re.findall(r'(?<![\w:])(_\w+)', cmt):
                lay = pdb.layout(sect)
                if lay.get(mem) == val:
                    got = "%s::%s" % (sect, mem)
                    break
        if got:
            strong.append((name, got))
    for name, got in strong:
        print("%-28s %s" % (name, got))
        n += 1
    print("-- %d proposal(s). The rest need a human: name the class the CODE dereferences,\n"
          "   or '-' when the constant is a stride, an enum value or an index rather than a field."
          % n)


def verify(pdb, ents, mapped):
    bad = unmapped = skipped = ok = 0
    for name, val, _, _sect in ents:
        exp = mapped.get(name)
        if exp is None:
            unmapped += 1
            continue
        if exp == '-':
            skipped += 1
            continue
        cls, _, member = exp.partition('::')
        if not pdb.layout(cls):
            print("  %-28s CLASS GONE    %s is not in this PDB" % (name, cls))
            bad += 1
            continue
        got = pdb.resolve(exp)
        if got is None:
            print("  %-28s MEMBER GONE   %s has no '%s'" % (name, cls, member))
            bad += 1
            continue
        if got != val:
            print("  %-28s MOVED         %s::%s is 0x%x, the header says 0x%x"
                  % (name, cls, member, got, val))
            bad += 1
        else:
            ok += 1
    print("")
    print("  %d verified   %d not a struct offset   %d unmapped   %d WRONG   (of %d)"
          % (ok, skipped, unmapped, bad, len(ents)))
    if unmapped:
        print("  unmapped offsets are UNCHECKED, not proven correct -- --discover grows coverage")
    return bad


def verify_tweaks(pdb, tw, mapped):
    """The tweaks constants, plus the thing only this side can check: a name declared in more than
    one module must agree with itself. A disagreement there is a live bug, not an update risk."""
    print("\n-- src/tweaks constants --")
    bad = ok = unmapped = skipped = 0
    for name in sorted(tw):
        decls = tw[name]
        vals = set(v for v, _, _ in decls)
        if len(vals) > 1:
            where = ", ".join("%s:%d=0x%x" % (f, l, v) for v, f, l in decls)
            print("  %-28s DISAGREES     %s" % (name, where))
            bad += 1
            continue
        val = decls[0][0]
        exp = mapped.get(name)
        if exp is None:
            unmapped += 1
            continue
        if exp == '-':
            skipped += 1
            continue
        if not pdb.layout(exp.partition('::')[0]):
            print("  %-28s CLASS GONE    %s is not in this PDB" % (name, exp.partition('::')[0]))
            bad += 1
            continue
        got = pdb.resolve(exp)
        if got is None:
            print("  %-28s MEMBER GONE   %s" % (name, exp))
            bad += 1
        elif got != val:
            print("  %-28s MOVED         %s is 0x%x, %s says 0x%x"
                  % (name, exp, got, decls[0][1], val))
            bad += 1
        else:
            ok += 1
    dup = sum(1 for n in tw if len(tw[n]) > 1)
    print("  %d verified   %d not a struct offset   %d unmapped   %d WRONG   (of %d, %d declared "
          "in more than one module)" % (ok, skipped, unmapped, bad, len(tw), dup))
    return bad


def main():
    exe = DEFAULT_EXE
    if '--pdb' in sys.argv:
        exe = sys.argv[sys.argv.index('--pdb') + 1]
    print("omp_offcheck -- %s" % exe)
    try:
        pdb = Pdb(exe)
    except Exception as e:
        print("  cannot read the PDB: %s" % e)
        return 1
    ents = parse_offsets()
    mapped = parse_map()
    tw = parse_tweaks_offsets()
    print("  %d offsets in game_syms.h, %d constants in src/tweaks, %d mapped\n"
          % (len(ents), len(tw), len(mapped)))
    if '--discover' in sys.argv:
        discover(pdb, ents, mapped)
        return 0
    bad = verify(pdb, ents, mapped)
    bad += verify_tweaks(pdb, tw, mapped)
    print("\nOFFCHECK %s" % ("PASS" if bad == 0 else "FAIL"))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
