"""Generate MSVC import libraries from an explicitly supplied llama.cpp build.

No download or executable launch is performed. The caller is responsible for
matching the DLL release to cmake/Dependencies.cmake's pinned llama source.
"""
import pathlib
import re
import subprocess
import sys

root, compiler, output = map(pathlib.Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
for name in ('llama', 'ggml', 'ggml-base'):
    dll = root / f'{name}.dll'
    exports = subprocess.check_output([str(compiler / 'dumpbin.exe'), '/exports', str(dll)], text=True)
    names = re.findall(r'^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)', exports, re.M)
    if not names:
        raise RuntimeError(f'No exported symbols found in {dll}')
    definition = output / f'{name}.def'
    definition.write_text(f'LIBRARY {name}.dll\nEXPORTS\n' + '\n'.join(names) + '\n')
    subprocess.run([str(compiler / 'lib.exe'), '/nologo', f'/def:{definition}', '/machine:x64', f'/out:{output / (name + ".lib")}'], check=True, stdout=subprocess.DEVNULL)
