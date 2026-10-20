"""
Unit tests for CLSMImage metadata extraction and PTU file support.

Tests the improved CLSMImage class with real PTU files from different
acquisition modes (confocal and STED).
"""

from __future__ import division

import os
import unittest
import json
import numpy as np
from pathlib import Path

import tttrlib

# Centralized test settings
from test_settings import settings, DATA_AVAILABLE  # type: ignore


@unittest.skipIf(not DATA_AVAILABLE, "Data directory not found, skipping CLSM metadata tests")
class TestCLSMImageMetadata(unittest.TestCase):
    """Test CLSMImage metadata extraction from PTU files."""
    
    @classmethod
    def setUpClass(cls):
        """Load test files."""
        # Use the same STED file for both tests (confocal and STED)
        # since we have the STED file available in V:\tttr-data
        cls.confocal_file = settings.get("clsm_confocal_filename")
        cls.sted_file = settings.get("clsm_sted_filename")
        
        # Check if files exist
        if not cls.confocal_file or not os.path.exists(cls.confocal_file):
            raise FileNotFoundError(f"Confocal test file not found: {cls.confocal_file}")
        if not cls.sted_file or not os.path.exists(cls.sted_file):
            raise FileNotFoundError(f"STED test file not found: {cls.sted_file}")
    
    def test_confocal_file_loading(self):
        """Test loading confocal PTU file."""
        d = tttrlib.TTTR(self.confocal_file)
        self.assertIsNotNone(d)
        self.assertGreater(len(d), 0)
    
    def test_confocal_metadata_extraction(self):
        """Test metadata extraction from confocal file."""
        d = tttrlib.TTTR(self.confocal_file)
        metadata = tttrlib.CLSMImage.get_metadata(d)
        
        # Check that metadata was extracted
        self.assertIsNotNone(metadata)
        self.assertGreater(len(metadata), 0)
        
        # Check key image parameters
        self.assertEqual(metadata['ImgHdr_PixX'], 500)
        self.assertEqual(metadata['ImgHdr_PixY'], 500)
        self.assertEqual(metadata['ImgHdr_TimePerPixel'], 0.01)
        self.assertFalse(metadata['ImgHdr_BiDirect'])
    
    def test_confocal_settings_extraction(self):
        """Test CLSM settings extraction from confocal file."""
        d = tttrlib.TTTR(self.confocal_file)
        settings = tttrlib.CLSMImage.read_clsm_settings(d)
        
        # Check that settings were extracted
        self.assertIsNotNone(settings)
        self.assertIn('marker_frame_start', settings)
        self.assertIn('marker_line_start', settings)
        self.assertIn('marker_line_stop', settings)
        self.assertIn('n_pixel_per_line', settings)
        self.assertIn('n_lines', settings)
        
        # Check marker values are correct (2^index conversion)
        # 2^2, and 2^2 | 2^1: a frame bit merged with the last line stop is a frame edge too
        self.assertEqual(settings['marker_frame_start'], [4, 6])
        self.assertEqual(settings['marker_line_start'], 1)     # 2^0
        self.assertEqual(settings['marker_line_stop'], 2)      # 2^1
        self.assertEqual(settings['n_pixel_per_line'], 500)
        self.assertEqual(settings['n_lines'], 500)
    
    def test_confocal_clsm_image_creation(self):
        """Test CLSMImage creation with auto-detected settings."""
        d = tttrlib.TTTR(self.confocal_file)
        img = tttrlib.CLSMImage(d)
        
        # Check shape
        self.assertEqual(img.n_frames, 1)
        self.assertEqual(img.n_lines, 500)
        self.assertEqual(img.n_pixel, 500)
    
    def test_confocal_clsm_image_metadata_storage(self):
        """Test that metadata is stored in CLSMImage instance."""
        d = tttrlib.TTTR(self.confocal_file)
        img = tttrlib.CLSMImage(d)
        
        # Check metadata is stored
        self.assertIsNotNone(img.metadata)
        self.assertGreater(len(img.metadata), 0)
        
        # Check specific metadata values
        self.assertEqual(img.metadata['ImgHdr_PixX'], 500)
        self.assertEqual(img.metadata['ImgHdr_PixY'], 500)
        self.assertIsNotNone(img.metadata['TTResult_NumberOfRecords'])
    
    def test_confocal_image_info(self):
        """Test formatted image information."""
        d = tttrlib.TTTR(self.confocal_file)
        img = tttrlib.CLSMImage(d)
        info = img.get_image_info()
        
        # Check formatted info
        self.assertIsNotNone(info)
        self.assertIn('dimensions', info)
        self.assertIn('pixel_time_ms', info)
        self.assertIn('bidirectional', info)
        self.assertIn('num_photons', info)
        
        # Check values
        self.assertEqual(info['dimensions'], (500, 500))
        self.assertEqual(info['pixel_time_ms'], 0.01)
        self.assertFalse(info['bidirectional'])
        self.assertGreater(info['num_photons'], 0)
    
    def test_confocal_repr_with_metadata(self):
        """Test extended representation with metadata."""
        d = tttrlib.TTTR(self.confocal_file)
        img = tttrlib.CLSMImage(d)
        repr_str = img.__repr_with_metadata__()
        
        # Check representation contains key info
        self.assertIn('CLSMImage', repr_str)
        self.assertIn('500x500', repr_str)
        self.assertIn('Dimensions', repr_str)
    
    def test_sted_file_loading(self):
        """Test loading STED PTU file."""
        d = tttrlib.TTTR(self.sted_file)
        self.assertIsNotNone(d)
        self.assertGreater(len(d), 0)
    
    def test_sted_metadata_extraction(self):
        """Test metadata extraction from STED file."""
        d = tttrlib.TTTR(self.sted_file)
        metadata = tttrlib.CLSMImage.get_metadata(d)
        
        # Check that metadata was extracted
        self.assertIsNotNone(metadata)
        self.assertGreater(len(metadata), 0)
        
        # Check key image parameters exist
        self.assertIsNotNone(metadata['ImgHdr_PixX'])
        self.assertIsNotNone(metadata['ImgHdr_PixY'])
    
    def test_sted_settings_extraction(self):
        """Test CLSM settings extraction from STED file."""
        d = tttrlib.TTTR(self.sted_file)
        settings = tttrlib.CLSMImage.read_clsm_settings(d)
        
        # Check that settings were extracted
        self.assertIsNotNone(settings)
        self.assertIn('marker_frame_start', settings)
        self.assertIn('marker_line_start', settings)
        self.assertIn('marker_line_stop', settings)
        self.assertIn('n_pixel_per_line', settings)
        self.assertIn('n_lines', settings)
        
        # Check marker values are positive
        self.assertGreater(settings['marker_frame_start'][0], 0)
        self.assertGreater(settings['marker_line_start'], 0)
        self.assertGreater(settings['marker_line_stop'], 0)
        self.assertGreater(settings['n_pixel_per_line'], 0)
        self.assertGreater(settings['n_lines'], 0)
    
    def test_sted_clsm_image_creation(self):
        """Test CLSMImage creation from STED file."""
        d = tttrlib.TTTR(self.sted_file)
        img = tttrlib.CLSMImage(d)
        
        # Check shape is valid
        self.assertGreater(img.n_frames, 0)
        self.assertGreater(img.n_lines, 0)
        self.assertGreater(img.n_pixel, 0)
    
    def test_sted_clsm_image_metadata_storage(self):
        """Test that metadata is stored in STED CLSMImage."""
        d = tttrlib.TTTR(self.sted_file)
        img = tttrlib.CLSMImage(d)
        
        # Check metadata is stored
        self.assertIsNotNone(img.metadata)
        self.assertGreater(len(img.metadata), 0)
    
    def test_sted_image_info(self):
        """Test formatted image information for STED."""
        d = tttrlib.TTTR(self.sted_file)
        img = tttrlib.CLSMImage(d)
        info = img.get_image_info()
        
        # Check formatted info
        self.assertIsNotNone(info)
        if 'dimensions' in info:
            self.assertIsInstance(info['dimensions'], tuple)
            self.assertEqual(len(info['dimensions']), 2)
    
    def test_metadata_safe_access(self):
        """Test safe metadata access with defaults."""
        d = tttrlib.TTTR(self.confocal_file)
        img = tttrlib.CLSMImage(d)
        
        # Safe access with defaults
        width = img.metadata.get('ImgHdr_PixX', 'Unknown')
        height = img.metadata.get('ImgHdr_PixY', 'Unknown')
        bidirectional = img.metadata.get('ImgHdr_BiDirect', False)
        
        self.assertEqual(width, 500)
        self.assertEqual(height, 500)
        self.assertFalse(bidirectional)
    
    def test_metadata_none_handling(self):
        """Test that missing metadata returns None gracefully."""
        d = tttrlib.TTTR(self.confocal_file)
        metadata = tttrlib.CLSMImage.get_metadata(d)
        
        # Check that None values are returned for missing tags
        # (not exceptions)
        for key, value in metadata.items():
            # Value can be None or a valid value, but not an exception
            self.assertNotIsInstance(value, Exception)
    
    def test_confocal_fill_operation(self):
        """Test fill operation on confocal image."""
        d = tttrlib.TTTR(self.confocal_file)
        img = tttrlib.CLSMImage(d)
        
        # Fill the image
        img.fill()
        
        # Check that intensity array was created
        self.assertIsNotNone(img.intensity)
        self.assertEqual(img.intensity.shape, (img.n_frames, img.n_lines, img.n_pixel))
    
    def test_sted_fill_operation(self):
        """Test fill operation on STED image."""
        d = tttrlib.TTTR(self.sted_file)
        img = tttrlib.CLSMImage(d)
        
        # Fill the image
        img.fill()
        
        # Check that intensity array was created
        self.assertIsNotNone(img.intensity)
        self.assertEqual(img.intensity.shape, (img.n_frames, img.n_lines, img.n_pixel))
    
    def test_metadata_consistency(self):
        """Test that metadata is consistent across multiple accesses."""
        d = tttrlib.TTTR(self.confocal_file)
        img1 = tttrlib.CLSMImage(d)
        
        # Load same file again
        d2 = tttrlib.TTTR(self.confocal_file)
        img2 = tttrlib.CLSMImage(d2)
        
        # Metadata should be the same
        self.assertEqual(img1.metadata['ImgHdr_PixX'], img2.metadata['ImgHdr_PixX'])
        self.assertEqual(img1.metadata['ImgHdr_PixY'], img2.metadata['ImgHdr_PixY'])
        self.assertEqual(img1.metadata['TTResult_NumberOfRecords'], img2.metadata['TTResult_NumberOfRecords'])


@unittest.skipIf(not DATA_AVAILABLE, "Data directory not found, skipping CLSM backward compatibility tests")
class TestCLSMImageBackwardCompatibility(unittest.TestCase):
    """Test backward compatibility of CLSMImage improvements."""
    
    @classmethod
    def setUpClass(cls):
        """Load test file."""
        cls.confocal_file = settings.get("clsm_confocal_filename")
    
    def test_explicit_settings_override(self):
        """Test that explicit settings override auto-detected ones."""
        d = tttrlib.TTTR(self.confocal_file)
        
        # Create with explicit settings
        img = tttrlib.CLSMImage(
            d,
            marker_frame_start=[4],
            marker_line_start=1,
            marker_line_stop=2,
            n_pixel_per_line=500
        )
        
        # Should still work
        self.assertEqual(img.n_pixel, 500)
    
    def test_backward_compatible_creation(self):
        """Test that old-style CLSMImage creation still works."""
        d = tttrlib.TTTR(self.confocal_file)
        
        # Old style: just pass TTTR object
        img = tttrlib.CLSMImage(d)
        
        # Should work and have correct shape
        self.assertGreater(img.n_frames, 0)
        self.assertGreater(img.n_lines, 0)
        self.assertGreater(img.n_pixel, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)


class TestReadClsmSettingsIsTheHeaderResolution(unittest.TestCase):
    """``CLSMImage.read_clsm_settings`` reports what ``CLSMImageInfo.from_header``
    resolves -- the settings the constructor builds with -- for every sample file.

    It used to decode the header a second time in Python and drifted: after the
    C++ reader learnt that a PTU frame bit merged with the last line stop (Leica
    SP5: 4 | 2 = 6) is a frame edge, the Python helper still said ``[4]``, and a
    caller that built with those settings got a 3 x 3316 x 256 SP5 image instead
    of 230 x 256 x 256.
    """

    def test_settings_equal_from_header_for_every_sample(self):
        files = [settings.get(k) for k in ("clsm_sp5_filename", "clsm_confocal_filename",
                                           "clsm_sted_filename", "ht3_clsm_filename")]
        checked = 0
        for path in files:
            if not path or not os.path.exists(path):
                continue
            tttr = tttrlib.TTTR(path)
            info = tttrlib.CLSMImageInfo.from_header(tttr.header)
            got = tttrlib.CLSMImage.read_clsm_settings(tttr)
            self.assertEqual(got["marker_frame_start"], [int(m) for m in info.marker_frame_start], path)
            self.assertEqual(got["marker_line_start"], info.marker_line_start, path)
            self.assertEqual(got["marker_line_stop"], info.marker_line_stop, path)
            self.assertEqual((got["n_pixel_per_line"], got["n_lines"]), (info.n_pixel, info.n_lines), path)
            # and building with the reported settings is building from the header
            built = tttrlib.CLSMImage(tttr_data=tttr, **{
                k: got[k] for k in ("marker_frame_start", "marker_line_start", "marker_line_stop",
                                    "marker_event_type", "n_pixel_per_line")})
            header = tttrlib.CLSMImage(tttr_data=tttr)
            self.assertEqual((built.n_frames, built.n_lines, built.n_pixel),
                             (header.n_frames, header.n_lines, header.n_pixel), path)
            checked += 1
        if not checked:
            self.skipTest("no CLSM sample files available")
