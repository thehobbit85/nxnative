"""The generated guest must reach real counted imports after loader W^X sealing."""
import csv
import sys

with open(sys.argv[1], encoding='utf-8') as report:
    counts = {(row['kind'], row['name']): int(row['count'])
              for row in csv.DictReader(report, delimiter='\t')}
expected = {('HLE', 'test_boot_checkpoint'): 10, ('HLE', 'test_sdk_call'): 2,
            ('FORCE', '_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE'): 2}
for name, count in expected.items():
    if counts.get(name) != count:
        raise SystemExit(f'{name}: expected {count} calls, got {counts.get(name)}')
