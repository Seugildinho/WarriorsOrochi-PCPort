"""Fetch pinned official distributions, checking their published SHA-256."""
from pathlib import Path
import hashlib, json, urllib.request, zipfile

PORT=Path(__file__).resolve().parents[1]
VENDOR=PORT/'vendor'; DOWNLOADS=VENDOR/'downloads'
SPECS=[
 ('ghidra.zip','https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip','ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db'),
 ('jdk21.zip','https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip','f9d6e191ab098c0d416e7d588a24420a8621cd2f4720dab2459b8b7b2d2d8b4e'),
 ('emotionengine.zip','https://github.com/chaoticgd/ghidra-emotionengine-reloaded/releases/download/v2.1.38/ghidra_12.1.4_PUBLIC_20261004_ghidra-emotionengine-reloaded.zip','e389ff3e921d9f7d4db2ece9906f808545ce4ff519d9e25a019b72ea509f240b')
]

def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  while b:=f.read(8*1024*1024):h.update(b)
 return h.hexdigest()

def extract(archive,dest):
 dest.mkdir(exist_ok=True,parents=True)
 with zipfile.ZipFile(archive) as z:
  for n in z.namelist():
   target=(dest/n).resolve()
   if not target.is_relative_to(dest.resolve()):raise ValueError('Unsafe ZIP path')
  z.extractall(dest)

def main():
 (PORT/'local').mkdir(exist_ok=True,parents=True)
 DOWNLOADS.mkdir(exist_ok=True,parents=True)
 for name,url,digest in SPECS:
  path=DOWNLOADS/name
  if not path.exists():
   print('Downloading',name,flush=True)
   part=path.with_suffix('.part')
   req=urllib.request.Request(url,headers={'User-Agent':'WarriorsOrochi-local-prototype'})
   with urllib.request.urlopen(req,timeout=60) as src,part.open('wb') as out:
    while b:=src.read(4*1024*1024):out.write(b)
   assert sha(part)==digest,(name,'SHA-256 mismatch')
   part.rename(path)
  assert sha(path)==digest,name
  print('Verified',name,flush=True)
 ghidra=VENDOR/'ghidra_12.1.4_PUBLIC'
 if not (ghidra/'support/analyzeHeadless.bat').exists():
  print('Extracting Ghidra',flush=True);extract(DOWNLOADS/'ghidra.zip',VENDOR)
 jdk=VENDOR/'jdk-21.0.12.1+1'
 if not (jdk/'bin/java.exe').exists():
  print('Extracting Java',flush=True);extract(DOWNLOADS/'jdk21.zip',VENDOR)
 extensions=ghidra/'Ghidra/Extensions'
 extract(DOWNLOADS/'emotionengine.zip',extensions)
 assert (jdk/'bin/java.exe').exists() and (ghidra/'support/analyzeHeadless.bat').exists()
 paths=dict(ghidra=ghidra.as_posix(),java_home=jdk.as_posix(),
            downloads=[dict(file=n,url=u,sha256=h) for n,u,h in SPECS])
 (PORT/'local/analysis_tools_paths.json').write_text(json.dumps(paths,indent=2),encoding='utf8')
 print('Analysis tools ready',flush=True)

if __name__=='__main__':main()
