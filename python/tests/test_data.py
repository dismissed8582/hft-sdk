import io
import pathlib
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from hft_sdk import data


class DownloadTests(unittest.TestCase):
    def test_checksum_failure_is_not_silent(self):
        with patch.object(data.urllib.request, 'urlopen', side_effect=OSError('offline')):
            with self.assertRaises(RuntimeError):
                data._checksum('https://example.test/archive.zip')
        with patch.object(data.urllib.request, 'urlopen', return_value=io.BytesIO(b'bad')):
            with self.assertRaises(RuntimeError):
                data._checksum('https://example.test/archive.zip')

    def test_archive_cannot_choose_output_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            archive = directory / 'archive.zip'
            destination = directory / 'expected.csv'
            with zipfile.ZipFile(archive, 'w') as output:
                output.writestr('../unexpected.csv', 'bad')
            with self.assertRaises(RuntimeError):
                data._extract_csv(archive, destination)
            self.assertFalse(destination.exists())
            with zipfile.ZipFile(archive, 'w') as output:
                output.writestr('expected.csv', 'good\n')
                output.writestr('../unexpected.csv', 'bad')
            data._extract_csv(archive, destination)
            self.assertEqual(destination.read_text(), 'good\n')
            self.assertEqual(sorted(p.name for p in directory.iterdir()), ['archive.zip', 'expected.csv'])

    def test_path_components_are_validated_before_download(self):
        for symbol, period, exchange in [('..', '2026-01', 'binance'),
                                         ('BTCUSDT', '../../tmp', 'binance'),
                                         ('BTCUSDT', '2026-01', '../binance')]:
            with self.subTest(symbol=symbol, period=period, exchange=exchange), self.assertRaises(ValueError):
                data.fetch_crypto_lob(symbol, period, exchange=exchange)

    def test_truncated_download_is_not_cached(self):
        class Response(io.BytesIO):
            headers = {'Content-Length': '100'}
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / 'data.bin'
            with patch.object(data.urllib.request, 'urlopen', return_value=Response(b'abc')):
                with self.assertRaises(RuntimeError):
                    data._download('https://example.test/data.bin', target, force=False,
                                   timeout=1, expected_sha256=None, progress=False)
            self.assertFalse(target.exists())
            self.assertEqual(list(pathlib.Path(directory).iterdir()), [])
