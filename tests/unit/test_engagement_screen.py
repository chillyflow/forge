"""Exercise engagement-screen population selection and manifest assembly."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'benchmark' / 'engagement_screen.py'
SPEC = importlib.util.spec_from_file_location('engagement_screen', SOURCE)
SCREEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCREEN)


def cell(task, feedback=0, failed=0, e2e=10.0, suite='smoke'):
    return {'task': task, 'judge_feedback': feedback, 'failed_validations': failed,
            'suite': suite, 'e2e_seconds': e2e}


class EngagementScreenTests(unittest.TestCase):
    def test_engaged_tasks_are_the_population(self):
        cells = [cell('t1', feedback=1), cell('t2'), cell('t3', feedback=3)]
        selection = SCREEN.select_population(cells)
        self.assertEqual([c['task'] for c in selection['engaged']], ['t1', 't3'])
        self.assertEqual(selection['branch'], 'one_to_three_engaged')

    def test_controls_prefer_smoke_then_speed(self):
        cells = [cell('s1', e2e=30.0), cell('s2', e2e=10.0), cell('s3', e2e=20.0),
                 cell('c1', e2e=5.0, suite='campaign')]
        selection = SCREEN.select_population(cells)
        self.assertEqual([c['task'] for c in selection['controls']], ['s2', 's3'])

    def test_controls_fall_back_to_any_inert_family(self):
        cells = [cell('s1', e2e=30.0), cell('c1', e2e=5.0, suite='campaign'),
                 cell('c2', e2e=7.0, suite='campaign')]
        selection = SCREEN.select_population(cells)
        self.assertEqual([c['task'] for c in selection['controls']], ['s1', 'c1'])

    def test_failed_validation_without_feedback_is_not_a_control(self):
        cells = [cell('x', failed=2), cell('y')]
        selection = SCREEN.select_population(cells)
        self.assertEqual([c['task'] for c in selection['controls']], ['y'])
        self.assertEqual([c['task'] for c in selection['excluded']], ['x'])

    def test_branches(self):
        self.assertEqual(SCREEN.select_population([])['branch'], 'zero_engaged')
        self.assertEqual(SCREEN.select_population([cell('a', feedback=1)])['branch'],
                         'one_to_three_engaged')
        many = [cell(f't{i}', feedback=1) for i in range(4)]
        self.assertEqual(SCREEN.select_population(many)['branch'], 'four_plus_engaged')

    def test_count_events_reads_judge_and_validation_events(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'events.jsonl'
            lines = [
                {'type': 'judge_feedback', 'data': {'model': 'jev-1.13.0'}},
                {'type': 'validation_result', 'data': {'checks_passed': False, 'status': 'conflict'}},
                {'type': 'validation_result', 'data': {'checks_passed': True, 'status': 'ok'}},
                {'type': 'judge_feedback', 'data': {}},
                {'type': 'message', 'data': 'noise'},
            ]
            path.write_text('\n'.join(json.dumps(line) for line in lines) + '\n', encoding='utf-8')
            counts = SCREEN.count_events(path)
            self.assertEqual(counts, {'judge_feedback': 2, 'validations': 2,
                                      'failed_validations': 1})


if __name__ == '__main__':
    unittest.main()
