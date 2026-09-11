import hashlib, json, sys, zipfile
from pathlib import Path
root=Path(sys.argv[1]).resolve()
archive=root/'complete-evidence.zip'
inventory=root/'evidence-inventory.json'
if archive.exists():
    raise SystemExit('Archive already exists; refusing to replace evidence')
files=[p for p in sorted(root.rglob('*')) if p.is_file() and p not in (archive,inventory)]
entries={p.relative_to(root).as_posix():{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in files}
inventory.write_text(json.dumps({'files':entries,'note':'Inventory excludes itself and the archive. Runtime libraries and model weights are identified by protocol hashes rather than bundled.'},indent=2,sort_keys=True)+'\n',encoding='utf-8')
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for p in files+[inventory]:
        z.write(p,p.relative_to(root).as_posix())
with zipfile.ZipFile(archive) as z:
    assert set(z.namelist())==set(entries)|{'evidence-inventory.json'}
    for name,metadata in entries.items():
        content=z.read(name)
        assert len(content)==metadata['bytes']
        assert hashlib.sha256(content).hexdigest()==metadata['sha256']
    assert z.read('evidence-inventory.json')==inventory.read_bytes()
print(json.dumps({'archive':str(archive),'files':len(files)+1,'bytes':archive.stat().st_size,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'verified':True}))
