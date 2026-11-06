"""Integration tests. Python is only used for tests, never by the converter."""
import datetime
import pathlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest

EXE = str(pathlib.Path(sys.argv.pop(1)).resolve())
IMAGE = str(pathlib.Path(sys.argv.pop(1)).resolve())


def crc(data):
    # CRC-16/ARC, check value for b"123456789" = 0xBB3D.
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
    return value


def seal(data):
    data[20:22] = b"\0\0"
    struct.pack_into("<H", data, 20, crc(data))
    return data


def index(used=(), next_slot=0):
    data = bytearray(48032)
    struct.pack_into("<8s8H", data, 0, b"0TIP00_1", 3000, 1,
                     next_slot // 100, next_slot % 100, 0, 0, 0, 24)
    for entry, slot, extra in used:
        struct.pack_into("<I8sI", data, 24 + entry * 16, 123456,
                         b"preserve", 1 | ((slot // 100) << 1) |
                         ((slot % 100) << 11) | extra)
    return seal(data)


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.pit = self.root / "pit.bin"
        self.out = self.root / "output"

    def run_converter(self, *args, success=True):
        result = subprocess.run([EXE, *map(str, args)], cwd=self.root,
                                capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def import_images(self, *images, success=True):
        return self.run_converter("--pit", self.pit, "-o", self.out,
                                  *(images or (IMAGE,)), success=success)

    def check_index(self):
        data = self.pit.read_bytes()
        copy = bytearray(data)
        copy[20:22] = b"\0\0"
        self.assertEqual(struct.unpack_from("<H", data, 20)[0], crc(copy))
        return data

    def photos(self):
        return sorted(self.out.rglob("*.JPG"))

    def test_crc_reference(self):
        self.assertEqual(crc(b"123456789"), 0xBB3D)

    def test_single_without_pit(self):
        self.run_converter(IMAGE)
        self.assertTrue((self.root / "HNI_0001.JPG").exists())
        self.assertFalse(self.pit.exists())

    def test_legacy_output_and_explicit_replacement(self):
        target = self.root / "custom.JPG"
        self.run_converter(IMAGE, target)
        self.run_converter("--output", target, IMAGE)
        self.run_converter("--verify", target)

    def test_batch_without_pit_skips_existing(self):
        self.out.mkdir()
        sentinel = self.out / "HNI_0001.JPG"
        sentinel.write_bytes(b"original")
        self.run_converter("-o", self.out, IMAGE, IMAGE)
        self.assertEqual(sentinel.read_bytes(), b"original")
        self.assertEqual(len(self.photos()), 3)
        for photo in self.photos()[1:]:
            self.run_converter("--verify", photo)
        self.assertFalse(self.pit.exists())

    def test_two_existing_inputs_default_to_batch(self):
        other = self.root / "other.png"
        other.write_bytes(pathlib.Path(IMAGE).read_bytes())
        self.run_converter(IMAGE, other)
        self.assertTrue((self.root / "HNI_0001.JPG").exists())
        self.assertTrue((self.root / "HNI_0002.JPG").exists())

    def test_import_preserves_entries_and_backup(self):
        original = index([(0, 0, 1 << 18), (2, 8, 3 << 18)])
        self.pit.write_bytes(original)
        self.import_images(IMAGE, IMAGE)
        data = self.check_index()
        self.assertEqual(data[24:40], original[24:40])
        self.assertEqual(data[56:72], original[56:72])
        self.assertEqual((self.root / "pit.bin.bak").read_bytes(), original)
        self.assertEqual([p.name for p in self.photos()], ["HNI_0002.JPG", "HNI_0003.JPG"])
        self.assertEqual(struct.unpack_from("<I", data, 24 + 16 + 12)[0], 1 | (1 << 11))
        timestamp = struct.unpack_from("<I", data, 40)[0]
        calendar_date = datetime.datetime(2000, 1, 1) + datetime.timedelta(seconds=timestamp)
        self.assertLess(abs((datetime.datetime.now() - calendar_date).total_seconds()), 10)
        for photo in self.photos():
            self.run_converter("--verify", photo)
        self.import_images()
        self.assertEqual((self.root / "pit.bin.bak.1").read_bytes(), data)
        self.check_index()
        self.assertEqual(len(self.photos()), 3)

    def test_folder_rollover(self):
        self.pit.write_bytes(index(next_slot=99))
        self.import_images(IMAGE, IMAGE)
        self.assertTrue((self.out / "DCIM/100NIN02/HNI_0100.JPG").exists())
        self.assertTrue((self.out / "DCIM/101NIN02/HNI_0001.JPG").exists())
        data = self.check_index()
        self.assertEqual(struct.unpack_from("<HH", data, 12), (1, 1))
        self.assertEqual(struct.unpack_from("<I", data, 52)[0], 3)

    def test_disk_collision(self):
        self.pit.write_bytes(index())
        directory = self.out / "DCIM/100NIN02"
        directory.mkdir(parents=True)
        original = directory / "HNI_0001.JPG"
        original.write_bytes(b"keep")
        self.import_images()
        self.assertEqual(original.read_bytes(), b"keep")
        self.assertTrue((directory / "HNI_0002.JPG").exists())
        self.check_index()

    def test_frames_do_not_reserve_photo_names(self):
        original = index([(0, 0, 1 << 20)])
        self.pit.write_bytes(original)
        self.import_images()
        data = self.check_index()
        self.assertEqual(data[24:40], original[24:40])
        self.assertEqual(self.photos()[0].name, "HNI_0001.JPG")

    def test_full_index(self):
        original = index([(i, i, 0) for i in range(3000)])
        self.pit.write_bytes(original)
        self.import_images(success=False)
        self.assertEqual(self.pit.read_bytes(), original)
        self.assertFalse(self.out.exists())

    def test_invalid_indices(self):
        malformed = [b"", b"Memory Pit", bytes(48032)]
        bad_crc = index()
        bad_crc[100] ^= 1
        malformed.append(bad_crc)
        bad_slot = index()
        struct.pack_into("<I", bad_slot, 36, 1 | (127 << 11))
        malformed.append(seal(bad_slot))
        for data in malformed:
            with self.subTest(size=len(data)):
                self.pit.write_bytes(data)
                self.import_images(success=False)
                self.assertEqual(self.pit.read_bytes(), data)
                self.assertFalse(self.out.exists())

    def test_rollback_after_bad_second_input(self):
        original = index()
        self.pit.write_bytes(original)
        bad = self.root / "bad.png"
        bad.write_bytes(b"not an image")
        self.import_images(IMAGE, bad, success=False)
        self.assertEqual(self.pit.read_bytes(), original)
        self.assertEqual(self.photos(), [])
        self.assertFalse((self.root / "pit.bin.bak").exists())

    def test_cannot_create_backup_rolls_back(self):
        original = index()
        self.pit.write_bytes(original)
        # Backup candidates all occupied: no original files may be removed.
        for i in range(10000):
            path = self.root / ("pit.bin.bak" if i == 0 else f"pit.bin.bak.{i}")
            path.touch()
        self.import_images(success=False)
        self.assertEqual(self.pit.read_bytes(), original)
        self.assertEqual(self.photos(), [])

    def test_positional_pit(self):
        self.pit.write_bytes(index())
        self.run_converter(IMAGE, IMAGE, self.pit, "-o", self.out)
        self.assertEqual(len(self.photos()), 2)
        self.check_index()

    def test_create_empty_index(self):
        self.run_converter("--create-pit", self.pit, "-o", self.out)
        data = self.check_index()
        self.assertEqual(len(data), 48032)
        self.assertEqual(data[:8], b"0TIP00_1")
        self.assertEqual(struct.unpack_from("<HH", data, 8), (3000, 1))
        self.assertEqual(struct.unpack_from("<H", data, 22)[0], 24)
        self.assertEqual(data[24:], bytes(48008))
        self.assertFalse((self.root / "pit.bin.bak").exists())
        # The newly created index must also support normal subsequent imports.
        self.import_images()
        self.check_index()
        self.assertEqual((self.root / "pit.bin.bak").read_bytes(), data)
        self.assertEqual(self.photos()[0].name, "HNI_0001.JPG")

    def test_missing_index_created_with_batch(self):
        self.import_images(IMAGE, IMAGE)
        data = self.check_index()
        self.assertEqual([p.name for p in self.photos()], ["HNI_0001.JPG", "HNI_0002.JPG"])
        self.assertEqual(struct.unpack_from("<HH", data, 12), (0, 2))
        self.assertEqual(struct.unpack_from("<I", data, 36)[0], 1)
        self.assertEqual(struct.unpack_from("<I", data, 52)[0], 1 | (1 << 11))
        self.assertFalse((self.root / "pit.bin.bak").exists())

    def test_create_with_images_and_positional_missing_index(self):
        self.run_converter("--create-pit", self.pit, "-o", self.out, IMAGE)
        self.check_index()
        other = self.root / "other.bin"
        self.run_converter(IMAGE, other, "-o", self.root / "other-output")
        self.assertTrue(other.exists())

    def test_create_nested_index_directories(self):
        self.pit = self.out / "private/ds/app/484E494A/pit.bin"
        self.import_images()
        self.check_index()
        self.assertTrue((self.out / "DCIM/100NIN02/HNI_0001.JPG").exists())

    def test_create_never_replaces_existing_index(self):
        for original in (bytes(index()), b"invalid index"):
            with self.subTest(valid=len(original) == 48032):
                self.pit.write_bytes(original)
                self.run_converter("--create-pit", self.pit, "-o", self.out, IMAGE, success=False)
                self.assertEqual(self.pit.read_bytes(), original)
                self.assertFalse(self.out.exists())

    def test_new_index_registers_existing_photos_and_frames(self):
        photo = self.out / "DCIM/101NIN02/HNI_0100.JPG"
        frame = self.out / "private/ds/app/484E494A/DCIM/100NIN02/HNI_0001.JPG"
        for path in (photo, frame):
            path.parent.mkdir(parents=True, exist_ok=True)
            self.run_converter("--output", path, IMAGE)
        original = photo.read_bytes()
        modified = datetime.datetime(2020, 2, 3, 4, 5, 6)
        os.utime(photo, (modified.timestamp(), modified.timestamp()))
        self.import_images()
        data = self.check_index()
        self.assertEqual(photo.read_bytes(), original)
        self.assertEqual(struct.unpack_from("<I", data, 36)[0], 1 | (1 << 1) | (99 << 11))
        self.assertEqual(struct.unpack_from("<I", data, 52)[0], 1 | (1 << 20))
        expected = int((modified - datetime.datetime(2000, 1, 1)).total_seconds())
        self.assertEqual(struct.unpack_from("<I", data, 24)[0], expected)
        self.assertTrue((self.out / "DCIM/102NIN02/HNI_0001.JPG").exists())
        self.assertEqual(struct.unpack_from("<HH", data, 12), (2, 1))
        self.assertEqual(struct.unpack_from("<HH", data, 16), (0, 1))
        self.assertEqual(len(self.photos()), 3)

    def test_new_index_frame_cursor_wraparound(self):
        for folder, name in (("100NIN02", "HNI_0001.JPG"), ("999NIN02", "HNI_0100.JPG")):
            frame = self.out / "private/ds/app/484E494A/DCIM" / folder / name
            frame.parent.mkdir(parents=True, exist_ok=True)
            self.run_converter("--output", frame, IMAGE)
        self.run_converter("--create-pit", self.pit, "-o", self.out)
        data = self.check_index()
        self.assertEqual(struct.unpack_from("<HH", data, 16), (0, 1))

    def test_new_index_rejects_invalid_existing_jpeg(self):
        photo = self.out / "DCIM/100NIN02/HNI_0001.JPG"
        photo.parent.mkdir(parents=True)
        photo.write_bytes(b"unsigned or damaged photo")
        self.import_images(success=False)
        self.assertFalse(self.pit.exists())
        self.assertEqual(photo.read_bytes(), b"unsigned or damaged photo")
        self.assertEqual(self.photos(), [photo])

    def test_new_index_rollback_keeps_existing_photos(self):
        photo = self.out / "DCIM/100NIN02/HNI_0001.JPG"
        photo.parent.mkdir(parents=True)
        self.run_converter("--output", photo, IMAGE)
        original = photo.read_bytes()
        bad = self.root / "bad.png"
        bad.write_bytes(b"not an image")
        self.import_images(IMAGE, bad, success=False)
        self.assertFalse(self.pit.exists())
        self.assertEqual(self.photos(), [photo])
        self.assertEqual(photo.read_bytes(), original)

    def test_new_index_capacity_includes_existing_files(self):
        original = self.root / "signed.JPG"
        self.run_converter("--output", original, IMAGE)
        for i in range(3000):
            path = self.out / f"DCIM/{100 + i // 100:03d}NIN02/HNI_{1 + i % 100:04d}.JPG"
            path.parent.mkdir(parents=True, exist_ok=True)
            os.link(original, path)
        self.import_images(success=False)
        self.assertFalse(self.pit.exists())
        self.assertEqual(len(self.photos()), 3000)
        # No new inputs: an exactly full album can still be indexed.
        self.run_converter("--create-pit", self.pit, "-o", self.out)
        self.check_index()
        extra = self.out / "DCIM/130NIN02/HNI_0001.JPG"
        extra.parent.mkdir(parents=True)
        os.link(original, extra)
        too_large = self.root / "too-large.bin"
        self.run_converter("--create-pit", too_large, "-o", self.out, success=False)
        self.assertFalse(too_large.exists())
        self.assertEqual(len(self.photos()), 3001)

    def test_missing_arguments(self):
        self.run_converter("--pit", success=False)
        self.run_converter("--pit", self.pit, success=False)
        self.run_converter("--unknown", IMAGE, success=False)


if __name__ == "__main__":
    unittest.main()
