// SPDX-License-Identifier: BSD-3-Clause
//
// MATLAB Level-5 MAT-file parsing and serialisation. Layout reference:
// "MATLAB 7 MAT-File Format" (The MathWorks, 2017), chapter 1. The reader
// follows scipy.io.matlab._mio5 in every place the specification leaves room
// (unpadded top-level elements, a zero-byte miMATRIX as an empty array, the
// small-data element form anywhere), so it reads what scipy reads.

#include "MatFile.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace tttrlib {
namespace io {

int mat_data_type_size(int t) {
    switch (t) {
        case MI_INT8: case MI_UINT8: case MI_UTF8: return 1;
        case MI_INT16: case MI_UINT16: case MI_UTF16: return 2;
        case MI_INT32: case MI_UINT32: case MI_SINGLE: case MI_UTF32: return 4;
        case MI_DOUBLE: case MI_INT64: case MI_UINT64: return 8;
        default: return 0;
    }
}

namespace {

struct Element {
    int type = 0;
    std::size_t n = 0;              // payload bytes
    const unsigned char* data = nullptr;
    std::size_t next = 0;           // offset of the element after this one
};

std::uint32_t rd32(const unsigned char* p, bool swap) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    if (swap) v = (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
    return v;
}

// One data element at p[off], in either the full (8-byte tag, padded payload)
// or the small form (byte count in the upper half of the first word, up to
// four payload bytes in the second).
Element read_element(const unsigned char* p, std::size_t n, std::size_t off, bool swap, bool pad = true) {
    if (off + 8 > n) throw std::runtime_error("MAT-file: element tag past the end of its container");
    Element e;
    const std::uint32_t w0 = rd32(p + off, swap);
    if (w0 >> 16) {
        e.type = static_cast<int>(w0 & 0xffffu);
        e.n = w0 >> 16;
        if (e.n > 4) throw std::runtime_error("MAT-file: small data element longer than 4 bytes");
        e.data = p + off + 4;
        e.next = off + 8;
        return e;
    }
    e.type = static_cast<int>(w0);
    e.n = rd32(p + off + 4, swap);
    if (off + 8 + e.n > n) throw std::runtime_error("MAT-file: element payload past the end of its container");
    e.data = p + off + 8;
    std::size_t len = e.n;
    if (pad && (len % 8)) len += 8 - len % 8;
    e.next = std::min(off + 8 + len, n);
    return e;
}

void swap_bytes(std::vector<unsigned char>& v, int width) {
    if (width <= 1) return;
    for (std::size_t i = 0; i + width <= v.size(); i += width) std::reverse(v.begin() + i, v.begin() + i + width);
}

std::vector<unsigned char> payload(const Element& e, bool swap) {
    std::vector<unsigned char> v(e.data, e.data + e.n);
    if (swap) swap_bytes(v, mat_data_type_size(e.type));
    return v;
}

std::vector<long long> as_integers(const Element& e, bool swap) {
    const int w = mat_data_type_size(e.type);
    if (w == 0 || e.type == MI_SINGLE || e.type == MI_DOUBLE)
        throw std::runtime_error("MAT-file: expected an integer element");
    std::vector<unsigned char> b = payload(e, swap);
    std::vector<long long> out;
    for (std::size_t i = 0; i + w <= b.size(); i += w) {
        switch (e.type) {
            case MI_INT8: out.push_back(static_cast<std::int8_t>(b[i])); break;
            case MI_UINT8: case MI_UTF8: out.push_back(b[i]); break;
            case MI_INT16: { std::int16_t v; std::memcpy(&v, &b[i], 2); out.push_back(v); break; }
            case MI_UINT16: case MI_UTF16: { std::uint16_t v; std::memcpy(&v, &b[i], 2); out.push_back(v); break; }
            case MI_INT32: { std::int32_t v; std::memcpy(&v, &b[i], 4); out.push_back(v); break; }
            case MI_UINT32: case MI_UTF32: { std::uint32_t v; std::memcpy(&v, &b[i], 4); out.push_back(v); break; }
            case MI_INT64: { std::int64_t v; std::memcpy(&v, &b[i], 8); out.push_back(v); break; }
            case MI_UINT64: { std::uint64_t v; std::memcpy(&v, &b[i], 8); out.push_back(static_cast<long long>(v)); break; }
            default: break;
        }
    }
    return out;
}

std::string as_text(const Element& e) {
    std::string s(reinterpret_cast<const char*>(e.data), e.n);
    const auto z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    return s;
}

long long n_elements(const std::vector<long long>& dims) {
    long long k = 1;
    for (long long d : dims) k *= d;
    return k;
}

// -------------------------------------------------------------- serialisation

void put32(std::vector<unsigned char>& out, std::uint32_t v) {
    unsigned char b[4];
    std::memcpy(b, &v, 4);
    out.insert(out.end(), b, b + 4);
}

void put_element(std::vector<unsigned char>& out, int type, const unsigned char* data, std::size_t n) {
    if (n <= 4) {
        put32(out, static_cast<std::uint32_t>((n << 16) | static_cast<unsigned>(type)));
        unsigned char b[4] = {0, 0, 0, 0};
        if (n) std::memcpy(b, data, n);
        out.insert(out.end(), b, b + 4);
        return;
    }
    put32(out, static_cast<std::uint32_t>(type));
    put32(out, static_cast<std::uint32_t>(n));
    out.insert(out.end(), data, data + n);
    while (out.size() % 8) out.push_back(0);
}

} // namespace

// --------------------------------------------------------------------- access

MatFile::MatFile(const std::string& filename) { read(filename); }

const MatFile::Node& MatFile::node(int i) const {
    if (i < 0 || i >= static_cast<int>(nodes_.size())) throw std::out_of_range("MatFile: no such node");
    return nodes_[i];
}
MatFile::Node& MatFile::node(int i) {
    if (i < 0 || i >= static_cast<int>(nodes_.size())) throw std::out_of_range("MatFile: no such node");
    return nodes_[i];
}

std::string MatFile::node_name(int i) const { return node(i).name; }
int MatFile::node_class(int i) const { return node(i).mx_class; }
bool MatFile::node_complex(int i) const { return node(i).is_complex; }
bool MatFile::node_logical(int i) const { return node(i).is_logical; }
bool MatFile::node_global(int i) const { return node(i).is_global; }
std::vector<long long> MatFile::node_dims(int i) const { return node(i).dims; }
int MatFile::node_real_type(int i) const { return node(i).real_type; }
int MatFile::node_imag_type(int i) const { return node(i).imag_type; }
int MatFile::node_n_fields(int i) const { return static_cast<int>(node(i).field_names.size()); }
std::string MatFile::node_field_name(int i, int k) const {
    const Node& nd = node(i);
    if (k < 0 || k >= static_cast<int>(nd.field_names.size())) throw std::out_of_range("MatFile: no such field");
    return nd.field_names[k];
}
std::string MatFile::node_class_name(int i) const { return node(i).class_name; }
std::vector<int> MatFile::node_children(int i) const { return node(i).children; }
std::string MatFile::node_error(int i) const { return node(i).error; }

static void copy_out(const std::vector<unsigned char>& v, unsigned char** out, int* n_out) {
    *n_out = static_cast<int>(v.size());
    *out = static_cast<unsigned char*>(std::malloc(v.empty() ? 1 : v.size()));
    if (!v.empty()) std::memcpy(*out, v.data(), v.size());
}
void MatFile::node_real(int i, unsigned char** out, int* n_out) const { copy_out(node(i).real, out, n_out); }
void MatFile::node_imag(int i, unsigned char** out, int* n_out) const { copy_out(node(i).imag, out, n_out); }

int MatFile::add_node(const std::string& name, int mx_class, std::vector<long long> dims, int real_type,
                      bool is_complex, bool is_logical, int imag_type, bool is_global) {
    Node nd;
    nd.name = name;
    nd.mx_class = mx_class;
    nd.dims = std::move(dims);
    nd.real_type = real_type;
    nd.is_complex = is_complex;
    nd.is_logical = is_logical;
    nd.imag_type = imag_type ? imag_type : real_type;
    nd.is_global = is_global;
    nodes_.push_back(std::move(nd));
    return static_cast<int>(nodes_.size()) - 1;
}
void MatFile::set_node_real(int i, const unsigned char* d, int n) { node(i).real.assign(d, d + n); }
void MatFile::set_node_imag(int i, const unsigned char* d, int n) { node(i).imag.assign(d, d + n); }
void MatFile::add_node_field_name(int i, const std::string& name) { node(i).field_names.push_back(name); }
void MatFile::set_node_class_name(int i, const std::string& name) { node(i).class_name = name; }
void MatFile::add_child(int parent, int child) {
    node(child);
    node(parent).children.push_back(child);
}
void MatFile::add_top_level(int i) {
    node(i);
    top_level_.push_back(i);
}

// -------------------------------------------------------------------- reading

void MatFile::read(const std::string& filename) {
    std::ifstream f(filename, std::ios::binary);
    if (!f) throw std::runtime_error("MAT-file: cannot open " + filename);
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    read_buffer(buf.data(), static_cast<int>(buf.size()));
}

void MatFile::read_buffer(const unsigned char* p, int n_int) {
    const std::size_t n = n_int < 0 ? 0 : static_cast<std::size_t>(n_int);
    nodes_.clear();
    top_level_.clear();
    if (n < 128) throw std::runtime_error("MAT-file: shorter than the 128-byte header");
    if (p[126] == 'I' && p[127] == 'M') big_endian_ = false;
    else if (p[126] == 'M' && p[127] == 'I') big_endian_ = true;
    else throw std::runtime_error("MAT-file: no Level-5 endian indicator (a v4 or HDF5 -v7.3 file?)");
    const bool swap = big_endian_;  // this reader assumes a little-endian host, as every target is
    std::uint16_t ver;
    std::memcpy(&ver, p + 124, 2);
    if (swap) ver = static_cast<std::uint16_t>((ver >> 8) | (ver << 8));
    version_ = ver;
    header_text_.assign(reinterpret_cast<const char*>(p), 116);
    while (!header_text_.empty() && (header_text_.back() == ' ' || header_text_.back() == '\0' ||
                                     header_text_.back() == '\t' || header_text_.back() == '\n'))
        header_text_.pop_back();

    std::size_t off = 128;
    while (off + 8 <= n) {
        // Top-level elements are not padded (scipy seeks tag + byte_count).
        const Element e = read_element(p, n, off, swap, /*pad=*/false);
        off = e.next;
        std::vector<unsigned char> raw;
        const unsigned char* mp = e.data;
        std::size_t mn = e.n;
        if (e.type == MI_COMPRESSED) {
            raw = zlib_inflate(e.data, e.n);
            const Element inner = read_element(raw.data(), raw.size(), 0, swap, false);
            if (inner.type != MI_MATRIX) throw std::runtime_error("MAT-file: compressed element is not an miMATRIX");
            mp = inner.data;
            mn = inner.n;
        }
        if (e.type == MI_COMPRESSED || e.type == MI_MATRIX) {
            const std::size_t at = nodes_.size();
            try {
                top_level_.push_back(parse_matrix(mp, mn, true));
            } catch (const std::exception& ex) {
                // One variable this reader cannot decode does not cost the file:
                // keep it as an unreadable node carrying the reason (scipy warns
                // and stores "Read error: ..." the same way).
                nodes_.resize(at);
                Node bad;
                bad.mx_class = MAT_UNREADABLE;
                bad.error = ex.what();
                try {
                    const Element flags = read_element(mp, mn, 0, swap);
                    const Element dims = read_element(mp, mn, flags.next, swap);
                    bad.name = as_text(read_element(mp, mn, dims.next, swap));
                } catch (...) {
                }
                nodes_.push_back(bad);
                top_level_.push_back(static_cast<int>(nodes_.size()) - 1);
            }
        } else {
            throw std::runtime_error("MAT-file: expected miMATRIX or miCOMPRESSED at top level, got type " +
                                     std::to_string(e.type));
        }
    }
}

int MatFile::parse_matrix(const unsigned char* p, std::size_t n, bool top) {
    const bool swap = big_endian_;
    Node nd;
    if (n == 0) {
        // MATLAB writes an empty cell element as a zero-byte miMATRIX; scipy
        // reads it as np.array([[]]), a 1x0 double.
        nd.mx_class = MAT_DOUBLE;
        nd.dims = {1, 0};
        nd.real_type = MI_DOUBLE;
        nodes_.push_back(nd);
        return static_cast<int>(nodes_.size()) - 1;
    }
    const Element flags = read_element(p, n, 0, swap);
    if (flags.type != MI_UINT32 || flags.n < 4) throw std::runtime_error("MAT-file: bad array flags");
    const std::uint32_t fw = rd32(flags.data, swap);
    nd.mx_class = static_cast<int>(fw & 0xffu);
    const unsigned fb = (fw >> 8) & 0xffu;
    nd.is_complex = fb & 0x08;
    nd.is_global = fb & 0x04;
    nd.is_logical = fb & 0x02;

    std::size_t off = flags.next;
    if (nd.mx_class == MAT_FUNCTION || nd.mx_class == MAT_OPAQUE)
        throw std::runtime_error(nd.mx_class == MAT_FUNCTION ? "function handles are not supported"
                                                             : "opaque MATLAB objects are not supported");
    const Element dims = read_element(p, n, off, swap);
    nd.dims = as_integers(dims, swap);
    off = dims.next;
    const Element name = read_element(p, n, off, swap);
    nd.name = as_text(name);
    off = name.next;
    if (!top) nd.is_global = false;

    const int self = static_cast<int>(nodes_.size());
    nodes_.push_back(nd);

    switch (nd.mx_class) {
        case MAT_DOUBLE: case MAT_SINGLE: case MAT_INT8: case MAT_UINT8: case MAT_INT16:
        case MAT_UINT16: case MAT_INT32: case MAT_UINT32: case MAT_INT64: case MAT_UINT64: {
            const Element re = read_element(p, n, off, swap);
            nodes_[self].real_type = re.type;
            nodes_[self].real = payload(re, swap);
            off = re.next;
            if (nodes_[self].is_complex) {
                const Element im = read_element(p, n, off, swap);
                nodes_[self].imag_type = im.type;
                nodes_[self].imag = payload(im, swap);
            }
            break;
        }
        case MAT_CHAR: {
            const Element ch = read_element(p, n, off, swap);
            nodes_[self].real_type = ch.type;
            nodes_[self].real = payload(ch, swap);
            break;
        }
        case MAT_CELL: {
            const long long k = n_elements(nodes_[self].dims);
            for (long long j = 0; j < k; ++j) {
                const Element c = read_element(p, n, off, swap);
                if (c.type != MI_MATRIX) throw std::runtime_error("MAT-file: cell element is not an miMATRIX");
                off = c.next;
                const int child = parse_matrix(c.data, c.n, false);
                nodes_[self].children.push_back(child);
            }
            break;
        }
        case MAT_OBJECT:
        case MAT_STRUCT: {
            if (nodes_[self].mx_class == MAT_OBJECT) {
                const Element cn = read_element(p, n, off, swap);
                nodes_[self].class_name = as_text(cn);
                off = cn.next;
            }
            const Element fl = read_element(p, n, off, swap);
            const std::vector<long long> flv = as_integers(fl, swap);
            const std::size_t len = flv.empty() ? 0 : static_cast<std::size_t>(flv[0]);
            off = fl.next;
            const Element fn = read_element(p, n, off, swap);
            off = fn.next;
            std::vector<std::string> names;
            if (len > 0) {
                for (std::size_t j = 0; j + len <= fn.n; j += len) {
                    std::string s(reinterpret_cast<const char*>(fn.data + j), len);
                    const auto z = s.find('\0');
                    if (z != std::string::npos) s.resize(z);
                    names.push_back(s);
                }
            }
            nodes_[self].field_names = names;
            const long long k = n_elements(nodes_[self].dims);
            for (long long j = 0; j < k; ++j) {
                for (std::size_t f = 0; f < names.size(); ++f) {
                    const Element c = read_element(p, n, off, swap);
                    if (c.type != MI_MATRIX) throw std::runtime_error("MAT-file: struct field is not an miMATRIX");
                    off = c.next;
                    const int child = parse_matrix(c.data, c.n, false);
                    nodes_[self].children.push_back(child);
                }
            }
            break;
        }
        case MAT_SPARSE:
            throw std::runtime_error("sparse arrays are not supported");
        default:
            throw std::runtime_error("unknown MATLAB array class " + std::to_string(nodes_[self].mx_class));
    }
    return self;
}

// -------------------------------------------------------------------- writing

void MatFile::write_matrix(std::vector<unsigned char>& out, int i, bool top) const {
    const Node& nd = node(i);
    const std::size_t tag_at = out.size();
    put32(out, MI_MATRIX);
    put32(out, 0);  // byte count, patched below

    const std::uint32_t fb = (nd.is_complex ? 0x08u : 0u) | ((top && nd.is_global) ? 0x04u : 0u) |
                             (nd.is_logical ? 0x02u : 0u);
    const std::uint32_t af[2] = {static_cast<std::uint32_t>(nd.mx_class) | (fb << 8), 0u};
    put_element(out, MI_UINT32, reinterpret_cast<const unsigned char*>(af), 8);

    std::vector<std::int32_t> d32;
    for (long long d : nd.dims) d32.push_back(static_cast<std::int32_t>(d));
    put_element(out, MI_INT32, reinterpret_cast<const unsigned char*>(d32.data()), d32.size() * 4);
    put_element(out, MI_INT8, reinterpret_cast<const unsigned char*>(nd.name.data()), nd.name.size());

    switch (nd.mx_class) {
        case MAT_CHAR:
            put_element(out, nd.real_type ? nd.real_type : MI_UTF8, nd.real.data(), nd.real.size());
            break;
        case MAT_CELL:
            for (int c : nd.children) write_matrix(out, c, false);
            break;
        case MAT_OBJECT:
        case MAT_STRUCT: {
            if (nd.mx_class == MAT_OBJECT)
                put_element(out, MI_INT8, reinterpret_cast<const unsigned char*>(nd.class_name.data()),
                            nd.class_name.size());
            std::size_t len = 1;
            for (const auto& f : nd.field_names) len = std::max(len, f.size() + 1);
            const std::int32_t l32 = static_cast<std::int32_t>(len);
            put_element(out, MI_INT32, reinterpret_cast<const unsigned char*>(&l32), 4);
            std::vector<unsigned char> names(len * nd.field_names.size(), 0);
            for (std::size_t f = 0; f < nd.field_names.size(); ++f)
                std::memcpy(names.data() + f * len, nd.field_names[f].data(), nd.field_names[f].size());
            put_element(out, MI_INT8, names.data(), names.size());
            const std::size_t expect = static_cast<std::size_t>(n_elements(nd.dims)) * nd.field_names.size();
            if (nd.children.size() != expect)
                throw std::runtime_error("MatFile: struct '" + nd.name + "' needs one child per element and field");
            for (int c : nd.children) write_matrix(out, c, false);
            break;
        }
        case MAT_DOUBLE: case MAT_SINGLE: case MAT_INT8: case MAT_UINT8: case MAT_INT16:
        case MAT_UINT16: case MAT_INT32: case MAT_UINT32: case MAT_INT64: case MAT_UINT64:
            put_element(out, nd.real_type, nd.real.data(), nd.real.size());
            if (nd.is_complex) put_element(out, nd.imag_type, nd.imag.data(), nd.imag.size());
            break;
        default:
            throw std::runtime_error("MatFile: cannot write array class " + std::to_string(nd.mx_class));
    }
    const std::uint32_t bytes = static_cast<std::uint32_t>(out.size() - tag_at - 8);
    std::memcpy(out.data() + tag_at + 4, &bytes, 4);
}

std::vector<unsigned char> MatFile::serialise(bool compress) const {
    std::vector<unsigned char> out;
    std::string text = header_text_;
    if (text.empty()) {
        const std::time_t now = std::time(nullptr);
        char when[64] = {0};
        std::strftime(when, sizeof when, "%a %b %e %H:%M:%S %Y", std::localtime(&now));
        text = std::string("MATLAB 5.0 MAT-file Platform: tttrlib, Created on: ") + when;
    }
    text.resize(116, ' ');
    out.insert(out.end(), text.begin(), text.end());
    out.insert(out.end(), 8, 0);  // subsystem data offset
    out.push_back(0x00);
    out.push_back(0x01);  // version 0x0100, little-endian
    out.push_back('I');
    out.push_back('M');

    for (int i : top_level_) {
        if (!compress) {
            write_matrix(out, i, true);
            continue;
        }
        std::vector<unsigned char> var;
        write_matrix(var, i, true);
        const std::vector<unsigned char> z = zlib_deflate(var.data(), var.size());
        put32(out, MI_COMPRESSED);
        put32(out, static_cast<std::uint32_t>(z.size()));
        out.insert(out.end(), z.begin(), z.end());
    }
    return out;
}

void MatFile::save_buffer(bool compress, unsigned char** out, int* n_out) const {
    copy_out(serialise(compress), out, n_out);
}

void MatFile::save(const std::string& filename, bool compress) const {
    const std::vector<unsigned char> out = serialise(compress);
    std::ofstream f(filename, std::ios::binary);
    if (!f) throw std::runtime_error("MatFile: cannot write " + filename);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!f) throw std::runtime_error("MatFile: write failed for " + filename);
}

} // namespace io
} // namespace tttrlib
