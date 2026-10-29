"""Compatibility edit values and admitted/applied state without native hardware."""
from copy import deepcopy
import importlib.util
import math
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'sdk/python'))
sys.path.insert(0, str(ROOT / 'mcp-server'))
from simxr_control import ControlError, ControlTimeout, Session
from simxr_control.compatibility import parse_edit
from control_fixture import configuration, Endpoint

spec = importlib.util.spec_from_file_location('compatibility_control', ROOT / 'mcp-server/legacy_control.py')
compatibility = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compatibility)


class EditTests(unittest.TestCase):
    def test_controller_head_relative_pose_and_held_controls_are_one_edit(self):
        state = configuration()
        original = deepcopy(state)
        edit = parse_edit('set_controller_pose', {'hand': 0, 'posX': 0, 'posY': 0,
                                                'posZ': -1, 'trigger': .75})
        commands = edit.commands(state)
        self.assertEqual([item['kind'] for item in commands],
                         ['take_control', 'controller_pose', 'take_control', 'controller_controls'])
        self.assertAlmostEqual(commands[1]['position'][0], 1)
        self.assertEqual(commands[1]['position'][1], 1.7)
        self.assertAlmostEqual(commands[1]['position'][2], 0)
        self.assertEqual(commands[3]['trigger'], .75)
        self.assertEqual(commands[3]['grip'], .25)
        self.assertEqual(commands[3]['stick_y'], -.3)
        self.assertEqual(state, original)

    def test_parsed_edit_does_not_retain_mutable_arguments(self):
        arguments = {'left_eye': {'left_deg': -30}, 'right_eye': {'right_deg': 35}}
        edit = parse_edit('set_fov', arguments)
        arguments['left_eye']['left_deg'] = -70
        views = edit.commands(configuration())[1]
        self.assertAlmostEqual(views['outer_fov'][0], math.radians(-30))
        self.assertEqual(views['ipd'], .067)
        self.assertEqual(views['inset_fov'], configuration()['views']['inset_fov'])
        with self.assertRaises(AttributeError):
            edit.outer_fov = ()

    def test_clear_both_controls_and_optics_preserve_unrelated_state(self):
        commands = parse_edit('hold_controller_inputs', {'clear': True, 'primary': 1}).commands(configuration())
        self.assertEqual(commands[1], {'kind': 'controller_controls', 'hand': 0, 'primary': True})
        self.assertEqual(commands[3], {'kind': 'controller_controls', 'hand': 1, 'primary': True})
        views = parse_edit('set_ipd', {'clear': True}).commands(configuration())[1]
        self.assertEqual(views['ipd'], .064)
        self.assertEqual(views['outer_fov'], configuration()['views']['outer_fov'])

    def test_invalid_values_never_produce_partial_commands(self):
        for name, arguments in (
                ('set_head_pose', {'x': float('nan')}),
                ('set_controller_pose', {'hand': True}),
                ('hold_controller_inputs', {'trigger': 2}),
                ('hold_controller_inputs', {'primary': .5}),
                ('set_ipd', {'ipd_mm': 201}),
                ('set_fov', {'left_eye': {}}),
                ('set_ipd', {'clear': 'yes'})):
            with self.subTest(name=name, arguments=arguments), self.assertRaises(ValueError):
                parse_edit(name, arguments)


class BindingTests(unittest.TestCase):
    def setUp(self):
        self.endpoint = Endpoint()
        self.discovery = patch('simxr_control.discover', return_value=['chosen.sock']).start()
        self.factory = patch('simxr_control.Client', return_value=self.endpoint).start()
        self.addCleanup(patch.stopall)
        self.tools = compatibility.LegacyControl('/chosen')
        self.addCleanup(self.tools.close)

    def test_missing_endpoint_and_unsupported_commands_fail_explicitly(self):
        self.discovery.return_value = []
        result = self.tools.call('set_head_pose', {})
        self.assertFalse(result['ok'])
        self.assertIn('No runtime control endpoint', result['error'])
        self.factory.assert_not_called()
        self.discovery.return_value = ['chosen.sock']
        self.endpoint.hello['capabilities'] = 0
        result = self.tools.call('set_head_pose', {})
        self.assertFalse(result['ok'])
        self.assertIn('does not support', result['error'])
        self.assertEqual(self.endpoint.closed, 1)
        self.assertEqual(self.endpoint.scheduled, [])

    def test_one_observation_and_exact_frame_then_blocked_receipt(self):
        result = self.tools.call('set_controller_pose', {'trigger': .75})
        self.assertTrue(result['ok'])
        self.assertFalse(result['applied'])
        self.assertEqual(result['transaction_id'], str(2**63 + 1))
        self.assertEqual(self.endpoint.scheduled[0][2]['frame'], 2**53 + 10)
        self.assertEqual(self.endpoint.observed, [(Session(17, 23), None)])
        blocked = self.tools.call('hold_controller_inputs', {'grip': .8})
        self.assertFalse(blocked['ok'])
        self.assertEqual(blocked['transaction_id'], result['transaction_id'])
        self.assertEqual(len(self.endpoint.scheduled), 1)
        self.endpoint.receipts[2**63 + 1]['phase'] = 'applied'
        applied_next = self.tools.call('hold_controller_inputs', {'grip': .8})
        self.assertTrue(applied_next['ok'])
        self.assertEqual(len(self.endpoint.scheduled), 2)

    def test_nested_ack_identity_is_exact_and_allows_next_explicit_edit(self):
        self.tools.call('set_head_pose', {})
        receipt = self.endpoint.receipts[2**63 + 1]
        receipt['phase'] = 'applied'
        receipt['document']['transaction_id'] = receipt.pop('transaction_id')
        self.assertTrue(self.tools.call('set_ipd', {'ipd_mm': 70})['ok'])
        self.assertEqual(len(self.endpoint.scheduled), 2)
        self.assertEqual(self.endpoint.scheduled[1][1][1]['ipd'], .07)

    def test_failed_ack_requires_explicit_retry_of_new_update(self):
        self.tools.call('set_head_pose', {})
        self.endpoint.receipts[2**63 + 1]['phase'] = 'conflict'
        result = self.tools.call('set_ipd', {'ipd_mm': 70})
        self.assertFalse(result['ok'])
        self.assertEqual(result['phase'], 'conflict')
        self.assertEqual(len(self.endpoint.scheduled), 1)
        self.assertTrue(self.tools.call('set_ipd', {'ipd_mm': 70})['ok'])
        self.assertEqual(len(self.endpoint.scheduled), 2)

    def test_uncertain_admission_closes_source_and_never_replays(self):
        self.endpoint.schedule_error = ControlTimeout(99)
        self.assertFalse(self.tools.call('set_head_pose', {})['ok'])
        self.endpoint.schedule_error = None
        result = self.tools.call('set_head_pose', {'x': 2})
        self.assertFalse(result['ok'])
        self.assertIn('unfinished exchange', result['error'])
        self.assertEqual(len(self.endpoint.scheduled), 1)
        self.assertEqual(self.endpoint.closed, 1)

    def test_new_generation_never_receives_old_pending_or_new_edit(self):
        self.tools.call('set_head_pose', {})
        self.endpoint.current_session = Session(17, 24)
        result = self.tools.call('set_ipd', {'ipd_mm': 70})
        self.assertFalse(result['ok'])
        self.assertIn('session changed', result['error'])
        self.assertEqual(self.endpoint.observed, [(Session(17, 23), None)])
        self.assertEqual(len(self.endpoint.scheduled), 1)
        self.assertEqual(self.endpoint.closed, 1)
        self.assertFalse(self.tools.call('set_ipd', {})['ok'])
        self.assertEqual(self.factory.call_count, 1)

    def test_invalid_edit_does_not_poll_or_admit(self):
        self.assertFalse(self.tools.call('set_head_pose', {'y': float('inf')})['ok'])
        self.assertEqual(self.endpoint.observed, [])
        self.assertEqual(self.endpoint.scheduled, [])

    def test_unknown_or_wrong_ack_is_never_application_success(self):
        for change in ({'phase': 'future_pending'}, {'transaction_id': 777}):
            with self.subTest(change=change):
                endpoint = Endpoint()
                self.factory.return_value = endpoint
                tools = compatibility.LegacyControl('/chosen')
                try:
                    tools.call('set_head_pose', {})
                    endpoint.receipts[2**63 + 1].update(change)
                    self.assertFalse(tools.call('set_head_pose', {})['ok'])
                    self.assertEqual(len(endpoint.scheduled), 1)
                    self.assertEqual(endpoint.closed, 1)
                    self.assertFalse(tools.call('set_head_pose', {})['ok'])
                finally:
                    tools.close()

    def test_close_retires_source_once_and_rejects_late_calls(self):
        self.tools.call('set_head_pose', {})
        self.tools.close()
        self.tools.close()
        self.assertEqual(self.endpoint.closed, 1)
        self.assertFalse(self.tools.call('set_head_pose', {})['ok'])
        self.assertEqual(len(self.endpoint.scheduled), 1)

    def test_structured_remote_error_keeps_code_and_does_not_admit(self):
        self.endpoint.status_error = ControlError({'error': 7, 'detail': 'Stale test generation'})
        result = self.tools.call('set_head_pose', {})
        self.assertEqual(result['code'], 'stale_generation')
        self.assertEqual(self.endpoint.scheduled, [])


if __name__ == '__main__':
    unittest.main()
