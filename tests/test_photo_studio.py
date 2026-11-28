import unittest
from pathlib import Path
from PIL import Image
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'app'))
from photo_studio import BACKGROUNDS, replace_background

class PhotoStudioTests(unittest.TestCase):
    def test_original_background_is_pixel_preserving(self):
        source = Image.new('RGB', (80, 120), (12, 34, 56))
        result = replace_background(source, 'Оригинал')
        self.assertEqual(result.size, source.size)
        self.assertEqual(result.getpixel((0, 0)), (12, 34, 56))

    def test_background_presets_are_neutral(self):
        self.assertEqual(BACKGROUNDS['Белый'], (255, 255, 255))
        self.assertEqual(BACKGROUNDS['Светло-серый'], (232, 235, 238))
        self.assertIsNone(BACKGROUNDS['Оригинал'])

if __name__ == '__main__':
    unittest.main()
