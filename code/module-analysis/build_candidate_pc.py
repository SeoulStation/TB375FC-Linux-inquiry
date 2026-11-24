"""정확한 C ELF의 restore-grid callsite만 생략하는 PC 전용 후보를 만든다.

기본은 PLAN이다. --build도 파일 생성/읽기 검증만 하며 로더나 기기는 실행하지 않는다.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import struct
import uuid

HERE = Path(__file__).resolve().parent
PILOT = HERE.parent.parent
SOURCE = Path(r"E:\Codex로 만든 코드들\TB375FC-Linux\development\resume-20261101-0920\combined-grid-pen-C-host\nt36532-combined-grid-pen-C.NOT-FOR-DEVICE.ko")
SOURCE_SHA = "d742bdb89ab712ce48f462e446b26ef85bbfba2606b045c3d6d4936c6fdfbfdc"
AUDIT = Path(r"E:\Codex로 만든 코드들\TB375FC-Linux\development\resume-20261031-2013\grid-elf-feasibility\elf_audit.py")
AUDIT_SHA = "d17859dce39da8502d08f0a25ba93d10810221f474638d1c3cf77a31c6d52ccd"
CALLSITE = 0x56d0
NOP = bytes.fromhex("1f2003d5")


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def pin(path):
    path = Path(path).resolve()
    raw = path.read_bytes()
    return {"Path": str(path), "Bytes": len(raw), "SHA256": sha(raw)}


def require(ok, message):
    if not ok:
        raise ValueError(message)


class ElfBytes:
    """표준 ELF64LE 구조를 직접 검사한다. 입력 실행이나 외부 프로그램 호출은 없다."""
    def __init__(self, raw):
        self.raw = bytes(raw)
        require(len(raw) >= 64, "짧은 ELF")
        self.header = struct.unpack_from("<16sHHIQQQIHHHHHH", raw)
        h = self.header
        require(h[0][:6] == b"\x7fELF\x02\x01" and h[1:3] == (1, 183), "ARM64 ET_REL 아님")
        require(h[10] == 0 and h[11] == 64 and h[12] > 0 and h[13] < h[12], "예상 밖 ELF 테이블")
        require(h[6] + h[11] * h[12] <= len(raw), "section header 범위 초과")
        self.headers = [struct.unpack_from("<IIQQQQIIQQ", raw, h[6] + 64*i) for i in range(h[12])]
        string = self.headers[h[13]]
        require(string[1] == 3 and string[4] + string[5] <= len(raw), "section string 범위")
        names = raw[string[4]:string[4] + string[5]]
        self.sections = []
        for i, s in enumerate(self.headers):
            require(s[0] < len(names), "section 이름 범위")
            end = names.find(b"\0", s[0]);require(end >= 0, "section 이름 미종결")
            name = names[s[0]:end].decode("utf-8")
            if s[1] != 8:
                require(s[4] + s[5] <= len(raw), "section 파일 범위")
            require(s[8] == 0 or s[8] & (s[8]-1) == 0, "section 정렬")
            self.sections.append({"Index":i,"Name":name,"Type":s[1],"Offset":s[4],"Size":s[5],"Link":s[6],"Info":s[7],"EntrySize":s[9]})
        self.byname = {s["Name"]:s for s in self.sections}
        require(len(self.byname) == len(self.sections), "중복 section 이름")
        sym = self.byname[".symtab"]
        require(sym["EntrySize"] == 24 and sym["Size"] % 24 == 0, "symbol 크기")
        require(sym["Link"] < len(self.sections) and self.sections[sym["Link"]]["Type"] == 3, "symbol 문자열 연결")
        strings = self.content(self.sections[sym["Link"]]["Name"])
        self.symbols = []
        for off in range(sym["Offset"], sym["Offset"] + sym["Size"], 24):
            n, info, other, section, value, size = struct.unpack_from("<IBBHQQ", raw, off)
            require(n < len(strings) and strings.find(b"\0", n) >= 0, "symbol 이름 범위")
            require(section < len(self.sections) or section >= 0xff00, "symbol section 범위")
            self.symbols.append((strings[n:].split(b"\0",1)[0].decode(), info, other, section, value, size))
        self.relocations = []
        for s in self.sections:
            if s["Type"] != 4:
                continue
            require(s["EntrySize"] == 24 and s["Size"] % 24 == 0, "RELA 크기")
            require(s["Link"] == sym["Index"] and s["Info"] < len(self.sections), "RELA 연결")
            target = self.sections[s["Info"]]
            for p in range(s["Offset"], s["Offset"] + s["Size"], 24):
                off, info, addend = struct.unpack_from("<QQq", raw, p)
                require(info >> 32 < len(self.symbols), "RELA symbol 범위")
                require(off < target["Size"], "RELA target 범위")
                symbol = self.symbols[info >> 32]
                sec = symbol[3]
                if sec == 0:
                    dest = ("IMPORT", symbol[0], addend)
                elif sec < len(self.sections):
                    dest = (self.sections[sec]["Name"], symbol[4] + addend)
                else:
                    dest = ("SPECIAL", sec, symbol[0], symbol[4] + addend)
                self.relocations.append({"Section":s["Name"],"FileOffset":p,"TargetSection":target["Name"],"Offset":off,"Type":info & 0xffffffff,"Symbol":symbol[0],"Destination":dest})

    def content(self, name):
        s = self.byname[name]
        return b"" if s["Type"] == 8 else self.raw[s["Offset"]:s["Offset"] + s["Size"]]

    def canonical(self):
        return [(r["Section"],r["TargetSection"],r["Offset"],r["Type"],r["Destination"]) for r in self.relocations]


def derive_candidate(source):
    require(sha(source) == SOURCE_SHA, "원본 C SHA 불일치")
    e = ElfBytes(source)
    text, rela = e.byname[".text"], e.byname[".rela.text"]
    site = text["Offset"] + CALLSITE
    require(source[site:site+4] == bytes.fromhex("00000094"), "56d0이 원래 unresolved BL이 아님")
    matches = [r for r in e.relocations if r["TargetSection"] == ".text" and r["Offset"] == CALLSITE]
    require(len(matches) == 1, "56d0 relocation 개수")
    remove = matches[0]
    require(remove["Section"] == ".rela.text" and remove["Type"] == 283 and remove["Destination"] == (".text.tb375fc_grid_lock",0), "56d0 grid CALL26 아님")
    out = bytearray(source)
    out[site:site+4] = NOP
    p, end = remove["FileOffset"], rela["Offset"] + rela["Size"]
    out[p:end-24] = source[p+24:end]
    out[end-24:end] = bytes(24)
    size_field = e.header[6] + rela["Index"] * 64 + 32
    struct.pack_into("<Q", out, size_field, rela["Size"]-24)
    return bytes(out), {"TextFileOffset":site,"RemovedRelocationFileOffset":p,"OldRelaEnd":end,"RelaSizeFieldOffset":size_field}


def verify_candidate(source, candidate):
    expected, offsets = derive_candidate(source)
    require(candidate == expected, "허용한 변경 이외의 바이트 차이")
    a, b = ElfBytes(source), ElfBytes(candidate)
    require(len(source) == len(candidate), "파일 길이 변경")
    require(a.header == b.header and a.symbols == b.symbols, "ELF header/symbol 변경")
    sh_changes = []
    for old, new in zip(a.sections,b.sections):
        require(old["Name"] == new["Name"], "section 순서 변경")
        name = old["Name"]
        if name == ".rela.text":
            expected_sh = list(a.headers[old["Index"]]);expected_sh[5] -= 24
            require(tuple(expected_sh) == b.headers[new["Index"]], "RELA size 외 header 변경")
            sh_changes.append(name)
        else:
            require(a.headers[old["Index"]] == b.headers[new["Index"]], "다른 section header 변경")
        if name not in (".text",".rela.text"):
            require(a.content(name) == b.content(name), "다른 section 내용 변경: " + name)
    old_relocs = [r for r in a.canonical() if not(r[1] == ".text" and r[2] == CALLSITE)]
    require(b.canonical() == old_relocs, "다른 relocation 의미/순서 변경")
    require(not any(r["TargetSection"] == ".text" and CALLSITE <= r["Offset"] < CALLSITE+4 for r in b.relocations), "생략위치 relocation 남음")
    for off in (0x428c,0xa4f0):
        r = next(r for r in b.relocations if r["TargetSection"] == ".text" and r["Offset"] == off)
        require(r["Type"] == 283 and r["Destination"] == (".text.tb375fc_grid_lock",0), "다른 grid caller 변경")
    diff = [i for i,(x,y) in enumerate(zip(a.content(".text"),b.content(".text"))) if x != y]
    require(diff == list(range(CALLSITE,CALLSITE+4)), "다른 실행 명령 변경")
    require(b.content(".text")[0x7110:0x7114] == NOP, "C pen 정책 변경")
    return {"SourceBytes":len(source),"CandidateBytes":len(candidate),"Sections":len(a.sections),"Symbols":len(a.symbols),
            "SourceRelocations":len(a.relocations),"CandidateRelocations":len(b.relocations),
            "OnlyChangedSectionHeaders":sh_changes,"OnlyChangedSectionContents":[".text",".rela.text"],
            "ExecutableTextChangedOffsets":[hex(x) for x in diff],
            "RemovedCall26TextOffset":hex(CALLSITE),"OtherGridCallerOffsetsPreserved":["0x428c","0xa4f0"],
            "UnchangedSymbols":True,"UnchangedModinfoVersionsUnwindAndOtherSections":True,
            "UnusedFormerRelaTailZeroed":True,"AllOtherRelocationsExactCanonicalOrderEqual":True,
            "ExactOffsets":{k:hex(v) for k,v in offsets.items()},
            "FileDifferingBytes":sum(x != y for x,y in zip(source,candidate))}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build",action="store_true")
    args = ap.parse_args()
    if not args.build:
        print(json.dumps({"Mode":"PLAN","Source":str(SOURCE),"SourceSHA256":SOURCE_SHA,
                          "Scope":"nvt_restore_cmd_func .text+56d0 정적callsite 모든호출생략. runtime once latch 아님.","DriverLoaded":False,"DeviceActions":0},ensure_ascii=False))
        return
    source = SOURCE.read_bytes()
    before = pin(SOURCE)
    candidate, _ = derive_candidate(source)
    proof = verify_candidate(source,candidate)
    require(pin(AUDIT)["SHA256"] == AUDIT_SHA,"기존독립 ELF parser 불일치")
    out = HERE / ("candidate-pc-"+uuid.uuid4().hex)
    out.mkdir()
    binary = out / "nt36532-C-restore-grid-callsite-omission.NOT-FOR-DEVICE.ko"
    with binary.open("xb") as f:
        f.write(candidate)
    # 다른 기존 parser로 실제 출력 파일의 sections/symbols/relocations를 다시 읽는다.
    spec = importlib.util.spec_from_file_location("independent_existing_elf_audit",AUDIT)
    mod = importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
    ae, be = mod.Elf(SOURCE), mod.Elf(binary)
    require(ae.symbols == be.symbols and ae.h == be.h,"기존parser symbol/header 검증")
    expected = [ae.canonical_relocation(r) for r in ae.relocations if not(r["target_section"]==".text" and r["offset"]==CALLSITE)]
    require([be.canonical_relocation(r) for r in be.relocations] == expected,"기존parser relocation 검증")
    require(pin(SOURCE)==before,"원본이 분석 중 변경됨")
    record={"Schema":1,"Kind":"C_RESTORE_GRID_CALLSITE_OMISSION_PC_CANDIDATE_V1","Success":True,
            "Source":before,"Candidate":pin(binary),"Builder":pin(__file__),"IndependentELFAudit":pin(AUDIT),
            "Verification":proof,"IndependentExistingParserPassed":True,"SourceUnchangedBeforeAfter":True,
            "Scope":"nvt_restore_cmd_func .text+56d0만 매 실행 정적으로 생략한다. 관측 당시 초기restore경로였다는 사실과 후보의모든호출적용성질을 구분한다.",
            "RuntimeOnceLatch":False,"AllInvocationsAtThisCallsiteOmitted":True,"NewStateOrLockAdded":False,
            "LaterIRQFWGridCall428cPreserved":True,"UserspaceGridCalla4f0Preserved":True,"CachedArgumentsElsewherePreserved":True,
            "TransactionOmissionIncludesPageWriteGridWriteAckReadAndOriginalDelay":True,
            "KnownConfounds":["기존 grid의 page/write/read 및20ms sleep과mutex점유시간도 사라진다.","필수 firmware/factory설정을 생략해touch나주변상태가악화될수있다.","성공해도gridpayload자체의원인증명이나일반warm복구성공이아니다."],
            "KernelLoaderOrLinkerExecutionTested":False,"ActualKernelAdmissionEstablished":False,
            "FirmwareSemanticsValidated":False,"ProvenFix":False,"ReadyToDeploy":False,
            "ModuleLoaded":False,"ModuleDeployed":False,"ActualDRMExecuted":False,"COMOpened":False,"DeviceActions":0,
            "DeviceExecutionAuthorizedByThisProof":False}
    p = out / "CANDIDATE-PC-PROOF.json"
    with p.open("x",encoding="utf-8",newline="\n") as f:json.dump(record,f,ensure_ascii=False,indent=2);f.write("\n")
    print(json.dumps({"Proof":pin(p),"Candidate":pin(binary)},ensure_ascii=False))


if __name__ == "__main__":
    main()
