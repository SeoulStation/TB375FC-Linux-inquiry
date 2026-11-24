"""Read-only original-module analysis; importable ELF helpers for host proofs."""
from pathlib import Path
import collections
import datetime
import hashlib
import json
import struct
ROOT=Path(r"E:\Codex로 만든 코드들\TB375FC-Linux")
ORIGINAL=ROOT/"build/vendor-module-analysis/exports/lib/modules/nt36532.ko"
PIN="e683e48175f57825911a02a4ff175a71b6e32cd8fbcc77900f284d32357ff119"

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def dump(p,v): Path(p).write_text(json.dumps(v,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")

class Elf:
 def __init__(self,path):
  self.path=Path(path); self.b=self.path.read_bytes()
  self.h=struct.unpack_from("<16sHHIQQQIHHHHHH",self.b)
  if self.h[0][:6]!=b"\x7fELF\x02\x01" or self.h[1:3]!=(1,183): raise ValueError("Expected ELF64LE ARM64 ET_REL")
  self.rawsh=[struct.unpack_from("<IIQQQQIIQQ",self.b,self.h[6]+i*self.h[11]) for i in range(self.h[12])]
  nameshdr=self.rawsh[self.h[13]]; names=self.b[nameshdr[4]:nameshdr[4]+nameshdr[5]]
  self.sections=[]
  for i,h in enumerate(self.rawsh):
   self.sections.append(dict(index=i,name=names[h[0]:].split(b"\0",1)[0].decode(),type=h[1],flags=h[2],address=h[3],offset=h[4],size=h[5],link=h[6],info=h[7],alignment=h[8],entry_size=h[9]))
  self.byname={x["name"]:x for x in self.sections}
  sym=self.byname[".symtab"]; strings=self.content(self.sections[sym["link"]]["name"])
  self.symbols=[]
  for i,o in enumerate(range(sym["offset"],sym["offset"]+sym["size"],sym["entry_size"])):
   name,info,other,section,value,size=struct.unpack_from("<IBBHQQ",self.b,o)
   self.symbols.append(dict(index=i,name=strings[name:].split(b"\0",1)[0].decode(),bind=info>>4,type=info&15,other=other,section=section,value=value,size=size))
  self.relocations=[]
  for s in self.sections:
   if s["type"]!=4: continue
   for o in range(s["offset"],s["offset"]+s["size"],24):
    offset,info,addend=struct.unpack_from("<QQq",self.b,o); target=self.symbols[info>>32]
    self.relocations.append(dict(relocation_section=s["name"],target_section=self.sections[s["info"]]["name"],offset=offset,type=info&0xffffffff,symbol_index=info>>32,symbol=target["name"],addend=addend,file_offset=o))
 def content(self,name):
  s=self.byname[name]
  return self.b[s["offset"]:s["offset"]+s["size"]] if s["type"]!=8 else b""
 def symbol(self,name):
  found=[s for s in self.symbols if s["name"]==name]
  if len(found)!=1: raise ValueError((name,len(found)))
  return found[0]
 def canonical_relocation(self,r):
  s=self.symbols[r["symbol_index"]]
  if s["section"]==0: target=("IMPORT",s["name"],r["addend"])
  elif s["section"]<len(self.sections): target=(self.sections[s["section"]]["name"],s["value"]+r["addend"])
  else: target=("SPECIAL",s["section"],s["name"],s["value"]+r["addend"])
  return (r["target_section"],r["offset"],r["type"],target)
 def versions(self):
  raw=self.content("__versions")
  return {raw[i+8:i+64].split(b"\0",1)[0].decode():struct.unpack_from("<Q",raw,i)[0] for i in range(0,len(raw),64)}
 def functions(self): return [s for s in self.symbols if s["type"]==2 and s["section"]<len(self.sections)]
 def owner(self,section,offset):
  found=[s for s in self.functions() if self.sections[s["section"]]["name"]==section and s["value"]<=offset<s["value"]+s["size"]]
  return found[0]["name"] if len(found)==1 else [s["name"] for s in found]
 def eh_records(self):
  b=self.content(".init.eh_frame"); rows=[]; p=0
  while p+4<=len(b):
   n=struct.unpack_from("<I",b,p)[0]
   if n==0:
    rows.append({"Offset":p,"Terminator":True,"RemainingNonzero":any(b[p+4:])}); break
   if n==0xffffffff or p+4+n>len(b): raise ValueError("Unsupported or malformed EH frame")
   ident=struct.unpack_from("<I",b,p+4)[0]
   rows.append({"Offset":p,"Bytes":n+4,"CIE":ident==0,"CIEPointer":ident,"Hex":b[p:p+min(n+4,48)].hex()})
   p+=n+4
  return rows

def main():
 e=Elf(ORIGINAL)
 if sha(ORIGINAL)!=PIN: raise ValueError("Original module pin differs")
 grid=e.symbol("nvt_edge_grid_zone_set"); text=e.content(".text")
 refs=[r for r in e.relocations if (e.symbols[r["symbol_index"]]["section"]==grid["section"] and e.symbols[r["symbol_index"]]["value"]+r["addend"]==grid["value"])]
 calls=[r for r in refs if r["type"]==283]
 assert sorted(r["offset"] for r in calls)==[0x428c,0x56d0,0xa4f0]
 versions=e.versions()
 config=ROOT/"device/stock-kernel.config"; lines=config.read_text().splitlines()
 terms=["CFI","SCS","SHADOW_CALL","PTR_AUTH","BTI","UNWIND","MODULE_SIG","MODVERSIONS","MODULE_PLTS","PL011","PSCI","ARM_GIC","ARM_ARCH_TIMER","INITRD","RD_GZIP","RD_LZ4"]
 flags=[x for x in lines if x.startswith(("CONFIG_","# CONFIG_")) and any(t in x for t in terms)]
 modules=json.loads((ROOT/"build/vendor-module-analysis/modules.json").read_text()); bypath={x["path"]:x for x in modules}
 order=[]; active=set(); done=set(); missing=[]; cycles=[]
 def visit(p):
  if p in done:return
  if p in active: cycles.append(p); return
  if p not in bypath: missing.append(p); return
  active.add(p)
  for d in bypath[p]["symbol_provider_dependencies"]:visit(d)
  active.remove(p); done.add(p); order.append(p)
 visit("lib/modules/nt36532.ko")
 refs_clean=[dict(r,owner=e.owner(r["target_section"],r["offset"]),offset=hex(r["offset"])) for r in refs]
 report={"HostOnly":True,"DeviceAccess":False,"ModulePatched":False,"ModuleLoaded":False,"OriginalPath":str(ORIGINAL),"OriginalSHA256":PIN,
 "Conclusion":"Conditionally feasible without reconstructing the whole C source: all needed symbols/import CRCs exist and only direct callers require redirection. This is not yet a loadable or safe tested fix.",
 "GridSymbol":grid,"GridRawSHA256":hashlib.sha256(text[grid["value"]:grid["value"]+grid["size"]]).hexdigest(),"AllRelocationReferencesToGrid":refs_clean,
 "GridPrototypeEvidence":"All3 actual callers load w0/w1 from byte fields and w2/w3 from halfwords. Function copies those four input registers, returns w0=0 at0x8e30 or w0=-1 at0x8e94, then shared epilogue. This supports four integer arguments and integer status from the exact stock assembly, independent of public C prototypes. Preserve x0-x7 and x8 conservatively before lock; preserve original x0/x1 across unlock.",
 "ExistingSymbols":{n:e.symbol(n) for n in ["ts","mutex_lock","mutex_unlock","nvt_edge_grid_zone_set","nvt_restore_cmd_func"]},
 "RequiredExistingCRCs":{n:hex(versions[n]) for n in ["mutex_lock","mutex_unlock","module_layout"]},"VersionCount":len(versions),
 "Imports":sorted(s["name"] for s in e.symbols if s["section"]==0 and s["name"]),"Modinfo":e.content(".modinfo").decode().strip("\0").split("\0"),
 "CallerLockContext":[{"Caller":"nvt_ts_work_func","GridCall":"0x428c","Evidence":"Main mutex ts+0xf8 locked at0x3fd8 and explicitly unlocked at0x425c before grid call. SPI helpers separately take ts+0x148. Wrapper main->SPI lock order matches existing IRQ path."},{"Caller":"nvt_restore_cmd_func","GridCall":"0x56d0","Evidence":"No main-mutex calls in this function. Its pointer is installed as work callback at .text0x2e80/0x2e84 and stored at work+24 at0x2e98. It is not a direct nested resume call found by named relocation. Full lifecycle/concurrency proof remains separate."},{"Caller":"nvt_edge_grid_zone_store","GridCall":"0xa4f0","Evidence":"No main-mutex calls in the sysfs handler. Argument fields are updated before wrapper acquisition, so wrapping transaction does not by itself make field updates atomic."}],
 "ReturnAndLockPlan":"Save incoming x0-x7 plus x8, preserve x19-x29/SP/LR and leave x18 untouched. Cache ts+0xf8, mutex_lock it, restore arguments, BL original grid, save x0/x1, mutex_unlock same pointer, restore original returns. No indirect calls, new exports, new imports, fake versions or lock-state guesses.",
 "SignatureAndCFI":{"AppendedSignature":e.b.endswith(b"~Module signature appended~\n"),"CFISymbols":[s for s in e.symbols if "cfi" in s["name"].lower()],"CFISectionNames":[s["name"] for s in e.sections if "cfi" in s["name"].lower()],"BTFSections":[s["name"] for s in e.sections if "BTF" in s["name"]],"NoteGNUPropertyHex":e.content(".note.gnu.property").hex(),"Scope":"Unsigned original already loads according to parent. SIG_FORCE=n does not alone guarantee acceptance: MODULE_SIG_PROTECT and strict KCFI remain. Same imported/exported symbol set is essential. Direct-only wrapper does not require an indirect-call type hash, but original callback prefixes/CFI checks stay untouched."},
 "Unwind":{"SectionBytes":len(e.content(".init.eh_frame")),"RecordCount":len(e.eh_records()),"CIEs":[r for r in e.eh_records() if r.get("CIE")],"Terminators":[r for r in e.eh_records() if r.get("Terminator")],"Requirement":"DYNAMIC_SCS and UNWIND_PATCH_PAC_INTO_SCS require valid wrapper CIE/FDE in .init.eh_frame with PACIASP/AUTIASP and DW_CFA_negate_ra_state. Preserve original FDE semantics; no embedded zero terminator before the wrapper. A new ordinary .eh_frame silently ignored by loader is insufficient. Do not manually push x18 unconditionally because dynamic-SCS mode differs on PAC-capable CPUs."},
 "ConfigPath":str(config),"ConfigSHA256":sha(config),"RelevantConfig":flags,
 "PreservationContract":["All existing data, text instruction bytes, .modinfo, __versions, .gnu.linkonce.this_module, exports/CRCs, alternatives/jump metadata and original callback/type prefixes remain byte-identical, except permissible appended code/unwind extent","Canonical relocation equivalence across every original SHT_RELA section; only3 grid CALL26 targets change, other original targets/addends unchanged after symbol/section renumbering","Exactly one wrapper->original direct call, three incoming wrapper calls, no recursion or indirect wrapper address references","GNU ld-r may merge sections or property notes; no assumed byte preservation. A stock-only roundtrip and strict semantic comparator must pass before any experimental object is considered","Retain .plt/.init.plt/ftrace placeholder sections and verify new CALL26 relocs fall under loader's executable-section PLT accounting","New unwind FDE must resolve to wrapper and valid location changes; PAC/SCS behavior needs separate dynamic mode testing"],
 "QEMUFeasibility":{"ExactSavedKernel":str(ROOT/"build/unpacked-original/boot_b/kernel-uncompressed"),"KernelSHA256":sha(ROOT/"build/unpacked-original/boot_b/kernel-uncompressed"),"LikelyBootSupport":"Built-in PL011 console, PSCI, GIC, ARM arch timer and initrd/GZIP/LZ4 are enabled. No ARCH_VIRT config line was found; generic ARM64 virt support is inferred, not a proven boot.","DependencyLoadOrder":order,"DependencyCountIncludingNT":len(order),"DependencyBytes":sum((ROOT/"build/vendor-module-analysis/exports"/p).stat().st_size for p in order),"MissingProviders":missing,"Cycles":cycles,"Limit":"Panel dependencies pull mediatek-drm and platform modules; module_init may fail without MediaTek DT/hardware. Such failure is a fixture blocker, not proof the wrapper fails. No broad fake exports or force-load bypass is proposed.","AllowedFutureIsolation":"No disks, network, USB or host passthrough; serial log only. Confirm effective PAC/SCS mode from the actual VM boot instead of assuming model names. A successful load with no matching NT device checks loader metadata/CRC/policy, but does not execute wrapper or prove touch, unload safety or silicon behavior."},
 "HostProofMethod":["Freeze original hashes and produce stock-only ld-r roundtrip; compare every original section/semantic relocation and original EH coverage","Assemble minimal direct-call-only wrapper with GNU-as PAC CFI; add same property note and route generated FDE to .init.eh_frame; no kbuild/modpost regeneration of CRCs","Link in a new NOT-FOR-DEVICE folder and retarget exactly3 original CALL26 relocations; inspect full output and reject all unreviewed changes","Run isolated user-mode AArch64 harness with fake ts/lock/original/unlock; deliberately clobber caller-saved regs in lock/unlock and verify args, status, SP alignment, callee-saved and LR/PAC return; no hardware","If exact stock GKI dependency closure is straightforward, boot diskless/networkless QEMU twice to exercise PAC and dynamic-SCS loading, recording actual mode; otherwise report precise dependency/init blocker","Keep actual hall/NVT unload, driver remove/lifetime behavior, real module load and warm display/touch tests as separate reviewed phases; no persistent image replacement implied"],
 "PrimaryReferences":{"ARM64PLT":"https://android.googlesource.com/kernel/common/+/442489c219235991de86d0277b5d859ede6d8792/arch/arm64/kernel/module-plts.c","ModuleFinalizeSCS":"https://android.googlesource.com/kernel/common/+/3bb38c52719baa7f9cdbf200016ed481b4498290/arch/arm64/kernel/module.c","SCSParser":"https://android.googlesource.com/kernel/common/+/refs/heads/android14-5.15/arch/arm64/kernel/patch-scs.c","GKIProtection":"https://android.googlesource.com/kernel/common-patches/+/e3b74f20e5bba7d5a607816b5a65d4dafbea129f/android-mainline/ANDROID-GKI-Add-module-load-time-protected-symbol-lookup.patch","Scope":"Primary ACK references explain relevant mechanisms; exact g4528d59d4335 source paths were inaccessible. They are not substituted as proof of the exact preserved binary loader behavior."}}
 assert sha(ORIGINAL)==PIN
 out=Path(__file__).parent/"grid-elf-feasibility.json"; dump(out,report)
 print(json.dumps({"HostOnly":True,"ModulePatched":False,"AuditJSON":str(out),"SHA256":sha(out),"GridDirectCalls":len(calls),"DependencyCount":len(order),"DependencyBytes":report["QEMUFeasibility"]["DependencyBytes"],"EHRecords":len(e.eh_records())},ensure_ascii=False,indent=2))
if __name__=="__main__": main()
