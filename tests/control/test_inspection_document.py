"""Cross-language historical state and hostile inspection-tree qualification."""
from pathlib import Path
import json
import struct
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control.protocol import Field, ProtocolError, decode_inspection, encode_fields


def node(kind, data=b'', name=''):
    return encode_fields([Field.text(1, name, True), Field.u32(2, kind, True), Field(3, data, True)])


def object_value(items):
    return node(6, encode_fields([Field(index, value, True) for index, value in enumerate(items, 1)]))


class InspectionTests(unittest.TestCase):
    def test_cpp_historical_fixture(self):
        fixture = subprocess.run([BINARY, '--fixture'], check=True, capture_output=True).stdout
        value = decode_inspection(fixture)
        json.dumps(value, allow_nan=False)
        frame = value['frame']
        revisions = {item['revision_id']: item for item in value['revisions']}
        self.assertEqual(frame['generation'], 0xfedcba9876543210)
        self.assertEqual(frame['frame_id'], 0x123456789abcdef0)
        self.assertEqual(frame['input']['revision'], 99)
        self.assertEqual(frame['input']['controllers'][0]['trigger'], 0.25)
        self.assertEqual(frame['tracking']['head']['pose']['position']['x'], 0.75)
        self.assertEqual(frame['tracking']['input']['body']['preset'], 'seated')
        self.assertEqual(len(frame['tracking']['body']['joints']), 84)
        skeleton = revisions[frame['tracking']['body']['skeleton']['revision_id']]
        self.assertEqual(skeleton['kind'], 'body_skeleton_v1')
        self.assertEqual(len(skeleton['document']['joints']), len(frame['tracking']['body']['joints']))
        actions = frame['actions']
        dictionary = revisions[actions['dictionary']['revision_id']]['document']
        self.assertEqual(dictionary['sets'][0]['name'], 'historical')
        self.assertEqual(dictionary['sets'][0]['priority'], 41)
        self.assertEqual(dictionary['actions'][0]['name'], 'select')
        self.assertEqual(actions['actions'][0]['id'], dictionary['actions'][0]['id'])
        self.assertTrue(any(value['boolean'] and value['active'] for value in actions['actions'][0]['values']))
        self.assertIn('/user/hand/right', [item['user_path'] for item in actions['profiles']])
        self.assertEqual(frame['haptics'][0]['action'], dictionary['actions'][0]['id'])
        self.assertEqual(frame['haptics'][0]['status'], 'stopped')
        layers = frame['layers']['layers']
        self.assertEqual([layer['kind'] for layer in layers], ['projection', 'quad'])
        self.assertEqual(layers[0]['space']['source'], layers[1]['space']['source'])
        self.assertEqual(layers[0]['space']['reference_type'], 2)  # XR_REFERENCE_SPACE_TYPE_LOCAL
        self.assertEqual(layers[0]['space']['pose_offset']['position']['y'], 1.0)
        self.assertEqual(layers[0]['views'][0]['color']['source'], layers[1]['color']['source'])
        self.assertEqual(layers[0]['views'][0]['depth']['image']['source'], 1)
        self.assertEqual(layers[0]['views'][0]['color']['source'], 2)
        self.assertEqual(layers[0]['views'][0]['color']['array_slice'], 2)
        self.assertEqual(layers[0]['views'][0]['color']['rectangle'], {'x': 4, 'y': 8, 'width': 12, 'height': 16})
        self.assertEqual(layers[0]['views'][0]['depth']['far_z'], '+Infinity')
        for key, expected in {
                'tracking': 'recorded', 'action_dictionary': 'recorded',
                'applied_foveation': 'not_recorded', 'applied_commands': 'not_recorded',
                'fb_foveation': 'unsupported', 'validation_scope': 'runtime_submitted_layers',
                'native_driver_validation': 'not_recorded', 'validation_events': 'not_recorded',
                'frame_trace': 'not_recorded'}.items():
            self.assertEqual(frame['availability'][key], expected, key)

    def test_scalar_widths_and_nonfinite(self):
        self.assertEqual(decode_inspection(node(2, struct.pack('<Q', 2**64 - 1))), 2**64 - 1)
        self.assertEqual(decode_inspection(node(3, struct.pack('<q', -2**63))), -2**63)
        self.assertEqual(decode_inspection(node(4, struct.pack('<I', 0x7fc01234))), 'NaN')
        self.assertEqual(decode_inspection(node(4, struct.pack('<f', float('-inf')))), '-Infinity')
        self.assertEqual(decode_inspection(node(8, struct.pack('<Q', 2**64 - 1))), {'revision_id': 2**64 - 1})
        self.assertEqual(Field.f32(1, 0.25).value, b'\0\0\x80>')
        for kind, data in ((0, b'x'), (1, b'\2'), (2, b'\0'), (3, b'\0' * 4), (4, b'\0' * 8),
                           (5, b'\xc0\x80'), (8, b'\0' * 8), (9, b'')):
            with self.assertRaises(ProtocolError):
                decode_inspection(node(kind, data))

    def test_arrays_over_tlv_field_boundary(self):
        pages = []
        for begin in (0, 256):
            children = [Field(index + 1, node(2, struct.pack('<Q', value)), True)
                        for index, value in enumerate(range(begin, min(begin + 256, 300)))]
            pages.append(Field(len(pages) + 1, encode_fields(children), True))
        self.assertEqual(decode_inspection(node(7, encode_fields(pages))), list(range(300)))
        for pages in ([Field(2, b'')], [Field(1, b'')],
                      [Field(1, encode_fields([Field(1, node(0))])), Field(2, encode_fields([Field(1, node(0))]))]):
            with self.assertRaises(ProtocolError):
                decode_inspection(node(7, encode_fields(pages)))

    def test_objects_depth_and_evolution(self):
        for items in ([node(0, name='')], [node(0, name='x'), node(0, name='x')]):
            with self.assertRaises(ProtocolError):
                decode_inspection(object_value(items))
        value = node(0, name='x')
        for _ in range(34):
            value = node(6, encode_fields([Field(1, value)]), name='x')
        with self.assertRaises(ProtocolError):
            decode_inspection(node(6, encode_fields([Field(1, value)])))
        base = [Field.text(1, '', True), Field.u32(2, 0, True), Field(3, b'', True)]
        self.assertIsNone(decode_inspection(encode_fields(base + [Field(65000, b'future')])) )
        with self.assertRaises(ProtocolError):
            decode_inspection(encode_fields(base + [Field(65000, b'future', True)]))
        with self.assertRaises(ProtocolError):
            decode_inspection(encode_fields(base[:2]))


if __name__ == '__main__':
    BINARY = sys.argv.pop(1)
    unittest.main()
