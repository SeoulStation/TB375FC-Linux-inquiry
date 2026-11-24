"""Read-only, offline bundle and selected evidence verification; Python 3.9+."""
from pathlib import Path
import hashlib, json, re

root = Path(__file__).resolve().parents[1]
checked = 0
for row in (root/'SHA256SUMS.txt').read_text('utf-8').splitlines():
    digest, name = row.split('  ',1)
    path=(root/name).resolve()
    assert root in path.parents and path.is_file(), name
    assert hashlib.sha256(path.read_bytes()).hexdigest()==digest, name
    checked+=1
stream=(root/'evidence/second/kernel-stream.raw').read_text('utf-8')
checksum=sum('checksum not match' in line for line in stream.splitlines())
assert checksum==80, checksum
records=[int(m[1]) for line in stream.splitlines() if (m:=re.match(r'^\d+,(\d+),\d+,',line))]
assert len(records)==1536 and records==list(range(records[0],records[-1]+1))
counts={}
for folder, expected in [('first',{'read_pre':0,'read_post':0,'write_pre':1,'write_post':1}),('second',{'read_pre':311,'read_post':311,'write_pre':564,'write_post':564})]:
    trace=(root/f'evidence/{folder}/trace.txt').read_text('utf-8')
    got={event:len(re.findall(r':\s+'+event+r':',trace)) for event in expected}
    assert got==expected,(folder,got)
    counts[folder]=got
viewer=(root/'evidence/second/viewer.txt').read_text('utf-8')
phases=re.findall(r'^TB_VIEWER_PHASE_V1 seq=(\d+) phase=([A-Z_]+) mono_ns=(\d+) pid=(\d+) mode=image$',viewer,re.M)
assert [x[1] for x in phases]==['APPLY_BEGIN','APPLY_OK','ACTIVE','RESTORE_BEGIN','RESTORE_COMMIT_OK','RESTORE_VERIFIED','RESTORE_END','CLEANUP_BEGIN','CLEANUP_END']
times={x[1]:int(x[2]) for x in phases}
print(json.dumps({'ManifestFilesVerified':checked,'ChecksumMismatchMessages':checksum,'ContinuousRecords':len(records),'InternalSequenceGaps':0,'TraceCounts':counts,'ViewerPhaseMarkers':len(phases),'ActiveToRestoreMS':(times['RESTORE_BEGIN']-times['ACTIVE'])/1e6,'VisualFault':'UNRESOLVED; optical failure reported on SECOND','TabletAccess':False},indent=2))
