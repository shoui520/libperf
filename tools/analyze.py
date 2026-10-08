#!/usr/bin/env python3
"""Print a terminal report or JSON for libperf function CSVs and stress logs."""

import argparse
import csv
import io
import json
import re
import statistics
import sys
from pathlib import Path


METRICS = ('cycles', 'us') + tuple(f'event{i}' for i in range(6))
COLUMNS = ('kind', 'bank', 'function', 'run', 'calls') + METRICS
STAGES = ('generate', 'neon', 'rle', 'validate')
APIS = ('reset', 'select', 'start', 'stop', 'get', 'set', 'software', 'time', 'frequency')
STRESS_EVENTS = (
    (('rename', 0x68), ('dcache_miss', 0x03), ('dtlb_miss', 0x05), ('branch_miss', 0x10)),
    (('vfp_rename', 0x73), ('neon_rename', 0x74), ('dcache_stall', 0x61), ('stores', 0x07)),
    (('strex_pass', 0x63), ('strex_fail', 0x64), ('eviction', 0x65), ('main_pipe', 0x70)),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def readFunction(text):
    lines = text.splitlines()
    require(lines and lines[0].startswith('# CPU Function Profiler,'),
            'missing function profiler header')
    metadata = {}
    for item in lines[0].split(',')[1:]:
        key, value = item.split('=', 1)
        require(key not in metadata, 'duplicate metadata field')
        metadata[key] = int(value)
    require(all(key in metadata for key in
                ('core', 'repeats', 'calls_per_batch', 'clock_before_mhz',
                 'clock_after_mhz', 'timebase_mhz')), 'missing capture metadata')
    repeats, calls = metadata['repeats'], metadata['calls_per_batch']
    require(metadata['core'] in (0, 1, 2) and repeats > 0 and calls > 0,
            'invalid core, repeats or calls per batch')
    require(all(metadata[key] > 0 for key in
                ('clock_before_mhz', 'clock_after_mhz', 'timebase_mhz')),
            'invalid clock metadata')
    definitions = {}
    for line in lines[1:]:
        if not line.startswith('# bank'):
            continue
        match = re.fullmatch(r'# bank(\d+),(.+)', line)
        require(match is not None, 'invalid bank definition')
        bank = int(match[1])
        require(bank not in definitions, 'duplicate bank definition')
        events = []
        for index, item in enumerate(match[2].split(',')):
            event = re.fullmatch(r'event(\d+)=(\w+)\(0x([0-9a-fA-F]{1,2})\)', item)
            require(event is not None and int(event[1]) == index, 'invalid event definition')
            events.append({'slot': index, 'name': event[2], 'code': int(event[3], 16)})
        require(len(events) == 6, 'each bank must define six events')
        definitions[bank] = events
    require(definitions, 'missing event bank definitions')
    reader = csv.DictReader(io.StringIO('\n'.join(line for line in lines
                                                if not line.startswith('#'))), strict=True)
    require(tuple(reader.fieldnames or ()) == COLUMNS, 'invalid CSV columns')
    records = {}
    for lineNumber, row in enumerate(reader, 1):
        require(None not in row and all(value is not None for value in row.values()),
                f'invalid CSV row {lineNumber}')
        kind, name = row['kind'], row['function']
        require(kind in ('baseline', 'raw', 'median_adjusted') and name,
                f'invalid row kind or function at row {lineNumber}')
        values = {key: int(row[key]) for key in COLUMNS if key not in ('kind', 'function')}
        bank, run = values['bank'], values['run']
        require(bank in definitions and values['calls'] == calls,
                f'invalid bank or call count at row {lineNumber}')
        require(run == -1 if kind == 'median_adjusted' else 0 <= run < repeats,
                f'invalid repetition at row {lineNumber}')
        require(all(values[key] >= 0 for key in METRICS), 'negative metric')
        require(kind != 'baseline' or name == 'empty', 'invalid baseline function')
        key = (kind, bank, name, run)
        require(key not in records, f'duplicate CSV record {key}')
        records[key] = values
    names = sorted({key[2] for key in records if key[0] != 'baseline'})
    require(names, 'no function measurements')
    expected = len(definitions) * (repeats + len(names) * (repeats + 1))
    require(len(records) == expected, 'incomplete function capture')
    banks = []
    for bank, events in sorted(definitions.items()):
        baselines = [records.get(('baseline', bank, 'empty', run)) for run in range(repeats)]
        require(all(baselines), f'missing baseline in bank {bank}')
        functions = []
        for name in names:
            raw = [records.get(('raw', bank, name, run)) for run in range(repeats)]
            reported = records.get(('median_adjusted', bank, name, -1))
            require(all(raw) and reported is not None, f'missing samples for {name}, bank {bank}')
            adjusted = [{key: max(0, sample[key] - baseline[key]) for key in METRICS}
                        for sample, baseline in zip(raw, baselines)]
            medians = {key: statistics.median(sample[key] for sample in adjusted)
                       for key in METRICS}
            require(all(reported[key] == medians[key] for key in METRICS),
                    f'reported median does not match samples: {name}, bank {bank}')
            cycleSamples = [sample['cycles'] / calls for sample in adjusted]
            median = medians['cycles'] / calls
            functions.append({
                'name': name,
                'cycles_per_call': median,
                'wall_us_per_call': medians['us'] / calls,
                'cycle_min_per_call': min(cycleSamples),
                'cycle_max_per_call': max(cycleSamples),
                'cycle_span_pct': (max(cycleSamples) - min(cycleSamples)) / median * 100
                if median else None,
                'events_per_call': [medians[f'event{i}'] / calls for i in range(6)],
                'raw_batches': raw,
                'adjusted_batches': adjusted,
            })
        total = sum(row['cycles_per_call'] for row in functions)
        for row in functions:
            row['equal_call_cycle_share_pct'] = row['cycles_per_call'] / total * 100 if total else None
        costs = {row['name']: row['cycles_per_call'] for row in functions}
        comparisons = [
            {'function': left, 'reference': right, 'cycle_cost_ratio': costs[left] / costs[right]}
            for left, right in (('branch_random', 'branch_predictable'),
                                ('cache_cold', 'cache_hot'), ('scalar_float', 'neon_float'))
            if left in costs and costs.get(right)
        ]
        banks.append({'bank': bank, 'events': events, 'baseline_batches': baselines,
                      'functions': functions, 'comparisons': comparisons})
    warnings = [
        'Banks are separate passes; do not combine their counters as simultaneous measurements.',
        'Each metric is a separate median of baseline-subtracted batches (clamped at zero), divided by calls.',
        'Sample span is observed variation, not a confidence interval.',
        'Wall time includes descheduling; PMU cycles and instruction rename events are not wall time or retired instructions.',
        'Shares describe one call of each measured function, not application CPU utilization.',
    ]
    if metadata['clock_before_mhz'] != metadata['clock_after_mhz']:
        warnings.append('CPU clock changed between endpoint readings.')
    warnings.append('Clock endpoints do not rule out changes during collection.')
    return {'kind': 'function', 'status': 'complete', 'metadata': metadata,
            'banks': banks, 'warnings': warnings}


def keyValues(text):
    result = {}
    for item in re.split(r'[ ,]+', text):
        key, value = item.split('=', 1)
        require(key not in result, f'duplicate field {key}')
        result[key] = int(value)
        require(result[key] >= 0, f'negative field {key}')
    return result


def readStress(text):
    parts = re.split(r'^START ', text, flags=re.MULTILINE)
    require(len(parts) > 1, 'missing stress START record')
    runs = []
    for part in parts[1:]:
        lines = part.splitlines()
        metadata = keyValues(lines[0])
        require(set(metadata) == {'epochs', 'cores', 'jobs/epoch', 'sizeofQueue'}
                and metadata['cores'] == 3 and metadata['jobs/epoch'] > 0,
                'invalid stress START record')
        workers, stages, events, passes, apis = {}, {}, {}, {}, {}
        failures, lifecycle = [], []
        result = None
        for line in lines[1:]:
            if line.startswith(('worker,', 'stages,', 'events,')):
                kind, epoch, core, fields = line.split(',', 3)
                key = (int(epoch), int(core))
                require(key[0] >= 0 and key[1] in (0, 1, 2), 'invalid epoch or core')
                values = keyValues(fields)
                target, expected = {
                    'worker': (workers, ('jobs', 'sw', 'cycles', 'us')),
                    'stages': (stages, STAGES),
                    'events': (events, ('c1', 'c2', 'c3', 'c4', 'c5')),
                }[kind]
                require(key not in target and set(values) == set(expected),
                        f'invalid or duplicate {kind} record')
                target[key] = values
            elif line.startswith('RESULT '):
                require(result is None, 'duplicate stress RESULT')
                _, status, fields = line.split(' ', 2)
                require(status in ('PASS', 'FAIL', 'CANCELLED'), 'unknown stress status')
                result = {'status': status, **keyValues(fields)}
                require(set(result) == {'status', 'completed_epochs', 'jobs', 'transient_threads'},
                        'invalid stress RESULT fields')
            elif line.startswith('API '):
                _, name, fields = line.split(' ', 2)
                values = keyValues(fields)
                require(name in APIS and name not in apis and set(values) == {'calls'},
                        'invalid API record')
                apis[name] = values['calls']
            elif line.startswith('FAIL'):
                failures.append(line)
            elif re.match(r'epoch=\d+ PASS ', line):
                fields = keyValues(line.replace(' PASS ', ' '))
                require(set(fields) == {'epoch', 'jobs', 'churn', 'live_reads', 'broadcasts', 'vanished'},
                        'invalid epoch PASS fields')
                epoch = fields.pop('epoch')
                require(epoch not in passes, 'duplicate epoch PASS record')
                passes[epoch] = fields
            elif ' lifecycle:' in line:
                match = re.fullmatch(r'epoch=(\d+) lifecycle: workers parked, closing PMU', line)
                require(match is not None, 'invalid lifecycle record')
                lifecycle.append(int(match[1]))
            elif line.startswith(('epoch=', 'Report:', 'Three-core')) or not line:
                require(not line.startswith('epoch=') or
                        re.fullmatch(r'epoch=\d+ BEGIN probe/wrap/migration/control', line),
                        'invalid epoch record')
            else:
                raise ValueError(f'unknown stress record: {line!r}')
        completed = len(passes)
        require(set(passes) == set(range(completed)), 'noncontiguous completed epochs')
        if result:
            require(result['completed_epochs'] == completed
                    and result['jobs'] == completed * metadata['jobs/epoch'],
                    'stress result disagrees with completed epochs')
            require(result['transient_threads'] >= completed * 12,
                    'stress transient-thread count is too small')
            if result['status'] == 'PASS':
                require(not failures and (metadata['epochs'] == 0 or completed == metadata['epochs'])
                        and result['transient_threads'] == completed * 12,
                        'inconsistent PASS result')
                require(set(workers) == set(stages) == set(events)
                        == {(epoch, core) for epoch in range(completed) for core in range(3)},
                        'PASS report contains unmatched or extra worker data')
        for epoch in range(completed):
            keys = [(epoch, core) for core in range(3)]
            require(all(key in workers and key in stages and key in events for key in keys),
                    f'missing worker data in completed epoch {epoch}')
            require(sum(workers[key]['jobs'] for key in keys) == metadata['jobs/epoch'],
                    'worker jobs disagree with epoch total')
            require(all(workers[key]['sw'] == workers[key]['jobs'] * 4
                        and workers[key]['cycles'] > 0 and workers[key]['us'] > 0 for key in keys),
                    'worker software increments or timing disagree')
            require(passes[epoch]['jobs'] == metadata['jobs/epoch']
                    and passes[epoch]['churn'] == (epoch + 1) * 12,
                    'epoch accounting disagrees')
            if epoch:
                require(all(passes[epoch][key] >= passes[epoch - 1][key]
                            for key in ('live_reads', 'broadcasts', 'vanished')),
                        'cumulative activity decreased')
        rows = []
        cores = []
        for core in range(3):
            keys = [(epoch, core) for epoch in range(completed)]
            jobs = sum(workers[key]['jobs'] for key in keys)
            stageTotals = {stage: sum(stages[key][stage] for key in keys) for stage in STAGES}
            cores.append({'core': core, 'jobs': jobs,
                          'worker_cycle_total': sum(workers[key]['cycles'] for key in keys),
                          'worker_wall_us_total': sum(workers[key]['us'] for key in keys),
                          'stage_cycle_totals': stageTotals,
                          'stage_cycles_per_job': {stage: value / jobs if jobs else None
                                                   for stage, value in stageTotals.items()}})
            for key in keys:
                rows.append({'epoch': key[0], 'core': core, **workers[key],
                             'stage_cycles': stages[key], 'event_bank': key[0] % 3,
                             'events': events[key]})
        eventBanks = []
        for bank, definitions in enumerate(STRESS_EVENTS):
            bankRows = [row for row in rows if row['event_bank'] == bank]
            jobs = sum(row['jobs'] for row in bankRows)
            eventBanks.append({'bank': bank, 'jobs': jobs, 'events': [
                {'slot': i, 'name': name, 'code': code,
                 'total': sum(row['events'][f'c{i}'] for row in bankRows),
                 'per_job': sum(row['events'][f'c{i}'] for row in bankRows) / jobs if jobs else None}
                for i, (name, code) in enumerate(definitions, 1)]})
        warnings = [
            'Stage costs are gross measured spans including measurement overhead and contention.',
            'Per-job costs are job-weighted averages, not medians. Core job counts are not CPU utilization.',
            'Worker wall times overlap across cores; do not add them to estimate elapsed runtime.',
            'Event banks rotate by epoch. Counter 5 changes during live broadcasts and has no fixed event attribution.',
            'VFP/NEON rename counts are not retired instructions; Cortex-A9 PMU errata can affect interpretation.',
        ]
        if not result:
            warnings.append('No RESULT footer: capture is incomplete; only completed epochs are summarized.')
        if set(apis) != set(APIS):
            warnings.append('API footer is incomplete.')
        require(not result or result['status'] != 'PASS' or set(apis) == set(APIS),
                'PASS report is missing API footer')
        require(len(set(lifecycle)) == len(lifecycle), 'duplicate lifecycle record')
        require(all(epoch in lifecycle for epoch in range(0, completed, 4)),
                'missing lifecycle phase in completed epochs')
        if any(key[0] >= completed for key in workers):
            warnings.append('Partial epoch worker data is excluded from cost summaries.')
        runs.append({'kind': 'stress', 'status': result['status'] if result else 'INCOMPLETE',
                     'metadata': metadata, 'result': result, 'completed_epochs': completed,
                     'cores': cores, 'event_banks': eventBanks, 'worker_samples': rows,
                     'api_calls': apis, 'last_epoch_activity': passes.get(completed - 1),
                     'lifecycle_epochs': lifecycle, 'failures': failures, 'warnings': warnings})
    return runs


def safeLabel(value, width=32):
    text = str(value).encode('unicode_escape').decode('ascii')
    return text if len(text) <= width else text[:width - 1] + '…'


def formatNumber(value, decimals=1):
    return '—' if value is None else f'{value:,.{decimals}f}'


def formatReport(captures, sortMetric, selectedBank):
    lines = ['', '  libperf · CPU profiling', '  ' + '─' * 96]
    for capture in captures:
        lines += ['', '  ' + safeLabel(capture['source'], 90)]
        for report in capture['runs']:
            if report['kind'] == 'function':
                meta = report['metadata']
                lines += [f"  Core {meta['core']} · {meta['clock_before_mhz']} → "
                          f"{meta['clock_after_mhz']} MHz · {meta['repeats']} repetitions × "
                          f"{meta['calls_per_batch']} calls/batch · medians verified"]
                for bank in report['banks']:
                    if selectedBank is not None and bank['bank'] != selectedBank:
                        continue
                    rows = sorted(bank['functions'], key=lambda row: row[sortMetric], reverse=True)
                    maximum = max(row[sortMetric] for row in rows)
                    lines += ['', f"  Bank {bank['bank']} · baseline-adjusted cost per call", '',
                              f"  {'Function':24} {'Relative cost':14} {'Cycles':>12} "
                              f"{'Wall µs':>11} {'Share':>7} {'Span':>8}"]
                    for row in rows:
                        length = round(14 * row[sortMetric] / maximum) if maximum else 0
                        bar = '█' * length or '·'
                        lines.append(f"  {safeLabel(row['name'], 24):24} {bar:14} "
                                     f"{formatNumber(row['cycles_per_call']):>12} "
                                     f"{formatNumber(row['wall_us_per_call']):>11} "
                                     f"{formatNumber(row['equal_call_cycle_share_pct']):>6}% "
                                     f"{formatNumber(row['cycle_span_pct']):>7}%")
                    lines += ['', '  Share = equal-call cycle share · Span = cycle (max − min) / median',
                              '', '  Events per call · each column has its own median',
                              f"  {'Function':24} " + ' '.join(f"{safeLabel(event['name'], 13):>13}" for event in bank['events'])]
                    for row in rows:
                        lines.append(f"  {safeLabel(row['name'], 24):24} " +
                                     ' '.join(f'{formatNumber(value):>13}' for value in row['events_per_call']))
                    for comparison in bank['comparisons']:
                        lines.append(f"  {comparison['function']} / {comparison['reference']}  "
                                     f"{comparison['cycle_cost_ratio']:.3f}× cycles")
            else:
                lines += [f"  Three-core stress · {report['status']} · "
                          f"{report['completed_epochs']:,} completed epochs", '',
                          '  Measured stage cycles per job · weighted averages', '',
                          f"  {'Core':6} {'Jobs':>9} " +
                          ' '.join(f'{stage:>13}' for stage in STAGES)]
                for core in report['cores']:
                    lines.append(f"  {core['core']:<6} {core['jobs']:>9,} " + ' '.join(
                        f"{formatNumber(core['stage_cycles_per_job'][stage]):>13}" for stage in STAGES))
                lines += ['', '  Stable event counters · counts per job, across cores']
                for bank in report['event_banks']:
                    lines.append(f"  Bank {bank['bank']} ({bank['jobs']:,} jobs)  " + ' · '.join(
                        f"{event['name']} {formatNumber(event['per_job'])}" for event in bank['events']))
                activity = report['last_epoch_activity']
                result = report['result']
                if result:
                    lines += ['', f"  Verified jobs {result['jobs']:,} · transient threads "
                              f"{result['transient_threads']:,} · close/reopen phases "
                              f"{len(report['lifecycle_epochs']):,}"]
                if activity:
                    lines += ['', f"  Live reads {activity['live_reads']:,} · broadcasts "
                              f"{activity['broadcasts']:,} · vanished-thread broadcasts "
                              f"{activity['vanished']:,}"]
                lines += ['', '  API calls  ' + ' · '.join(
                    f"{name} {report['api_calls'][name]:,}" for name in APIS if name in report['api_calls'])]
                lines.extend('  ' + safeLabel(failure, 160) for failure in report['failures'])
            lines += ['', '  Interpretation']
            lines.extend('  • ' + warning for warning in report['warnings'])
            lines += ['', '  ' + '─' * 96]
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog='Examples: analyze.py results.csv --bank 0; analyze.py results.txt --json. '
               'Multiple files are kept as separate captures. Valid FAIL/CANCELLED/incomplete '
               'stress logs retain their recorded status; malformed data exits with code 2. '
               'Python 3.8+; no third-party packages required.')
    parser.add_argument('captures', nargs='+', help='function CSVs or stress logs; - reads stdin')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--json', action='store_true', help='emit complete structured analysis (schema v1)')
    mode.add_argument('--tui', action='store_true', help='print the static terminal report (default)')
    parser.add_argument('--bank', type=int, help='show one function event bank in the terminal report')
    parser.add_argument('--sort', choices=('cycles', 'wall'), default='cycles',
                        help='rank function costs by cycles or wall time (default: cycles)')
    args = parser.parse_args()
    envelope = {'schema': 'libperf.analysis', 'schema_version': 1, 'status': 'ok', 'captures': []}
    try:
        require(args.captures.count('-') <= 1, 'stdin may only be read once')
        require(not args.json or (args.bank is None and args.sort == 'cycles'),
                '--bank and --sort are terminal presentation options; JSON always contains all data')
        for name in args.captures:
            text = sys.stdin.read() if name == '-' else Path(name).read_text(encoding='utf-8-sig')
            if text.startswith('\ufeff'):
                text = text[1:]
            if text.startswith('# CPU Function Profiler,'):
                runs = [readFunction(text)]
                require(args.bank is None or any(bank['bank'] == args.bank for bank in runs[0]['banks']),
                        f'event bank {args.bank} is not present')
            else:
                runs = readStress(text)
            envelope['captures'].append({'source': Path(name).name if name != '-' else 'stdin', 'runs': runs})
    except (OSError, ValueError, csv.Error) as exc:
        if args.json:
            print(json.dumps({'schema': envelope['schema'], 'schema_version': 1, 'status': 'error',
                              'error': {'code': 'capture_read_error', 'message': str(exc)}},
                             ensure_ascii=True))
        else:
            print(f'libperf: {ascii(str(exc))[1:-1]}', file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(envelope, indent=2, allow_nan=False, ensure_ascii=True))
    else:
        metric = 'cycles_per_call' if args.sort == 'cycles' else 'wall_us_per_call'
        output = formatReport(envelope['captures'], metric, args.bank)
        # Pipes and terminals using an ASCII locale still get a readable report.
        sys.stdout.write(output.encode(sys.stdout.encoding or 'utf-8', errors='replace')
                         .decode(sys.stdout.encoding or 'utf-8'))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(0)
