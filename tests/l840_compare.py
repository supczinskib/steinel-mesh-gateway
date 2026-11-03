"""Held-out packet comparison. Does not select or modify simulation parameters."""
from bisect import bisect_right


def compare_packets(trajectory, samples, transport_bound_ms, tick_ms=10):
    if not trajectory or not samples or transport_bound_ms is None:
        return {'status': 'NOT_SCORABLE', 'checked': 0, 'mismatches': []}
    times = [time for time, _ in trajectory]
    levels = [level for _, level in trajectory]
    mismatches = []
    for sample in samples:
        # A reported present value was sampled before RX. The observed diagnostic
        # RTT supplies a declared comparison bound; it is not a radio guarantee.
        start = max(0, sample['at'] - transport_bound_ms - tick_ms)
        end = sample['at'] + tick_ms
        left = max(0, bisect_right(times, start) - 1)
        right = max(0, bisect_right(times, end) - 1)
        possible = levels[left:right + 1]
        if sample['opcode'] == 0x8204:
            possible = [int(level != 0) for level in possible]
        if sample['value'] not in possible:
            mismatches.append({'at_ms': sample['at'], 'opcode': sample['opcode'],
                               'observed': sample['value'], 'predicted_values': sorted(set(possible))})
    return {'status': 'MATCH' if not mismatches else 'MISMATCH', 'checked': len(samples), 'mismatches': mismatches}


def assess(profiles, unresolved):
    scored = [profile for profile in profiles if profile['status'] != 'NOT_SCORABLE']
    matching = [index for index, profile in enumerate(profiles) if profile['status'] == 'MATCH']
    if not scored:
        status = 'NOT_SCORABLE'
    elif not matching:
        status = 'MISMATCH'
    else:
        status = 'CONDITIONAL_MATCH' if unresolved else 'MATCH'
    return {'status': status, 'profiles_checked': len(scored), 'matching_profile_indices': matching,
            'unresolved': list(unresolved)}
