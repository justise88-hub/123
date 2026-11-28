import json
import sqlite3
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app'))
from backup_recovery import create_full_backup, inspect_full_backup, restore_full_backup, prune_full_backups, audit_backup_directory, rehearse_backup_restore


class BackupRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.data = self.root / 'data'
        self.templates = self.root / 'templates'
        self.data.mkdir(); self.templates.mkdir()
        (self.data / 'Employees').mkdir()
        (self.data / 'Config').mkdir()
        (self.data / 'History').mkdir()
        self.db = self.data / 'Employees' / 'employees.sqlite3'
        self.photo = self.root / 'external_photo.jpg'
        self.photo.write_bytes(b'jpeg-sample-payload')
        con = sqlite3.connect(self.db)
        con.executescript('''CREATE TABLE employees(id INTEGER PRIMARY KEY, full_name TEXT, photo_path TEXT);
            CREATE TABLE audit_log(id INTEGER PRIMARY KEY, event_type TEXT);''')
        con.execute('INSERT INTO employees VALUES(1, ?, ?)', ('Иванов Иван', str(self.photo)))
        con.commit(); con.close()
        (self.data / 'Config' / 'settings.json').write_text('{"theme":"dark"}', encoding='utf-8')
        (self.data / 'History' / 'history.json').write_text('[]', encoding='utf-8')
        (self.templates / 'sample.card').write_text('{"template":true}', encoding='utf-8')

    def tearDown(self):
        self.tmp.cleanup()

    def test_full_backup_contains_db_settings_photo_and_template(self):
        archive = self.root / 'full.pspmbackup'
        result = create_full_backup(self.db, self.data, self.templates, archive)
        self.assertEqual(result['photos'], 1)
        checked = inspect_full_backup(archive)
        self.assertEqual(checked['files'], 5)
        self.assertEqual(checked['photos'], 1)

    def test_restore_remaps_photo_and_restores_settings(self):
        archive = self.root / 'full.pspmbackup'
        create_full_backup(self.db, self.data, self.templates, archive)
        # Damage current DB and settings to prove restore actually replaces them.
        con = sqlite3.connect(self.db); con.execute("UPDATE employees SET full_name='Changed'"); con.commit(); con.close()
        (self.data / 'Config' / 'settings.json').write_text('{}', encoding='utf-8')
        result = restore_full_backup(archive, self.db, self.data, self.templates)
        con = sqlite3.connect(self.db)
        name, photo = con.execute('SELECT full_name,photo_path FROM employees WHERE id=1').fetchone()
        con.close()
        self.assertEqual(name, 'Иванов Иван')
        self.assertTrue(Path(photo).is_file())
        self.assertEqual(Path(photo).parent, self.data / 'Employees' / 'Photos')
        self.assertEqual(json.loads((self.data / 'Config' / 'settings.json').read_text()), {'theme':'dark'})
        self.assertEqual(result['photos_restored'], 1)

    def test_encrypted_full_backup_roundtrip(self):
        archive = self.root / 'full.pspmenc'
        create_full_backup(self.db, self.data, self.templates, archive, passphrase='long test passphrase')
        self.assertTrue(inspect_full_backup(archive, passphrase='long test passphrase')['encrypted'])
        with self.assertRaises(Exception):
            inspect_full_backup(archive, passphrase='wrong password')

    def test_tampered_payload_is_rejected(self):
        archive = self.root / 'full.pspmbackup'
        create_full_backup(self.db, self.data, self.templates, archive)
        changed = self.root / 'tampered.pspmbackup'
        with zipfile.ZipFile(archive) as src, zipfile.ZipFile(changed, 'w') as dst:
            for item in src.infolist():
                data = src.read(item.filename)
                if item.filename == 'Config/settings.json': data += b' ' 
                dst.writestr(item.filename, data)
        with self.assertRaisesRegex(ValueError, 'Контрольная сумма'):
            inspect_full_backup(changed)

    def test_backup_inventory_checks_all_archives(self):
        folder = self.root / 'backups'; folder.mkdir()
        good = folder / 'PSPassMaster_full_good.pspmbackup'
        create_full_backup(self.db, self.data, self.templates, good)
        (folder / 'PSPassMaster_full_broken.pspmbackup').write_bytes(b'not a zip')
        report = audit_backup_directory(folder)
        self.assertEqual(report['total'], 2)
        self.assertEqual(report['valid'], 1)
        self.assertEqual(report['invalid'], 1)

    def test_backup_inventory_marks_encrypted_archive_as_password_required(self):
        folder = self.root / 'backups'; folder.mkdir()
        encrypted = folder / 'PSPassMaster_full_locked.pspmenc'
        create_full_backup(self.db, self.data, self.templates, encrypted, passphrase='long test passphrase')
        report = audit_backup_directory(folder)
        self.assertEqual(report['password_required'], 1)
        self.assertEqual(report['invalid'], 0)

    def test_restore_rehearsal_does_not_modify_live_database(self):
        archive = self.root / 'full.pspmbackup'
        create_full_backup(self.db, self.data, self.templates, archive)
        before = self.db.read_bytes()
        result = rehearse_backup_restore(archive)
        self.assertTrue(result['ok'])
        self.assertEqual(result['employees'], 1)
        self.assertEqual(result['photos_verified'], 1)
        self.assertEqual(self.db.read_bytes(), before)

    def test_invalid_photo_reference_is_rejected(self):
        archive = self.root / 'full.pspmbackup'
        create_full_backup(self.db, self.data, self.templates, archive)
        changed = self.root / 'bad_photo_map.pspmbackup'
        with zipfile.ZipFile(archive) as src:
            manifest = json.loads(src.read('backup_manifest.json'))
            manifest['photo_map']['1']['archive_path'] = '../outside.jpg'
            with zipfile.ZipFile(changed, 'w', compression=zipfile.ZIP_DEFLATED) as dst:
                dst.writestr('backup_manifest.json', json.dumps(manifest))
                for name in src.namelist():
                    if name != 'backup_manifest.json': dst.writestr(name, src.read(name))
        with self.assertRaises(ValueError):
            inspect_full_backup(changed)

    def test_prune_keeps_requested_number(self):
        folder = self.root / 'backups'; folder.mkdir()
        for i in range(4): (folder / f'PSPassMaster_full_2026010{i}_000000.pspmbackup').write_bytes(b'x')
        removed = prune_full_backups(folder, keep=2)
        self.assertEqual(len(removed), 2)
        self.assertEqual(len(list(folder.glob('*.pspmbackup'))), 2)

    def test_restore_when_templates_are_inside_data_root(self):
        local_templates = self.data / 'Templates'
        local_templates.mkdir()
        (local_templates / 'local.card').write_text('local-template', encoding='utf-8')
        archive = self.root / 'portable.pspmbackup'
        create_full_backup(self.db, self.data, local_templates, archive)
        result = restore_full_backup(archive, self.db, self.data, local_templates)
        self.assertTrue((local_templates / 'local.card').is_file())
        self.assertEqual(result['photos_restored'], 1)

    def test_archive_path_traversal_is_rejected(self):
        archive = self.root / 'evil.pspmbackup'
        with zipfile.ZipFile(archive, 'w') as zf:
            zf.writestr('../evil.txt', 'x')
            zf.writestr('backup_manifest.json', json.dumps({'format':'PSPM-FULL-BACKUP','format_version':1,'files':[{'path':'../evil.txt','size':1,'sha256':'x'}],'database':'database/employees.sqlite3'}))
        with self.assertRaises(ValueError):
            inspect_full_backup(archive)
        from backup_recovery import _safe_archive_name
        for unsafe in (r"..\outside.txt", "C:/outside.txt", "folder\\..\\outside.txt"):
            with self.assertRaises(ValueError):
                _safe_archive_name(unsafe)


if __name__ == '__main__':
    unittest.main()
