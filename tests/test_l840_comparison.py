"""Positive and negative controls for the convergence evaluator itself."""
import unittest
from l840_compare import assess, compare_packets


class ComparisonTests(unittest.TestCase):
    def test_agreement_can_pass(self):
        result = compare_packets([(0, 0), (1000, 65535), (121000, 0)],
                                 [{'at': 1100, 'opcode': 0x8204, 'value': 1},
                                  {'at': 121100, 'opcode': 0x824e, 'value': 0}], 100)
        self.assertEqual(assess([result], [])['status'], 'MATCH')

    def test_wrong_state_fails(self):
        result = compare_packets([(0, 0)], [{'at': 1100, 'opcode': 0x8204, 'value': 1}], 100)
        self.assertEqual(assess([result], [])['status'], 'MISMATCH')
        self.assertEqual(result['mismatches'][0]['predicted_values'], [0])

    def test_transport_uncertainty_is_bounded(self):
        sample = [{'at': 1150, 'opcode': 0x8204, 'value': 1}]
        self.assertEqual(compare_packets([(0, 65535), (1100, 0)], sample, 100)['status'], 'MATCH')
        self.assertEqual(compare_packets([(0, 65535), (900, 0)], sample, 100)['status'], 'MISMATCH')

    def test_missing_inputs_are_not_a_measured_mismatch(self):
        result = compare_packets([(0, 0)], [{'at': 1100, 'opcode': 0x8204, 'value': 0}], 100)
        self.assertEqual(assess([result], ['unknown fade settings'])['status'], 'CONDITIONAL_MATCH')
        self.assertEqual(assess([result], [])['status'], 'MATCH')

    def test_missing_comparison_is_not_a_pass(self):
        self.assertEqual(assess([compare_packets([], [], None)], [])['status'], 'NOT_SCORABLE')


if __name__ == '__main__':
    unittest.main()
