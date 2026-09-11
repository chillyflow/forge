"""Results-only candidate archive closure; never starts inference or changes evidence.

Requires the complete declared gate population (G1 by default). All candidate
files, referenced runtime bytes and explicitly supplied extra files are archived.
Only the generated ZIP and its own inventory are excluded from recursive input.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
IMPORT_CACHE = tempfile.TemporaryDirectory(prefix='forge-closure-imports-')
sys.pycache_prefix = IMPORT_CACHE.name
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'benchmark'))
import agent_loop_acceptance as acceptance
import agent_loop_campaign as campaign

require = acceptance.require
read = acceptance.read_json
digest = acceptance.file_digest
GENERATED = {'candidate-evidence.zip', 'candidate-evidence-inventory.json'}


def file_map(directory):
    """Inventory every regular file, including hidden files, caches and archives."""
    require(directory.is_dir(), 'missing evidence directory: ' + str(directory))
    paths = {}
    for path in directory.rglob('*'):
        attrs = getattr(path.lstat(), 'st_file_attributes', 0)
        require(not path.is_symlink() and not attrs & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0),
                'cannot seal symlink or reparse-point evidence: ' + str(path))
        if path.is_file():
            paths[path.relative_to(directory).as_posix()] = path
    return paths


def inventory(paths):
    return {name: {'sha256': digest(path), 'bytes': path.stat().st_size}
            for name, path in sorted(paths.items())}


def verify_zip(path, expected):
    """Verify ZIP entries by streaming; large retained runtime DLLs stay bounded."""
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        require(len(names) == len(set(names)) and set(names) == set(expected),
                'archive membership mismatch')
        for name in names:
            value, size = hashlib.sha256(), 0
            with archive.open(name) as source:
                for block in iter(lambda: source.read(1024 * 1024), b''):
                    value.update(block)
                    size += len(block)
            require(expected[name] == {'sha256': value.hexdigest(), 'bytes': size},
                    'archive bytes differ from frozen inventory: ' + name)


def archive_paths(output, paths):
    expected = inventory(paths)
    archive = output / 'candidate-evidence.zip'
    # Stored entries avoid spending minutes recompressing existing ZIPs and CUDA
    # DLLs. This includes runtime bytes; no external model is copied.
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_STORED, allowZip64=True) as bundle:
        for name, path in sorted(paths.items()):
            bundle.write(path, name)
    verify_zip(archive, expected)
    require(inventory(paths) == expected, 'evidence changed while writing its closure archive')
    return archive, expected


def gate_population(contract, outcomes, through_gate):
    gates = ['G1', 'G2', 'G3', 'G4', 'G5', 'G6']
    required_gates = set(gates[:gates.index(through_gate) + 1])
    planned = {row['run_id']: row for row in contract['schedule']}
    ids = [record['run_id'] for record in outcomes]
    executions = [record['execution_id'] for record in outcomes]
    require(len(ids) == len(set(ids)), 'duplicate scheduled outcome')
    require(len(executions) == len(set(executions)), 'duplicate execution identity')
    require(set(ids) <= set(planned), 'unscheduled outcome')
    required = {row['run_id'] for row in planned.values() if row['gate'] in required_gates}
    require(required <= set(ids), 'declared gate population is incomplete; missing ' +
            str(len(required - set(ids))) + ' scheduled outcomes')
    require(not any(row['gate'] not in required_gates for run_id, row in planned.items() if run_id in ids),
            'outcomes extend beyond the declared through-gate; declare the full measured population')
    return {'through_gate': through_gate, 'scheduled': len(required), 'recorded': len(outcomes),
            'by_gate': {gate: sum(row['gate'] == gate for row in planned.values()) for gate in gates
                        if gate in required_gates}}


def check_identity(candidate, directory):
    observed = campaign.observe(candidate)
    require(observed == candidate['identity'], 'source/runtime/model/configuration changed since freeze')
    current, diff = campaign.source_identity(Path(candidate['source_root']), {})
    original = read(acceptance.read_artifact(ROOT, candidate['source_manifest']))
    require(current['head'] == original['head'] and current['diff_sha256'] == original['diff_sha256'],
            'source revision or tracked diff differs from frozen provenance')
    require(digest(directory / 'source.diff') == original['diff_sha256'], 'retained source diff changed')
    return {'identity': observed, 'source_head': current['head'],
            'source_diff_sha256': hashlib.sha256(diff).hexdigest(), 'g0_runtime': campaign.observe_g0(candidate)}


def check_candidate(directory, through_gate):
    contract, candidate = campaign.load(directory)
    acceptance.validate_contract(contract, ROOT)
    acceptance.validate_candidate(contract, candidate, ROOT)
    outcomes, g0 = read(directory / 'outcomes.json'), read(directory / 'g0.json')
    population = gate_population(contract, outcomes, through_gate)
    planned = {row['run_id']: row for row in contract['schedule']}
    refs = {}
    for field in ('source_manifest', 'source_archive', 'source_inventory', 'runtime_manifest'):
        refs['candidate/' + field] = candidate[field]
    for record in outcomes:
        acceptance.check_binding(candidate, planned[record['run_id']], record)
        retained = acceptance.check_archive(ROOT, record['archive'], record['inventory'])
        run_root = acceptance.read_artifact(ROOT, record['archive']).parent
        for name, entry in retained.items():
            if name.startswith('raw/'):
                raw = run_root / name.removeprefix('raw/')
                require(raw.is_file() and digest(raw) == entry['sha256'],
                        'raw run evidence differs from its verified ZIP: ' + name)
        for key in ('archive', 'inventory'):
            refs[record['run_id'] + '/' + key] = record[key]
        for role, reference in record['evidence'].items():
            refs[record['run_id'] + '/evidence/' + role] = reference
    checks = {'G0-' + row['check_id']: row for row in contract['g0_checks']}
    require(len(g0) == len(checks) and {r['run_id'] for r in g0} == set(checks), 'incomplete G0 evidence')
    require(len({r['execution_id'] for r in [*g0, *outcomes]}) == len(g0) + len(outcomes),
            'execution identity reused across G0/coding outcomes')
    for record in g0:
        acceptance.validate_g0(candidate, checks[record['run_id']], record, ROOT)
        refs[record['run_id'] + '/evidence'] = record['evidence']
        artifact = read(acceptance.read_artifact(ROOT, record['evidence']))
        for role in ('log', 'junit'):
            if role in artifact:
                refs[record['run_id'] + '/' + role] = artifact[role]
    for name, entry in candidate['g0_runtime']['files'].items():
        refs['g0-runtime/' + name] = entry
    for name, entry in candidate.get('g0_binaries', {}).items():
        refs['g0-probe/' + name] = entry
    runtime = read(acceptance.read_artifact(ROOT, candidate['runtime_manifest']))
    for name, entry in runtime['files'].items():
        refs['runtime/' + name] = {'path': str(Path(candidate['runtime_directory']) / name),
                                 'sha256': entry['sha256']}
    for reference in refs.values():
        acceptance.read_artifact(ROOT, reference)
    final = acceptance.report(contract, candidate, outcomes, g0, ROOT)
    require(final == read(directory / 'acceptance-report.json'),
            'stored final report differs from recomputed report; finish reporting before closure')
    identity = check_identity(candidate, directory)
    return candidate, population, final, identity, refs


def close(args):
    directory, output = args.candidate.resolve(), args.output.resolve()
    require(not output.exists(), 'closure output already exists; do not overwrite a prior attempt')
    require(not output.is_relative_to(directory) and not directory.is_relative_to(output),
            'closure output must be outside the unchanged candidate directory')
    candidate, population, final, identity, references = check_candidate(directory, args.through_gate)
    model = Path(candidate['model_path']).resolve()
    require(not model.is_relative_to(directory), 'model is inside candidate evidence; refuse copying model bytes')
    paths = {'candidate/' + name: path for name, path in file_map(directory).items()}
    reference_index = {}
    for role, reference in references.items():
        path = acceptance.read_artifact(ROOT, reference).resolve()
        require(path != model, 'model bytes may not enter closure archive')
        if path.is_relative_to(directory):
            member = 'candidate/' + path.relative_to(directory).as_posix()
        else:
            member = 'referenced/' + reference['sha256'] + '/' + path.name
            paths[member] = path
        reference_index[role] = {**reference, 'member': member}
    for index, included in enumerate(args.include):
        included = included.resolve()
        items = file_map(included) if included.is_dir() else {included.name: included}
        for name, path in items.items():
            require(path.resolve() != model, 'explicit include contains the model; refuse copying model bytes')
            paths[f'additional/{index:03}/' + name] = path
    require(all(path.suffix.lower() != '.gguf' and not path.samefile(model) for path in paths.values()),
            'evidence includes model bytes; model files may not be copied')
    output.mkdir(parents=True)
    shutil.copyfile(__file__, output / 'close_candidate.py')
    campaign.write(output / 'final-report.json', final)
    campaign.write(output / 'closure-plan.json', {
        'schema_version': 1, 'created_utc': campaign.now(), 'candidate_id': candidate['candidate_id'],
        'candidate_sha256': candidate['candidate_sha256'], 'population': population,
        'identity_before': identity, 'reference_index': reference_index,
        'candidate_accepted': final['accepted'],
        'retained_verification_policy': candidate.get('verification_policy', 'legacy-unspecified'),
        'archive_scope': 'Every file currently in candidate evidence plus explicit referenced artifacts and supplied includes.',
        'runtime_bytes': 'included: frozen inference runtime and G0 executable/DLL copies',
        'model_bytes': 'not copied; existing external model identity is verified before and after closure',
        'legacy_limit': 'An archive preserves retained bytes; it cannot recover cache bytes omitted by a legacy runner.',
        'build_provenance_limit': 'Retained source/diff, binaries and G0 logs are evidence, not a reproducible-build attestation.',
        'self_exclusions': sorted(GENERATED)})
    paths.update({'closure/' + name: path for name, path in file_map(output).items() if name not in GENERATED})
    archive, entries = archive_paths(output, paths)
    # Recheck live source/runtime/model and exact candidate file membership only
    # after every archive byte has been reopened and verified.
    after = check_identity(candidate, directory)
    require(after == identity, 'candidate identity changed during closure')
    require({'candidate/' + name for name in file_map(directory)} ==
            {name for name in entries if name.startswith('candidate/')}, 'candidate files changed during closure')
    require(read(directory / 'acceptance-report.json') == final, 'final report changed during closure')
    require(inventory(paths) == entries, 'retained evidence changed during final identity verification')
    manifest = {'files': entries, 'archive': campaign.reference(archive), 'closed_utc': campaign.now(),
                'closure_complete': True, 'candidate_accepted': final['accepted'],
                'population': population, 'identity_after': after, 'runtime_bytes_included': True,
                'model_bytes_copied': False}
    campaign.write(output / 'candidate-evidence-inventory.json', manifest)
    print(json.dumps({key: manifest[key] for key in ('archive', 'closure_complete', 'candidate_accepted',
                      'population', 'runtime_bytes_included', 'model_bytes_copied')}, indent=2))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--through-gate', choices=['G1', 'G2', 'G3', 'G4', 'G5', 'G6'], default='G1')
    parser.add_argument('--include', action='append', default=[], type=Path,
                        help='Additional explicitly named provenance file/directory; never include the model.')
    return close(parser.parse_args(argv))


if __name__ == '__main__':
    raise SystemExit(main())
