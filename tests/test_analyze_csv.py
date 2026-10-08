"""Portable measurement semantics and command-line compatibility."""

import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from pmu_csv import readTrace


class RegionCsvTests(unittest.TestCase):
    def testWeightedMeansAndCoverage(self):
        report = readTrace('# libperf.csv,version=1\n'
                           'region,calls,pmu_calls,wall_us,cycles,event.loads.0x06\n'
                           'work,10,8,100,800,80\nwork,2,2,40,400,40\n')
        row = report['regions'][0]
        self.assertEqual(row['metrics']['cycles']['per_call'], 120)
        self.assertEqual(row['metrics']['wall_us']['per_call'], 140 / 12)
        self.assertEqual(row['pmu_coverage_pct'], 100 * 10 / 12)
        self.assertEqual(report['events']['event.loads.0x06']['code'], 6)

    def testUnavailableAndMeasuredZero(self):
        report = readTrace('region,calls,pmu_calls,cycles,wall_us\n'
                           'missing,3,0,0,100\nzero,3,3,0,100\n')
        missing, zero = report['regions']
        self.assertNotIn('cycles', missing['metrics'])
        self.assertEqual(zero['metrics']['cycles']['per_call'], 0)

    def testMappingsAndUnknownEvents(self):
        report = readTrace('operation;elapsed_us;misses;notes\nwork;12;7;hello\n',
                           ['region=operation', 'wall_us=elapsed_us'], ['misses'])
        self.assertNotIn('timestamp_us', report['columns'])
        self.assertEqual(report['regions'][0]['metrics']['misses']['total'], 7)
        self.assertIsNone(report['events']['misses']['code'])
        self.assertEqual(report['samples'][0]['extra'], {'notes': 'hello'})

    def testExclusiveMaxAndThreadBanks(self):
        report = readTrace('label,thread,bank,calls,wall_us,exclusive_us,max_us,cycles,exclusive_cycles\n'
                           'work,a,0,2,20,10,12,100,40\nwork,a,0,1,15,8,15,70,20\n'
                           'work,b,0,1,4,4,4,30,30\nwork,a,1,1,8,8,8,40,40\n')
        self.assertEqual(len(report['regions']), 3)
        metrics = report['regions'][0]['metrics']
        self.assertEqual(metrics['max_us']['total'], 15)
        self.assertIsNone(metrics['max_us']['per_call'])
        self.assertEqual(metrics['exclusive_cycles']['per_call'], 20)

    def testMissingCellsUseMeasuredDenominator(self):
        report = readTrace('region,calls,cycles,event.unknown\nwork,10,,\nwork,2,40,0\n')
        self.assertEqual(report['regions'][0]['metrics']['cycles']['per_call'], 20)
        self.assertEqual(report['regions'][0]['metrics']['event.unknown']['per_call'], 0)

    def testCumulativeWrapInterleavedThreadsReset(self):
        report = readTrace('region,thread,timestamp_us,cycles,reset\n'
                           'seed,a,100,4294967200,0\nseed,b,100,10,0\n'
                           'work,a,200,104,0\nwork,b,200,30,0\n'
                           'seed,a,300,0,1\nwork,a,400,40,0\n', counterMode='cumulative')
        self.assertEqual([row['metrics']['cycles']['total'] for row in report['regions']], [240, 20])
        self.assertEqual(report['samples'][2]['snapshots']['cycles'], 104)
        self.assertTrue(report['samples'][4]['baseline'])

    def testQuotedMultilineLabelsAndLargeCounts(self):
        report = readTrace('region,cycles\n"line\n# text",9007199254740993\n')
        self.assertEqual(report['samples'][0]['region'], 'line\n# text')
        self.assertEqual(report['regions'][0]['metrics']['cycles']['total'], 9007199254740993)

    def testInvalidInputs(self):
        cases = ['region,cycles,cycles\na,1,2\n',
                 'region,cycles\na,-1\n', 'region,cycles\na,NaN\n',
                 'region,cycles\na,1,2\n', 'region,name,cycles\na,b,1\n',
                 'region,calls,pmu_calls,cycles\na,1,2,1\n',
                 'region,calls,pmu_calls,cycles\na,1,0,1\n',
                 '# libperf.csv,version=2\nregion,cycles\na,1\n',
                 'region,cycles,reset\na,1,2\n']
        for text in cases:
            with self.subTest(text=text), self.assertRaises(ValueError):
                readTrace(text)
        with self.assertRaises(ValueError):
            readTrace('region,calls,timestamp_us,cycles\na,2,10,20\n', counterMode='cumulative')

    def testCliJsonAndStaticReport(self):
        text = '\ufeffregion,calls,pmu_calls,cycles,wall_us,event.rename.0x68\nwork,2,2,100,20,40\n'
        command = [sys.executable, str(ROOT / 'tools/analyze.py'), '-']
        result = subprocess.run(command + ['--json'], input=text, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)['captures'][0]['runs'][0]
        self.assertEqual(report['kind'], 'trace')
        result = subprocess.run(command, input=text, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('work', result.stdout)
        self.assertIn('50.0', result.stdout)
        self.assertNotIn('\x1b', result.stdout)


if __name__ == '__main__':
    unittest.main()
