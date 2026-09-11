"""Synthetic temporary closure checks; no model or candidate closure is run."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import warnings
import zipfile

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('closure', HERE / 'close_candidate.py')
closure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(closure)


class ClosureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='forge-candidate-closure-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.contract = closure.read(HERE / 'acceptance/contract.json')
        self.rows = [row for row in self.contract['schedule'] if row['gate'] == 'G1']
        self.records = [{'run_id': row['run_id'], 'execution_id': str(i)} for i, row in enumerate(self.rows)]

    def test_complete_18_population_is_required(self):
        value = closure.gate_population(self.contract, self.records, 'G1')
        self.assertEqual(value['scheduled'], 18)
        self.assertEqual(value['recorded'], 18)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            closure.gate_population(self.contract, self.records[:-1], 'G1')

    def test_duplicate_unscheduled_and_extra_gate_outcomes_rejected(self):
        with self.assertRaisesRegex(ValueError, 'duplicate scheduled'):
            closure.gate_population(self.contract, self.records + self.records[:1], 'G1')
        bad = copy.deepcopy(self.records)
        bad[0]['run_id'] = 'invented'
        with self.assertRaisesRegex(ValueError, 'unscheduled'):
            closure.gate_population(self.contract, bad, 'G1')
        extra = next(row for row in self.contract['schedule'] if row['gate'] == 'G2')
        with self.assertRaisesRegex(ValueError, 'beyond'):
            closure.gate_population(self.contract, self.records +
                                   [{'run_id': extra['run_id'], 'execution_id': 'G2'}], 'G1')

    def test_execution_identity_cannot_be_reused(self):
        self.records[1]['execution_id'] = self.records[0]['execution_id']
        with self.assertRaisesRegex(ValueError, 'duplicate execution'):
            closure.gate_population(self.contract, self.records, 'G1')

    def test_archive_includes_runtime_hidden_files_caches_and_existing_zips(self):
        candidate = self.root / 'candidate'
        files = {'runtime/forge.exe': b'synthetic runtime bytes',
                 'g0-binaries/unit.exe': b'synthetic G0 bytes',
                 'source.zip': b'synthetic existing source archive',
                 'runs/task/session/trial-01/.forge/sessions/a/context/1.txt': b'raw prompt',
                 'runs/task/terminal-workspace/__pycache__/source.pyc': b'cache input',
                 '.git': b'regular gitfile', '.hidden': b'hidden provenance'}
        for name, data in files.items():
            path = candidate / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        output = self.root / 'closure'
        output.mkdir()
        before = closure.inventory(closure.file_map(candidate))
        archive, entries = closure.archive_paths(output, {'candidate/' + name: path
                                                for name, path in closure.file_map(candidate).items()})
        self.assertEqual(set(entries), {'candidate/' + name for name in files})
        self.assertEqual(closure.inventory(closure.file_map(candidate)), before)
        with zipfile.ZipFile(archive) as bundle:
            self.assertEqual(bundle.read('candidate/runtime/forge.exe'), files['runtime/forge.exe'])
        closure.verify_zip(archive, entries)

    def test_archive_tampering_and_duplicate_members_rejected(self):
        source = self.root / 'log.txt'
        source.write_bytes(b'original log')
        archive, entries = closure.archive_paths(self.root, {'log.txt': source})
        entries['log.txt']['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'bytes differ'):
            closure.verify_zip(archive, entries)
        replacement = self.root / 'extra.zip'
        with zipfile.ZipFile(replacement, 'w') as bundle:
            bundle.writestr('extra.txt', 'unexpected')
        with self.assertRaisesRegex(ValueError, 'membership'):
            closure.verify_zip(replacement, entries)
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(replacement, 'w') as bundle:
                bundle.writestr('log.txt', 'one')
                bundle.writestr('log.txt', 'two')
        with self.assertRaisesRegex(ValueError, 'membership'):
            closure.verify_zip(replacement, entries)

    def test_complete_closure_keeps_failed_acceptance_separate_and_does_not_copy_model(self):
        candidate_dir = self.root / 'candidate'
        runtime = candidate_dir / 'runtime/forge.exe'
        runtime.parent.mkdir(parents=True)
        runtime.write_bytes(b'synthetic frozen runtime')
        report = {'accepted': False, 'gates': {'G1': {'failed': 18}}}
        closure.campaign.write(candidate_dir / 'acceptance-report.json', report)
        model = self.root / 'model.gguf'
        model.write_bytes(b'synthetic model stays external')
        candidate = {'model_path': str(model), 'candidate_id': 'synthetic', 'candidate_sha256': 'synthetic'}
        population = {'scheduled': 18, 'recorded': 18, 'through_gate': 'G1'}
        refs = {'runtime/forge.exe': closure.campaign.reference(runtime)}
        identity = {'identity': 'synthetic verified identity'}
        output = self.root / 'closed'
        args = SimpleNamespace(candidate=candidate_dir, output=output, through_gate='G1', include=[])
        before = closure.inventory(closure.file_map(candidate_dir))
        with mock.patch.object(closure, 'check_candidate', return_value=(
                candidate, population, report, identity, refs)), \
                mock.patch.object(closure, 'check_identity', return_value=identity), mock.patch('builtins.print'):
            self.assertEqual(closure.close(args), 0)
        receipt = closure.read(output / 'candidate-evidence-inventory.json')
        self.assertTrue(receipt['closure_complete'])
        self.assertFalse(receipt['candidate_accepted'])
        self.assertTrue(receipt['runtime_bytes_included'])
        self.assertFalse(receipt['model_bytes_copied'])
        self.assertFalse(any(name.endswith('.gguf') for name in receipt['files']))
        self.assertEqual(closure.inventory(closure.file_map(candidate_dir)), before)
        closure.verify_zip(output / 'candidate-evidence.zip', receipt['files'])


if __name__ == '__main__':
    unittest.main()
