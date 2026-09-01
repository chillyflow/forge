"""Shared, dependency-free benchmark fixture and measurement utilities."""
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import shutil
import subprocess
import threading
import time


FIXTURE_PREPARATION = 'utf8-lf-language-format-v2'
TASK_ID = re.compile(r'^[A-Za-z0-9][A-Za-z0-9_.-]*$')
CREATE_NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def digest(path):
    path = Path(path)
    h = hashlib.sha256()
    with path.open('rb') as stream:
        while data := stream.read(16 * 1024 * 1024):
            h.update(data)
    return h.hexdigest()


def runtime_bundle(executable):
    """Hash an executable plus adjacent shared libraries used by local Windows builds."""
    executable = Path(executable).resolve()
    files = [executable]
    if os.name == 'nt':
        files += sorted(path for path in executable.parent.glob('*.dll') if path.is_file())
    values = {path.name: {'sha256': digest(path), 'bytes': path.stat().st_size}
              for path in files}
    encoded = json.dumps(values, sort_keys=True, separators=(',', ':')).encode('utf-8')
    return {'sha256': hashlib.sha256(encoded).hexdigest(), 'files': values}


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def _safe_path(root, relative):
    if not isinstance(relative, str) or not relative or '\x00' in relative:
        raise ValueError('Fixture paths must be non-empty strings')
    if Path(relative).is_absolute():
        raise ValueError(f'Fixture paths must be relative: {relative!r}')
    root = root.resolve()
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or path == root:
        raise ValueError(f'Unsafe fixture path: {relative!r}')
    return path


def protected_files(task):
    """Return manifest-declared immutable paths, with a legacy test-file fallback."""
    paths = task.get('protected_files')
    if paths is None:
        paths = [name for name in task['files'] if
                 Path(name).name == 'repair_test.go' or
                 Path(name).name.startswith('test_') or
                 Path(name).name.endswith(('_test.go', '_test.py'))]
    if not isinstance(paths, list) or not paths or any(not isinstance(x, str) for x in paths):
        raise ValueError(f'Task {task.get("id", "<unknown>")} requires protected_files')
    if len(paths) != len(set(paths)):
        raise ValueError(f'Task {task.get("id", "<unknown>")} repeats a protected path')
    missing = [name for name in paths if name not in task['files']]
    if missing:
        raise ValueError(f'Protected files are absent from fixture: {missing}')
    return paths


def validate_task(task, source=None):
    label = str(source) if source else '<task>'
    if not isinstance(task, dict):
        raise ValueError(f'{label}: task must be an object')
    task_id = task.get('id')
    if not isinstance(task_id, str) or not TASK_ID.fullmatch(task_id):
        raise ValueError(f'{label}: unsafe or missing task id')
    if not isinstance(task.get('prompt'), str) or not task['prompt'].strip():
        raise ValueError(f'{label}: prompt must be a non-empty string')
    verify = task.get('verify')
    if (not isinstance(verify, list) or not 1 <= len(verify) <= 64 or
            any(not isinstance(x, str) or not x or '\x00' in x for x in verify)):
        raise ValueError(f'{label}: verify must be a non-empty argv string list')
    files = task.get('files')
    if not isinstance(files, dict) or not files:
        raise ValueError(f'{label}: files must be a non-empty object')
    for relative, content in files.items():
        if not isinstance(relative, str) or not isinstance(content, str):
            raise ValueError(f'{label}: fixture paths and contents must be strings')
        _safe_path(Path.cwd(), relative)
    protected_files(task)
    if 'entry_files' in task:
        entries = task['entry_files']
        if (not isinstance(entries, list) or any(not isinstance(x, str) for x in entries) or
                any(x not in files for x in entries)):
            raise ValueError(f'{label}: entry_files must name fixture files')
    for key in ('suite', 'language', 'category'):
        if key in task and (not isinstance(task[key], str) or not task[key]):
            raise ValueError(f'{label}: {key} must be a non-empty string')
    return task


def materialize(root, task):
    """Write normalized inputs and apply deterministic, language-specific formatting."""
    root = Path(root).resolve()
    paths = []
    for relative, content in task['files'].items():
        path = _safe_path(root, relative)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8', newline='\n')
        paths.append(path)
    go_files = sorted(path.relative_to(root).as_posix() for path in paths if path.suffix == '.go')
    if go_files:
        gofmt = shutil.which('gofmt')
        if not gofmt:
            raise RuntimeError('gofmt must be on PATH for Go fixture preparation')
        for start in range(0, len(go_files), 32):
            subprocess.run([gofmt, '-w', *('./' + name for name in go_files[start:start + 32])],
                           cwd=root, check=True, capture_output=True, timeout=30)
    hashes = {path.relative_to(root).as_posix(): digest(path) for path in sorted(paths)}
    encoded = json.dumps(hashes, sort_keys=True, separators=(',', ':')).encode('utf-8')
    return {'fixture_preparation': FIXTURE_PREPARATION,
            'fixture_sha256': hashlib.sha256(encoded).hexdigest(), 'fixture_files': hashes}


def snapshot_protected(root, task):
    root = Path(root).resolve()
    return {name: digest(_safe_path(root, name)) for name in protected_files(task)}


def protected_unchanged(root, before):
    root = Path(root).resolve()
    return all(_safe_path(root, name).is_file() and digest(_safe_path(root, name)) == value
               for name, value in before.items())


def initialize_git(root):
    root = Path(root).resolve()
    subprocess.run(['git', 'init', '-q', str(root)], check=True, capture_output=True)
    subprocess.run(['git', '-C', str(root), 'config', 'core.autocrlf', 'false'], check=True)
    subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
    subprocess.run(['git', '-C', str(root), '-c', 'user.name=Forge benchmark',
                    '-c', 'user.email=benchmark@example.invalid', 'commit', '-qm',
                    'Fixture baseline'], check=True)


def load_tasks(task_dir, suite='smoke', selected=()):
    selected = set(selected)
    tasks = []
    seen = set()
    for path in sorted(Path(task_dir).glob('*.json')):
        task = validate_task(json.loads(path.read_text(encoding='utf-8')), path)
        if task['id'] in seen:
            raise ValueError(f'Duplicate task id: {task["id"]}')
        seen.add(task['id'])
        if suite != 'all' and task.get('suite', 'smoke') != suite:
            continue
        if selected and task['id'] not in selected:
            continue
        tasks.append((path.resolve(), task))
    missing = selected - {task['id'] for _, task in tasks}
    if missing:
        raise ValueError(f'Unknown or suite-mismatched tasks: {sorted(missing)}')
    if not tasks:
        raise ValueError('No matching tasks')
    return tasks


def schedule(cases, repetitions=1, seed=42, randomize=True):
    if repetitions < 1:
        raise ValueError('repetitions must be positive')
    order = [(case, repetition) for repetition in range(1, repetitions + 1) for case in cases]
    if randomize:
        random.Random(seed).shuffle(order)
    return [(case, repetition, index) for index, (case, repetition) in enumerate(order, 1)]


def required_tools(tasks):
    tools = {task['verify'][0] for _, task in tasks}
    if any(any(Path(name).suffix == '.go' for name in task['files']) for _, task in tasks):
        tools.add('gofmt')
    return sorted(tools)


def check_tools(tasks):
    missing = [name for name in required_tools(tasks) if not shutil.which(name)]
    if missing:
        raise RuntimeError(f'Required fixture tools are missing from PATH: {missing}')


def platform_metadata():
    values = {'platform': platform.platform(), 'python_version': platform.python_version()}
    for name in ('go', 'python', 'node'):
        executable = shutil.which(name)
        if not executable:
            continue
        command = [executable, 'version'] if name == 'go' else [executable, '--version']
        try:
            output = subprocess.check_output(command, text=True, stderr=subprocess.STDOUT,
                                             timeout=10).strip()
            values[f'{name}_version'] = output
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
            pass
    try:
        values['gpu'] = subprocess.check_output(
            ['nvidia-smi', '--query-gpu=name,driver_version,memory.total',
             '--format=csv,noheader'], text=True, stderr=subprocess.DEVNULL,
            timeout=10, creationflags=CREATE_NO_WINDOW).strip()
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        values['gpu'] = None
    return values


def _windows_process_table():
    class ProcessEntry(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD),
                    ('th32ProcessID', wintypes.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
                    ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
                    ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', wintypes.LONG),
                    ('dwFlags', wintypes.DWORD), ('szExeFile', wintypes.WCHAR * 260)]
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
    invalid = ctypes.c_void_p(-1).value
    if snapshot == invalid:
        return {}
    table = {}
    entry = ProcessEntry()
    entry.dwSize = ctypes.sizeof(entry)
    try:
        ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            table[int(entry.th32ProcessID)] = int(entry.th32ParentProcessID)
            ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return table


def _windows_rss(pid):
    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD),
                    ('PeakWorkingSetSize', ctypes.c_size_t), ('WorkingSetSize', ctypes.c_size_t),
                    ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
                    ('QuotaPagedPoolUsage', ctypes.c_size_t),
                    ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
                    ('QuotaNonPagedPoolUsage', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                    ('PeakPagefileUsage', ctypes.c_size_t), ('PrivateUsage', ctypes.c_size_t)]
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE,
                                           ctypes.POINTER(ProcessMemoryCounters),
                                           wintypes.DWORD]
    handle = kernel32.OpenProcess(0x0410, False, pid)
    if not handle:
        return 0
    counters = ProcessMemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    try:
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return 0
        return int(counters.WorkingSetSize)
    finally:
        kernel32.CloseHandle(handle)


def _posix_process_table():
    table = {}
    proc = Path('/proc')
    if proc.is_dir():
        for path in proc.iterdir():
            if not path.name.isdigit():
                continue
            try:
                fields = (path / 'stat').read_text().split()
                table[int(path.name)] = int(fields[3])
            except (OSError, ValueError, IndexError):
                pass
        return table
    try:
        output = subprocess.check_output(['ps', '-axo', 'pid=,ppid='], text=True, timeout=2)
        for line in output.splitlines():
            pid, parent = line.split()
            table[int(pid)] = int(parent)
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError):
        pass
    return table


def _posix_rss(pid):
    status = Path('/proc') / str(pid) / 'status'
    if status.is_file():
        try:
            for line in status.read_text().splitlines():
                if line.startswith('VmRSS:'):
                    return int(line.split()[1]) * 1024
        except (OSError, ValueError, IndexError):
            return 0
    try:
        return int(subprocess.check_output(['ps', '-o', 'rss=', '-p', str(pid)],
                                           text=True, timeout=2).strip()) * 1024
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError):
        return 0


def process_tree_rss(roots):
    table = _windows_process_table() if os.name == 'nt' else _posix_process_table()
    descendants = set(int(pid) for pid in roots if pid)
    changed = True
    while changed:
        changed = False
        for pid, parent in table.items():
            if parent in descendants and pid not in descendants:
                descendants.add(pid)
                changed = True
    reader = _windows_rss if os.name == 'nt' else _posix_rss
    return sum(reader(pid) for pid in descendants), len(descendants)


def gpu_used_bytes(index=0):
    try:
        output = subprocess.check_output(
            ['nvidia-smi', f'--id={index}', '--query-gpu=memory.used',
             '--format=csv,noheader,nounits'], text=True, stderr=subprocess.DEVNULL,
            timeout=3, creationflags=CREATE_NO_WINDOW).strip().splitlines()
        return int(float(output[0])) * 1024 * 1024 if output else None
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError):
        return None


class ResourceMonitor:
    """Sample aggregate process-tree RSS and device VRAM without third-party modules."""

    def __init__(self, roots=(), interval=0.25, gpu_index=0):
        self.roots = list(roots)
        self.interval = interval
        self.gpu_index = gpu_index
        self.baseline_gpu = gpu_used_bytes(gpu_index)
        self.peak_gpu = self.baseline_gpu
        self.peak_rss = 0
        self.peak_processes = 0
        self.samples = 0
        self._stop = threading.Event()
        self._thread = None

    def add_root(self, pid):
        if pid:
            self.roots.append(int(pid))

    def _sample(self):
        rss, count = process_tree_rss(self.roots)
        gpu = gpu_used_bytes(self.gpu_index)
        self.peak_rss = max(self.peak_rss, rss)
        self.peak_processes = max(self.peak_processes, count)
        if gpu is not None:
            self.peak_gpu = gpu if self.peak_gpu is None else max(self.peak_gpu, gpu)
        self.samples += 1

    def _run(self):
        while not self._stop.is_set():
            self._sample()
            self._stop.wait(self.interval)

    def start(self):
        self._thread = threading.Thread(target=self._run, name='benchmark-resource-monitor',
                                        daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=max(2, self.interval * 4))
        self._sample()
        delta = None
        if self.baseline_gpu is not None and self.peak_gpu is not None:
            delta = max(0, self.peak_gpu - self.baseline_gpu)
        return {'peak_process_tree_rss_bytes': self.peak_rss,
                'peak_process_count': self.peak_processes,
                'baseline_gpu_used_bytes': self.baseline_gpu,
                'peak_gpu_used_bytes': self.peak_gpu,
                'peak_gpu_delta_bytes': delta,
                'rss_scope': 'root_processes_and_descendants',
                'gpu_scope': 'whole_device',
                'sample_interval_seconds': self.interval,
                'sample_count': self.samples}


def run_monitored(command, *, cwd=None, env=None, stdout=None, stderr=None, timeout=None,
                  extra_pids=(), gpu_index=0):
    """Run one command and return lifecycle timing plus sampled resource peaks."""
    monitor = ResourceMonitor(interval=0.25, gpu_index=gpu_index)
    flags = CREATE_NO_WINDOW if os.name == 'nt' else 0
    start = time.monotonic()
    process = subprocess.Popen(command, cwd=cwd, env=env, stdout=stdout, stderr=stderr,
                               creationflags=flags)
    monitor.add_root(process.pid)
    for pid in extra_pids:
        monitor.add_root(pid)
    monitor.start()
    timed_out = False
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    elapsed = time.monotonic() - start
    usage = monitor.stop()
    return {'returncode': 124 if timed_out else process.returncode,
            'timed_out': timed_out, 'wall_seconds': elapsed, 'resource_usage': usage}


def verify_task(root, task, output, env=None, timeout=120, gpu_index=0, extra_pids=()):
    output = Path(output)
    with (output / 'verification.stdout').open('w', encoding='utf-8') as stdout, \
            (output / 'verification.stderr').open('w', encoding='utf-8') as stderr:
        result = run_monitored(task['verify'], cwd=root, env=env, stdout=stdout, stderr=stderr,
                               timeout=timeout, gpu_index=gpu_index, extra_pids=extra_pids)
    result['passed'] = result['returncode'] == 0
    return result
