"""General region measurements; independent of any application or event bank."""

import csv
import io
import re


ALIASES = {
    'region': ('region', 'label', 'function', 'name', 'scope'),
    'thread': ('thread', 'thread_id', 'tid'),
    'process': ('process', 'process_id', 'pid'),
    'session': ('session',),
    'core': ('core', 'cpu'),
    'bank': ('bank',),
    'calls': ('calls',),
    'pmu_calls': ('pmu_calls',),
    'wall_us': ('wall_us', 'duration_us', 'us'),
    'exclusive_wall_us': ('exclusive_wall_us', 'exclusive_us'),
    'cycles': ('cycles', 'cycle_count', 'cycle_delta'),
    'exclusive_cycles': ('exclusive_cycles',),
    'timestamp_us': ('timestamp_us', 'elapsed_us'),
    'max_us': ('max_us',),
    'reset': ('reset',),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, field):
    require(re.fullmatch(r'[0-9]+', value or '') is not None,
            f'{field}: expected a nonnegative integer')
    return int(value)


def readTrace(text, columns=(), events=(), counterMode='delta', counterBits=32):
    """Normalize CSV without inventing event meanings or missing measurements."""
    lines = text.splitlines()
    preamble = 0
    while preamble < len(lines) and (not lines[preamble].strip() or lines[preamble].startswith('#')):
        preamble += 1
    comments = [line for line in lines[:preamble] if line.startswith('#')]
    versions = [line for line in comments if line.startswith('# libperf.csv,')]
    require(not versions or versions == ['# libperf.csv,version=1'],
            'unsupported or duplicate libperf CSV version')
    body = '\n'.join(lines[preamble:])
    require(body.strip(), 'empty CSV')
    try:
        dialect = csv.Sniffer().sniff(body[:8192], delimiters=',;\t')
    except csv.Error:
        dialect = csv.excel
    reader = csv.DictReader(io.StringIO(body), dialect=dialect, strict=True)
    headers = reader.fieldnames or []
    require(headers and len(set(headers)) == len(headers), 'missing or duplicate CSV headers')
    mapping = {}
    explicit = {}
    for item in columns:
        require('=' in item, '--column requires FIELD=HEADER')
        field, header = item.split('=', 1)
        require(field in ALIASES and field not in explicit, 'unknown or repeated column role')
        require(header in headers, f'missing mapped header {header}')
        explicit[field] = header
    for field, aliases in ALIASES.items():
        matches = [name for name in aliases if name in headers and
                   (field in explicit or name not in explicit.values())]
        require(field in explicit or len(matches) <= 1,
                f'ambiguous {field}; select it with --column')
        if field in explicit or matches:
            mapping[field] = explicit.get(field, matches[0] if matches else None)
    require(len(set(mapping.values())) == len(mapping), 'one header mapped to multiple roles')
    require('region' in mapping, 'missing region column; use --column region=HEADER')
    definitions = {}
    for header in headers:
        if header in mapping.values():
            continue
        match = re.fullmatch(r'(exclusive_)?event\.(.+?)(?:\.(0x[0-9a-fA-F]{1,2}))?', header)
        if match:
            definitions[header] = {'name': match[2],
                                   'code': int(match[3], 16) if match[3] else None,
                                   'scope': 'exclusive' if match[1] else 'inclusive'}
        elif re.fullmatch(r'(exclusive_)?event[0-9]+', header):
            definitions[header] = {'name': header[len('exclusive_'):] if header.startswith('exclusive_') else header,
                                   'code': None,
                                   'scope': 'exclusive' if header.startswith('exclusive_') else 'inclusive'}
    for item in events:
        header, separator, code = item.partition('=')
        require(header in headers and header not in mapping.values(), f'invalid event header {header}')
        eventCode = int(code, 0) if separator else None
        require(eventCode is None or 0 <= eventCode <= 255, 'event code must fit in eight bits')
        definitions[header] = {'name': header[len('exclusive_'):] if header.startswith('exclusive_') else header,
                               'code': eventCode,
                               'scope': 'exclusive' if header.startswith('exclusive_') else 'inclusive'}
    require(any(field in mapping for field in ('cycles', 'exclusive_cycles', 'wall_us',
                                              'exclusive_wall_us')) or definitions,
            'no measurements; select counter columns with --event HEADER[=0xCODE]')
    require(counterMode != 'cumulative' or 'timestamp_us' in mapping,
            'cumulative snapshots require timestamp_us')
    require(counterMode != 'cumulative' or not any(field in mapping for field in
            ('calls', 'pmu_calls', 'wall_us', 'exclusive_wall_us', 'max_us')),
            'cumulative mode accepts timestamped raw PMU snapshots, not region totals')
    numeric = set(ALIASES) - {'region', 'thread', 'process', 'session', 'core', 'bank'}
    counterFields = set(definitions) | (set(mapping) & {'cycles', 'exclusive_cycles'})
    samples, previous = [], {}
    used = set(mapping.values()) | set(definitions)
    for rowNumber, row in enumerate(reader, 2):
        require(None not in row and all(value is not None for value in row.values()),
                f'wrong field count at CSV record {rowNumber}')
        sample = {field: (integer(row[header], field) if field in numeric and row[header] else
                          None if not row[header] else row[header])
                  for field, header in mapping.items()}
        require(sample.get('region'), f'empty region at CSV record {rowNumber}')
        require(sample.get('reset') in (None, 0, 1), 'reset must be 0 or 1')
        sample['events'] = {header: integer(row[header], header) if row[header] else None
                            for header in definitions}
        sample['extra'] = {header: row[header] for header in headers if header not in used}
        sample['record'] = rowNumber
        if counterMode == 'cumulative':
            key = tuple(sample.get(field) for field in ('process', 'thread', 'session', 'bank', 'core'))
            require(sample.get('timestamp_us') is not None, 'missing snapshot timestamp')
            values = {field: sample['events'][field] if field in definitions else sample.get(field)
                      for field in counterFields}
            require(all(value is None or value < 1 << counterBits for value in values.values()),
                    'snapshot exceeds counter width')
            prior = previous.get(key)
            previous[key] = (sample['timestamp_us'], values)
            if prior is None or sample.get('reset'):
                sample['baseline'] = True
            else:
                require(sample['timestamp_us'] >= prior[0], 'snapshot timestamps go backwards')
                sample['wall_us'] = sample['timestamp_us'] - prior[0]
                sample['baseline'] = False
                sample['snapshots'] = values.copy()
                for field, value in values.items():
                    delta = ((value - prior[1][field]) % (1 << counterBits)
                             if value is not None and prior[1][field] is not None else None)
                    if field in definitions:
                        sample['events'][field] = delta
                    else:
                        sample[field] = delta
        calls = sample.get('calls', 1)
        require(calls is not None and calls > 0, 'calls must be positive when present')
        pmuCalls = sample.get('pmu_calls', calls)
        require(pmuCalls is not None and 0 <= pmuCalls <= calls, 'invalid PMU call coverage')
        sample['calls'], sample['pmu_calls'] = calls, pmuCalls
        if pmuCalls == 0:
            require(all(value in (None, 0) for value in sample['events'].values()) and
                    all(sample.get(field) in (None, 0) for field in ('cycles', 'exclusive_cycles')),
                    'nonzero PMU counts with zero PMU calls')
        samples.append(sample)
    require(samples, 'no CSV measurements')
    groups = {}
    for sample in samples:
        if sample.get('baseline'):
            continue
        key = tuple(sample.get(field) for field in ('region', 'process', 'thread', 'session', 'bank'))
        group = groups.setdefault(key, {'region': key[0], 'process': key[1], 'thread': key[2],
                                       'session': key[3], 'bank': key[4], 'cores': [],
                                       'calls': 0, 'pmu_calls': 0, 'rows': 0, 'metrics': {}})
        group['calls'] += sample['calls']
        group['pmu_calls'] += sample['pmu_calls']
        group['rows'] += 1
        if sample.get('core') is not None and sample['core'] not in group['cores']:
            group['cores'].append(sample['core'])
        values = {field: sample.get(field) for field in ('cycles', 'exclusive_cycles', 'wall_us',
                                                       'exclusive_wall_us', 'max_us')}
        values.update(sample['events'])
        for field, value in values.items():
            isPmu = field in counterFields
            if value is None or (isPmu and sample['pmu_calls'] == 0):
                continue
            metric = group['metrics'].setdefault(field, {'total': 0, 'calls': 0, 'rows': 0})
            metric['total'] = max(metric['total'], value) if field == 'max_us' else metric['total'] + value
            metric['calls'] += sample['pmu_calls'] if isPmu else sample['calls']
            metric['rows'] += 1
    for group in groups.values():
        group['pmu_coverage_pct'] = 100 * group['pmu_calls'] / group['calls']
        for field, metric in group['metrics'].items():
            metric['per_call'] = None if field == 'max_us' else metric['total'] / metric['calls']
    warnings = ['Counts are not baseline-subtracted. Means use summed counts and measured calls.',
                'Inclusive regions can overlap; sums are not CPU utilization.',
                'Missing PMU measurements are unavailable, not zero-cost work.']
    if 'pmu_calls' not in mapping:
        warnings.append('PMU call coverage was not recorded; present counts assume all calls were measured.')
    if counterMode == 'cumulative':
        warnings += ['The ending row labels each interval. One wrap at most is assumed; unmarked resets cannot be detected.',
                     'Thread, session, bank or core changes establish separate snapshot baselines.']
    return {'kind': 'trace', 'status': 'complete', 'counter_mode': counterMode,
            'counter_bits': counterBits if counterMode == 'cumulative' else None,
            'columns': mapping, 'events': definitions, 'comments': comments,
            'samples': samples, 'regions': list(groups.values()), 'warnings': warnings}


def formatTrace(report, sortMetric, selectedBank, safeLabel, formatNumber):
    rows = [row for row in report['regions'] if selectedBank is None or str(selectedBank) == row['bank']]
    field = 'cycles' if sortMetric == 'cycles_per_call' else 'wall_us'
    if field == 'cycles' and not any('cycles' in row['metrics'] for row in rows):
        field = 'wall_us'

    def cost(row):
        return row['metrics'].get(field, {}).get('per_call')

    rows.sort(key=lambda row: cost(row) if cost(row) is not None else -1, reverse=True)
    maximum = max((cost(row) or 0 for row in rows), default=0)
    lines = [f"  Region measurements · {report['counter_mode']} · {len(report['samples']):,} records",
             f"  Ranking: {field} per measured call · event details are counts per measured call", '',
             f"  {'Region / thread / bank':38} {'Relative cost':14} {'Cycles/call':>12} {'Wall µs/call':>12} {'PMU':>7}"]
    for row in rows:
        name = row['region'] + ''.join(' / ' + row[key] for key in ('thread', 'bank') if row[key] is not None)
        bar = '█' * round(14 * (cost(row) or 0) / maximum) if maximum else '·'
        cycles = row['metrics'].get('cycles', {}).get('per_call')
        wall = row['metrics'].get('wall_us', {}).get('per_call')
        lines.append(f"  {safeLabel(name, 38):38} {bar:14} {formatNumber(cycles):>12} "
                     f"{formatNumber(wall):>12} {row['pmu_coverage_pct']:>6.1f}%")
        identities = [f"{key}={row[key]}" for key in ('process', 'session') if row[key] is not None]
        if row['cores']:
            identities.append('cores=' + ','.join(row['cores']))
        if identities:
            lines.append('    ' + safeLabel(' · '.join(identities), 160))
        details = [f"{header} {formatNumber(metric['per_call'] if header != 'max_us' else metric['total'])}"
                   for header, metric in row['metrics'].items() if header not in ('cycles', 'wall_us')]
        if details:
            lines.append('    ' + ' · '.join(safeLabel(detail, 120) for detail in details))
    return lines
