// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_IO_MAT_FILE_H
#define TTTRLIB_IO_MAT_FILE_H
// Validation: A/B-TESTED 2026-10-06 -- every value read back equals scipy.io.loadmat (1.18.0) on the
//   repository's MATLAB/Octave fixtures and on generated files covering every class, both chars
//   codecs, cells, nested and empty structs, complex and logical data, compressed and plain;
//   files written here load in scipy unchanged. test/python/io/test_matfile_parity.py.
//   Register: okf/testing/algorithm-validation.md

/*!
 * \file MatFile.h
 * \brief MATLAB Level-5 MAT-files ("-v6"/"-v7", not the HDF5-based "-v7.3"), read and written.
 *
 * The file is a 128-byte header followed by one data element per variable,
 * each either a plain `miMATRIX` or an `miCOMPRESSED` zlib stream holding one.
 * An array is a tree: a cell holds arrays, a struct holds one array per
 * element and field. This class owns that tree as a flat node list -- the
 * shape every binding can walk without a recursive type crossing the
 * boundary -- and leaves the *meaning* of the bytes (dtype, char codec,
 * Fortran order, scipy's squeeze rules) to the language layer, which is where
 * a numpy array or a dict is made anyway. `tttrlib.matfile` is that layer for
 * Python and reads and writes like `scipy.io.loadmat` / `savemat`.
 *
 * Compression uses the inflate/deflate in this module (RFC 1950/1951), so the
 * module has no zlib dependency and builds unchanged for WebAssembly.
 *
 * Not supported, by design: sparse arrays, function handles and the opaque
 * MATLAB-object stream (classdef objects, strings, tables). A variable of one
 * of those classes is kept as an *unreadable* node carrying the reason, so
 * one exotic variable does not cost the file -- the same choice scipy makes.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace tttrlib {
namespace io {

//! MATLAB array classes (`mxCLASS`) as they appear in the array-flags word.
enum MatClass {
    MAT_CELL = 1, MAT_STRUCT = 2, MAT_OBJECT = 3, MAT_CHAR = 4, MAT_SPARSE = 5,
    MAT_DOUBLE = 6, MAT_SINGLE = 7, MAT_INT8 = 8, MAT_UINT8 = 9, MAT_INT16 = 10,
    MAT_UINT16 = 11, MAT_INT32 = 12, MAT_UINT32 = 13, MAT_INT64 = 14, MAT_UINT64 = 15,
    MAT_FUNCTION = 16, MAT_OPAQUE = 17,
    //! Not a MATLAB class: a variable this reader could not decode (see node_error).
    MAT_UNREADABLE = -1
};

//! Storage types of a data element (`miTYPE`).
enum MatDataType {
    MI_INT8 = 1, MI_UINT8 = 2, MI_INT16 = 3, MI_UINT16 = 4, MI_INT32 = 5, MI_UINT32 = 6,
    MI_SINGLE = 7, MI_DOUBLE = 9, MI_INT64 = 12, MI_UINT64 = 13, MI_MATRIX = 14,
    MI_COMPRESSED = 15, MI_UTF8 = 16, MI_UTF16 = 17, MI_UTF32 = 18
};

//! Bytes per element of an miTYPE, 0 for a type that is not a plain array.
int mat_data_type_size(int mi_type);

/*!
 * \brief A Level-5 MAT-file as a flat tree of arrays.
 *
 * Reading: construct from a path (or call read()), then walk
 * top_level() and node_children(). Writing: add_node() each array, attach
 * children in MATLAB's order (column-major elements; per struct element, the
 * fields in field order), mark the variables with add_top_level(), save().
 *
 * Numeric and char payloads are kept as the bytes of their *storage* type
 * (node_real_type()), in native byte order and column-major element order: a
 * MATLAB double that MATLAB chose to store as miUINT8 comes back as uint8
 * bytes, exactly as scipy's `mat_dtype=False` reports it.
 */
class MatFile {
public:
    MatFile() = default;

    //! Read \p filename. \throws std::runtime_error on a file that is not a
    //! Level-5 MAT-file or is truncated.
    explicit MatFile(const std::string& filename);

    //! Read \p filename, replacing the current contents.
    void read(const std::string& filename);

    //! Parse a whole file already in memory.
    void read_buffer(const unsigned char* in_data, int n_data);

    //! Write the file. Each variable becomes one miCOMPRESSED element when
    //! \p compress is set, a plain miMATRIX otherwise.
    void save(const std::string& filename, bool compress = false) const;

    //! The whole file as bytes, as save() would write it.
    void save_buffer(bool compress, unsigned char** out, int* n_out) const;

    //! The 116-byte descriptive text, trailing blanks and NULs removed.
    std::string header_text() const { return header_text_; }
    void set_header_text(const std::string& text) { header_text_ = text; }
    //! The header's version word (0x0100 for every Level-5 writer).
    int version() const { return version_; }
    //! True when the file was written big-endian ("MI").
    bool big_endian() const { return big_endian_; }

    //! Number of nodes, top-level and nested.
    int n_nodes() const { return static_cast<int>(nodes_.size()); }
    //! The variables, in file order.
    std::vector<int> top_level() const { return top_level_; }

    std::string node_name(int i) const;
    int node_class(int i) const;
    bool node_complex(int i) const;
    bool node_logical(int i) const;
    bool node_global(int i) const;
    std::vector<long long> node_dims(int i) const;
    int node_real_type(int i) const;
    int node_imag_type(int i) const;
    //! Number of struct/object fields.
    int node_n_fields(int i) const;
    //! Name of field \p k (fields are exposed one at a time so no binding needs a string-vector type).
    std::string node_field_name(int i, int k) const;
    std::string node_class_name(int i) const;
    std::vector<int> node_children(int i) const;
    //! Why an MAT_UNREADABLE node could not be read.
    std::string node_error(int i) const;
    //! Payload bytes of the real (or only) part.
    void node_real(int i, unsigned char** out, int* n_out) const;
    //! Payload bytes of the imaginary part (empty unless complex).
    void node_imag(int i, unsigned char** out, int* n_out) const;

    //! Append an array; returns its index. \p real_type / \p imag_type are
    //! miTYPEs (ignored for cells and structs).
    int add_node(const std::string& name, int mx_class, std::vector<long long> dims,
                 int real_type = 0, bool is_complex = false, bool is_logical = false,
                 int imag_type = 0, bool is_global = false);
    void set_node_real(int i, const unsigned char* in_data, int n_data);
    void set_node_imag(int i, const unsigned char* in_data, int n_data);
    void add_node_field_name(int i, const std::string& name);
    void set_node_class_name(int i, const std::string& name);
    void add_child(int parent, int child);
    void add_top_level(int i);

private:
    struct Node {
        std::string name;
        int mx_class = MAT_DOUBLE;
        bool is_complex = false, is_logical = false, is_global = false;
        std::vector<long long> dims;
        int real_type = 0, imag_type = 0;
        std::vector<unsigned char> real, imag;
        std::vector<std::string> field_names;
        std::string class_name;
        std::vector<int> children;
        std::string error;
    };

    const Node& node(int i) const;
    Node& node(int i);
    int parse_matrix(const unsigned char* p, std::size_t n, bool top);
    void write_matrix(std::vector<unsigned char>& out, int i, bool top) const;
    std::vector<unsigned char> serialise(bool compress) const;

    std::string header_text_;
    int version_ = 0x0100;
    bool big_endian_ = false;
    std::vector<Node> nodes_;
    std::vector<int> top_level_;
};

//! zlib (RFC 1950) decompression of \p n_data bytes. \throws std::runtime_error.
std::vector<unsigned char> zlib_inflate(const unsigned char* in_data, std::size_t n_data);
//! zlib (RFC 1950) compression: LZ77 over a 32 KiB window, fixed Huffman codes.
std::vector<unsigned char> zlib_deflate(const unsigned char* in_data, std::size_t n_data);

} // namespace io
} // namespace tttrlib

#endif // TTTRLIB_IO_MAT_FILE_H
