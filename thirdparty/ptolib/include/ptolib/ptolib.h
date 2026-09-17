// SPDX-License-Identifier: MIT
/*!
 * \file ptolib.h
 * \brief ptolib: PTO (Portable Tagged Objects), the DataStore and its `.dstore`
 *        encoding, with separately compiled codec implementations.
 * \author Thomas-Otavio Peulen
 * \copyright MIT License, Thomas-Otavio Peulen
 * \version 0.3.2
 *
 * PTO is a self-contained, packed data container: an EBML document (DocType
 * `"pto"`) that binds opaque payloads into one file, gives each a UID that
 * survives rewriting, lets typed tags describe them and reference each other,
 * and updates in place with two checksummed generation indexes.
 * In-place edits are not transactions. The DataStore is its natural payload,
 * with a compiled SIMD expression engine for gating it.
 * https://github.com/tpeulen/ptolib
 *
 * Include this header and link the CMake target ptolib::ptolib. The core is
 * compiled as C++14; codec C sources are compiled separately. Codec provider
 * and decoder-only options belong to the build, not to public includes.
 *
 * The optional generated dist/ptolib.h supports PTOLIB_IMPLEMENTATION for
 * consumers that explicitly choose a single-header release artifact.
 */
#ifndef PTOLIB_H
#define PTOLIB_H

#define PTOLIB_VERSION_MAJOR 0
#define PTOLIB_VERSION_MINOR 3
#define PTOLIB_VERSION_PATCH 2
#define PTOLIB_VERSION_STRING "0.3.2"

#ifndef PTOLIB_API
#define PTOLIB_API
#endif
// `__restrict` on the pointers of an inline loop, for the compilers; nothing
// for SWIG, which parses this header for the bindings and does not know it.
#if defined(SWIG)
#define PTOLIB_RESTRICT
#else
#define PTOLIB_RESTRICT __restrict
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#ifndef SWIG
#include <cstdio>
#include <functional>
#endif

/*!
 * \namespace pto
 * \brief Everything ptolib declares: the DataStore, the `.dstore` encoding,
 *        the PTO container and the expression engine.
 */
namespace pto {

namespace detail {

/// Set bits in a word. The compiler's instruction where the target has one;
/// the parallel bit count otherwise, which is a dozen operations and no call.
inline int popcount64(std::uint64_t w) {
#if defined(__aarch64__) || defined(__POPCNT__) || (defined(__clang__) && !defined(_MSC_VER))
    return __builtin_popcountll(w);
#else
    w = w - ((w >> 1) & 0x5555555555555555ULL);
    w = (w & 0x3333333333333333ULL) + ((w >> 2) & 0x3333333333333333ULL);
    w = (w + (w >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<int>((w * 0x0101010101010101ULL) >> 56);
#endif
}

/// Eight bytes to eight bits: bit k set where byte k is not zero. Branch-free,
/// and eight rows per step rather than one.
inline std::uint64_t pack8(std::uint64_t x) {
    const std::uint64_t low7 = x & 0x7F7F7F7F7F7F7F7FULL;
    const std::uint64_t nz = ((low7 + 0x7F7F7F7F7F7F7F7FULL) | x) & 0x8080808080808080ULL;
    return (nz * 0x0002040810204081ULL) >> 56;
}

/// The inverse: eight bits to eight bytes of 0 or 1, byte k from bit k.
inline std::uint64_t spread8(unsigned bits) {
    const std::uint64_t x = (static_cast<std::uint64_t>(bits & 0xFFu) * 0x0101010101010101ULL) & 0x8040201008040201ULL;
    return ((x + 0x7F7F7F7F7F7F7F7FULL) >> 7) & 0x0101010101010101ULL;
}

}  // namespace detail

// ===========================================================================
// DataStore -- columnar tables
// ===========================================================================

/*!
 * \brief An allocator whose vectors do not zero what they are about to overwrite.
 *
 * `std::vector<T>::resize` value-initialises, so growing a buffer for a reader
 * that is about to write every element writes it twice: once with zeros and
 * once with the data. On a 28-million-event file that is 458 MB of pointless
 * stores, and it is exactly what the malloc this replaced did not do.
 *
 * Only reachable through \ref Column::resize_uninitialized, which says what it
 * does. \ref Column::resize still zeroes, because a general table should.
 */
template<typename T>
struct DefaultInitAllocator : std::allocator<T> {
    using std::allocator<T>::allocator;

    template<typename U>
    struct rebind { using other = DefaultInitAllocator<U>; };

    template<typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new(static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
    template<typename U>
    void construct(U* p) {
        ::new(static_cast<void*>(p)) U;      // default-init: scalars left alone
    }
};

template<typename T>
using RawVector = std::vector<T, DefaultInitAllocator<T>>;

/// What a column holds. String is stored but never binned -- see \ref Column.
enum class ColumnType {
    Float64,
    Float32,
    Int64,
    Int32,
    // The narrow integer types exist because the photon stream is made of them:
    // a routing channel is one byte and a micro time is two, and widening them
    // to int64 would quadruple the largest arrays in the library for nothing.
    Int16,
    Int8,
    UInt64,
    UInt32,
    UInt16,
    UInt8,
    Bool,
    String
};

/// Whether a column type can hold a NaN or an infinity at all.
inline bool is_floating(ColumnType t) {
    return t == ColumnType::Float64 || t == ColumnType::Float32;
}

/// The dtype's name, as the bindings spell it. For error messages that have to
/// say which two types would not combine.
inline const char* column_type_name(ColumnType t) {
    switch (t) {
        case ColumnType::Float64: return "float64";
        case ColumnType::Float32: return "float32";
        case ColumnType::Int64:   return "int64";
        case ColumnType::Int32:   return "int32";
        case ColumnType::Int16:   return "int16";
        case ColumnType::Int8:    return "int8";
        case ColumnType::UInt64:  return "uint64";
        case ColumnType::UInt32:  return "uint32";
        case ColumnType::UInt16:  return "uint16";
        case ColumnType::UInt8:   return "uint8";
        case ColumnType::Bool:    return "bool";
        case ColumnType::String:  return "str";
    }
    return "float64";
}

/// Bytes per element of a stored column, for reporting memory use.
inline int column_type_size(ColumnType t) {
    switch (t) {
        case ColumnType::Float64: return 8;
        case ColumnType::Float32: return 4;
        case ColumnType::Int64:   return 8;
        case ColumnType::Int32:   return 4;
        case ColumnType::Int16:   return 2;
        case ColumnType::Int8:    return 1;
        case ColumnType::UInt64:  return 8;
        case ColumnType::UInt32:  return 4;
        case ColumnType::UInt16:  return 2;
        case ColumnType::UInt8:   return 1;
        case ColumnType::Bool:    return 1;   // bit-packed; see Column::nbytes
        case ColumnType::String:  return 4;   // the code; the dictionary is extra
    }
    return 8;
}

/*!
 * \brief The msgpack encoding of a column description held as JSON text.
 *
 * Storage, not API: \ref Column::metadata keeps a text face because a string
 * crosses four bindings with no typemap and is what a human reads in a
 * debugger. This is what a *file* holds, for four reasons -- types survive
 * (JSON has one number type, so an integer row index comes back as a double),
 * binary values need no base64, the description is parsed on every open whether
 * or not anyone looks at it, and it is about a quarter smaller.
 *
 * Empty in, empty out: a column with no description costs one length prefix.
 *
 * \throws std::invalid_argument if the text does not parse, matching
 *         \ref Column::set_metadata rather than silently storing nothing.
 */
std::vector<unsigned char> metadata_to_msgpack(const std::string& json_text);

/*!
 * \brief JSON text from what \ref metadata_to_msgpack wrote.
 *
 * Returns ``""`` for a byte range that is not msgpack. A corrupt description is
 * not a reason to refuse the column -- the values are still exactly what was
 * measured, and losing the units is the smaller loss.
 */
std::string metadata_from_msgpack(const unsigned char* bytes, std::size_t n);

/*!
 * \brief A run of rows that were never measured, and why.
 *
 * The form a concat leaves behind: a column absent from one of twenty files is
 * missing for that file's whole contribution, which is one contiguous run, so a
 * bit per row would store a million copies of one fact. Half-open,
 * `[first, last)`.
 *
 * `why` is the part a bit cannot carry. "Not measured because that file did not
 * have this column" is information; a zero bit is the absence of it.
 *
 * At namespace scope rather than inside Column, where it belongs by meaning: a
 * nested class does not reach three of the four bindings, so it would have been
 * an opaque pointer everywhere but C++.
 */
struct NaRange {
    std::size_t first = 0;
    std::size_t last = 0;
    std::string why;
};

/*!
 * \brief A bit per row.
 *
 * One eighth the size of a byte array, which matters at ten million rows, and
 * read a word at a time when a fill is scanning it.
 */
// An out-of-range row index on a column accessor read past the allocation --
// silently at a small overrun, fatally at a large one. Out of line and
// noreturn so the guard costs a branch and nothing else in the caller.
[[noreturn]] void datastore_index_out_of_range(std::size_t index, std::size_t size);

class BitMask {
public:
    BitMask() = default;
    explicit BitMask(std::size_t n, bool value = true) { assign(n, value); }

    void assign(std::size_t n, bool value) {
        n_ = n;
        words_.assign((n + 63) / 64, value ? ~0ULL : 0ULL);
        if (value && n % 64) words_.back() = (1ULL << (n % 64)) - 1ULL;
    }
    void clear() { n_ = 0; words_.clear(); }
    bool empty() const { return n_ == 0; }
    std::size_t size() const { return n_; }

    // Bounded, unlike the raw word indexing these used to do. `test` past the
    // end was a SIGSEGV straight from a binding -- BitMask().test(100000000)
    // -- and `set` is the worse half, ORing a bit into whatever it landed on.
    //
    // `n_` is the right bound and there is no capacity/count split to get wrong
    // here, unlike an event reader's arrays: `words_` is only ever assigned
    // alongside `n_`, always at (n_ + 63) / 64, and never grown on its own.
    //
    // These are per-row on every mask and filter, so the throw stays out of
    // line and the guard is a predicted compare.
    inline bool test(std::size_t i) const {
        if (i >= n_) datastore_index_out_of_range(i, n_);
        return (words_[i >> 6] >> (i & 63)) & 1ULL;
    }
    inline void set(std::size_t i, bool v) {
        if (i >= n_) datastore_index_out_of_range(i, n_);
        const std::uint64_t bit = 1ULL << (i & 63);
        if (v) words_[i >> 6] |= bit; else words_[i >> 6] &= ~bit;
    }

    /// Number of set bits.
    std::size_t count() const {
        std::size_t c = 0;
        for (std::uint64_t w : words_) c += static_cast<std::size_t>(detail::popcount64(w));
        return c;
    }
    std::size_t nbytes() const { return words_.size() * sizeof(std::uint64_t); }

    /// Raw words, for building a mask 64 rows at a time rather than bit by bit.
    std::uint64_t* words() { return words_.data(); }
    const std::uint64_t* words() const { return words_.data(); }
    std::size_t n_words() const { return words_.size(); }

    /// The inverse of \ref words -- take a mask back from packed words, for a
    /// loader that read them out of a file rather than computing them. Keeps
    /// the packing an implementation detail on both sides.
    void assign_words(const std::uint64_t* w, std::size_t n_bits) {
        n_ = n_bits;
        words_.assign(w, w + (n_bits + 63) / 64);
        trim();
    }

    // --- set algebra ------------------------------------------------------
    //
    // A selection is built by combining conditions, and doing it on bits works
    // 64 rows at a time. The alternative -- a bool array per condition, ORed
    // with numpy -- moves 64x the memory and is what this exists to replace.

    void and_with(const BitMask& o) {
        const std::size_t n = std::min(words_.size(), o.words_.size());
        for (std::size_t i = 0; i < n; i++) words_[i] &= o.words_[i];
        for (std::size_t i = n; i < words_.size(); i++) words_[i] = 0;
    }
    void or_with(const BitMask& o) {
        const std::size_t n = std::min(words_.size(), o.words_.size());
        for (std::size_t i = 0; i < n; i++) words_[i] |= o.words_[i];
        trim();
    }
    /// Clear every bit that is set in `o`.
    void andnot_with(const BitMask& o) {
        const std::size_t n = std::min(words_.size(), o.words_.size());
        for (std::size_t i = 0; i < n; i++) words_[i] &= ~o.words_[i];
    }
    void invert() {
        for (std::uint64_t& w : words_) w = ~w;
        trim();
    }
    bool any() const {
        for (std::uint64_t w : words_) if (w != 0) return true;
        return false;
    }

    /*!
     * Set from a byte-per-row array, which is what numpy hands over.
     *
     * A word at a time. Setting one bit at a time is a read-modify-write of a
     * whole word per row, and this is on the interactive path -- every redraw
     * hands over a fresh mask of every row in the table.
     */
    void from_bytes(const unsigned char* b, std::size_t n) {
        assign(n, false);
        const std::size_t full = n / 64;
        for (std::size_t k = 0; k < full; k++) {
            const unsigned char* q = b + k * 64;
            std::uint64_t bits = 0;
            for (int j = 0; j < 8; j++) {
                std::uint64_t eight;
                std::memcpy(&eight, q + 8 * j, 8);
                bits |= detail::pack8(eight) << (8 * j);
            }
            words_[k] = bits;
        }
        std::uint64_t bits = 0;
        for (std::size_t i = full * 64; i < n; i++)
            bits |= static_cast<std::uint64_t>(b[i] != 0 ? 1 : 0) << (i - full * 64);
        if (full < words_.size()) words_[full] = bits;
    }
    /// Expand into a caller-provided byte-per-row array. Takes a length
    /// because a language binding cannot pass a bare pointer safely.
    void to_bytes(unsigned char* out_bytes, int n_out) const {
        const std::size_t n = std::min<std::size_t>(n_, static_cast<std::size_t>(n_out < 0 ? 0 : n_out));
        std::size_t i = 0;
        // Eight rows per step, straight from the word: no per-row bounds check
        // and no per-row read of the word.
        for (; i + 8 <= n; i += 8) {
            const std::uint64_t eight = detail::spread8(static_cast<unsigned>(words_[i >> 6] >> (i & 63)));
            std::memcpy(out_bytes + i, &eight, 8);
        }
        for (; i < n; i++) out_bytes[i] = (words_[i >> 6] >> (i & 63)) & 1ULL ? 1 : 0;
    }

private:
    /// Clear the bits past n_ in the last word, so count() and any() cannot see
    /// bits that are not rows.
    void trim() {
        if (n_ % 64 && !words_.empty()) words_.back() &= (1ULL << (n_ % 64)) - 1ULL;
    }

    std::size_t n_ = 0;
    std::vector<std::uint64_t> words_;
};

/*!
 * \brief One column.
 *
 * Exactly one of the typed vectors is populated, chosen by \ref type. Nothing
 * is promoted on the way in: a float32 column stays float32, because half the
 * memory is the point and the histogram fill is templated on the type anyway.
 *
 * \section col_string Strings are stored, not binned
 *
 * A text column is dictionary-encoded: the distinct values once, plus an int32
 * code per row. That is the compression, and it is also what makes such a
 * column usable -- the codes ARE a category axis, so "how many rows per label"
 * needs no separate pass. The strings themselves never reach a histogram.
 */
class Column {
public:
    Column() = default;
    Column(std::string name, ColumnType type) : name_(std::move(name)), type_(type) {}

    /*!
     * \brief The column's name, and the key it is looked up by.
     *
     * An attribute of \ref metadata rather than a field beside it, cached here
     * because lookup is the hot path and parsing JSON per lookup would not be.
     * Returned by reference: no allocation, no parse.
     */
    const std::string& name() const { return name_; }

    /// Set the name, and the ``name`` attribute of \ref metadata with it.
    void set_name(std::string s);

    /*!
     * \brief Everything known about the column, as a JSON object.
     *
     * A column carries one extensible description instead of a growing list of
     * members: units, the dictionary item it corresponds to, a longer label.
     * Adding one is another key rather than a change to this class, to the
     * `.dstore` directory record, to four bindings and to the format version.
     *
     * The store neither validates nor interprets what is in here. ``units`` is
     * conventional and is what this exists for -- a burst duration in
     * milliseconds and a lifetime in nanoseconds otherwise say so only in their
     * column names, when whoever wrote them remembered.
     *
     * Empty when the column has none; nothing fabricates an object.
     */
    const std::string& metadata() const { return metadata_; }

    /*!
     * \brief Replace the description.
     *
     * \param json a JSON **object**, or the empty string for none.
     * \throws std::invalid_argument if it does not parse, or is not an object.
     *         Rejecting here rather than storing it means the failure is at the
     *         call that got it wrong, not at some later read.
     */
    void set_metadata(const std::string& json);

    /// One attribute, or ``""`` when absent, so a caller never has to parse.
    std::string attribute(const std::string& key) const;

    /*!
     * \brief One attribute as JSON **text**, or ``""`` when absent.
     *
     * The difference from \ref attribute is quoting, and it is what makes the
     * round trip exact: a stored string comes back as ``"ns"`` with its quotes,
     * a stored array as ``[[2,4]]``. \ref attribute unquotes a string so a
     * caller reading ``units`` need not parse, which leaves it unable to say
     * whether ``[[2,4]]`` was an array or a string that looked like one.
     */
    std::string attribute_json(const std::string& key) const;

    /*!
     * \brief Set one attribute to a string value, creating the description if
     *        there was none.
     *
     * The value is stored as a JSON **string**, whatever it looks like:
     * ``set_attribute("na", "[[2,4]]")`` stores the seven characters, not an
     * array of ranges. Use \ref set_attribute_json for anything structured.
     */
    void set_attribute(const std::string& key, const std::string& value);

    /*!
     * \brief Set one attribute to a JSON value given as text.
     *
     * \param json_value a JSON value -- ``42``, ``[[2,4]]``, ``{"of":"run.ptu"}``,
     *        ``"ns"`` **with** its quotes for a string. The empty string erases
     *        the key, matching \ref set_attribute.
     * \throws std::invalid_argument if it does not parse.
     *
     * Text rather than a variant because a string crosses four bindings with no
     * typemap; the type is preserved from here on, and \ref metadata is stored
     * as msgpack, so an integer written as an integer stays one.
     */
    void set_attribute_json(const std::string& key, const std::string& json_value);

    /// The ``units`` attribute -- what this was built for. ``""`` when unsaid.
    const std::string& units() const { return units_; }

    /// Set the ``units`` attribute.
    void set_units(std::string s);

    ColumnType type() const { return type_; }
    std::size_t size() const { return n_; }
    bool is_numeric() const { return type_ != ColumnType::String; }

    /*!
     * Release any capacity beyond what is stored.
     *
     * Bulk-loading a column through push_string leaves a vector with up to
     * twice the capacity it needs, which on a hundred-million-row column is
     * hundreds of megabytes of nothing. nbytes() reports capacity rather than
     * size precisely so that this is visible.
     */
    void shrink_to_fit() {
        f64_.shrink_to_fit(); f32_.shrink_to_fit();
        i64_.shrink_to_fit(); i32_.shrink_to_fit();
        i16_.shrink_to_fit(); i8_.shrink_to_fit();
        u64_.shrink_to_fit(); u32_.shrink_to_fit();
        u16_.shrink_to_fit(); u8_.shrink_to_fit();
        codes_.shrink_to_fit(); dictionary_.shrink_to_fit();
    }

    /*!
     * \brief Bytes actually held -- CAPACITY, not size.
     *
     * The buffers: values, dictionary and mask. NOT the description, which is
     * how a column that records its gaps as ranges rather than as bits shows
     * that it allocated no mask -- and which is a description rather than a
     * buffer, so counting it would make "a float32 column is half a float64
     * one" false for a reason that has nothing to do with dtypes.
     */
    std::size_t nbytes() const {
        std::size_t b = 0;
        switch (type_) {
            case ColumnType::Float64: b = f64_.capacity() * 8; break;
            case ColumnType::Float32: b = f32_.capacity() * 4; break;
            case ColumnType::Int64:   b = i64_.capacity() * 8; break;
            case ColumnType::Int32:   b = i32_.capacity() * 4; break;
            case ColumnType::Int16:   b = i16_.capacity() * 2; break;
            case ColumnType::Int8:    b = i8_.capacity(); break;
            case ColumnType::UInt64:  b = u64_.capacity() * 8; break;
            case ColumnType::UInt32:  b = u32_.capacity() * 4; break;
            case ColumnType::UInt16:  b = u16_.capacity() * 2; break;
            case ColumnType::UInt8:   b = u8_.capacity(); break;
            case ColumnType::Bool:    b = bits_.nbytes(); break;
            case ColumnType::String:
                b = codes_.capacity() * 4;
                for (const std::string& s : dictionary_) b += s.capacity() + sizeof(std::string);
                break;
        }
        return b + mask_.nbytes();
    }

    // --- setting ----------------------------------------------------------

    // Plain int / long long / unsigned char rather than the <cstdint> spellings:
    // SWIG matches typemaps on the type as written, and (int* IN_ARRAY1, int DIM1)
    // does not apply to a parameter declared std::int32_t. The types are the same
    // where it matters and the binding is the point of these overloads.
    void set_f64(const double* v, int n) { f64_.assign(v, v + n); n_ = n; type_ = ColumnType::Float64; }
    void set_f32(const float* v, int n) { f32_.assign(v, v + n); n_ = n; type_ = ColumnType::Float32; }
    void set_i64(const long long* v, int n) { i64_.assign(v, v + n); n_ = n; type_ = ColumnType::Int64; }
    void set_i32(const int* v, int n) { i32_.assign(v, v + n); n_ = n; type_ = ColumnType::Int32; }
    void set_i16(const short* v, int n) { i16_.assign(v, v + n); n_ = n; type_ = ColumnType::Int16; }
    void set_i8(const signed char* v, int n) { i8_.assign(v, v + n); n_ = n; type_ = ColumnType::Int8; }
    void set_u64(const unsigned long long* v, int n) { u64_.assign(v, v + n); n_ = n; type_ = ColumnType::UInt64; }
    void set_u32(const unsigned int* v, int n) { u32_.assign(v, v + n); n_ = n; type_ = ColumnType::UInt32; }
    void set_u16(const unsigned short* v, int n) { u16_.assign(v, v + n); n_ = n; type_ = ColumnType::UInt16; }
    void set_u8(const unsigned char* v, int n) { u8_.assign(v, v + n); n_ = n; type_ = ColumnType::UInt8; }
    void set_bool(const unsigned char* v, int n) { bits_.from_bytes(v, n); n_ = n; type_ = ColumnType::Bool; }

    /*!
     * Append one string, dictionary-encoded.
     *
     * Repeated values cost four bytes, not the string. A column of ten million
     * rows drawn from twenty labels is 40 MB of codes plus a few hundred bytes,
     * rather than several hundred megabytes of std::string.
     */
    void push_string(const std::string& s) {
        type_ = ColumnType::String;
        auto it = lookup_.find(s);
        int code;
        if (it == lookup_.end()) {
            code = static_cast<int>(dictionary_.size());
            dictionary_.push_back(s);
            lookup_.emplace(s, code);
        } else {
            code = it->second;
        }
        codes_.push_back(code);
        n_ = codes_.size();
    }

    /*!
     * \brief Install a dictionary and codes wholesale.
     *
     * For a bulk loader that already knows the distinct values. push_string does
     * a hash lookup per row, which is right when values arrive one at a time and
     * catastrophic when a million of them arrive at once: merging per-block
     * dictionaries this way turned a million lookups into one per distinct
     * value.
     */
    void set_dictionary(const std::vector<std::string>& dict) {
        type_ = ColumnType::String;
        dictionary_ = dict;
        lookup_.clear();
        for (std::size_t i = 0; i < dictionary_.size(); i++)
            lookup_.emplace(dictionary_[i], static_cast<int>(i));
    }
    /// \see set_dictionary. Codes must index into it.
    void set_codes(const int* v, int n) {
        type_ = ColumnType::String;
        codes_.assign(v, v + n);
        n_ = codes_.size();
    }
    /// Room for `n` elements of the column's own type, for an appending loader.
    void reserve(std::size_t n) {
        switch (type_) {
            case ColumnType::Float64: f64_.reserve(n); break;
            case ColumnType::Float32: f32_.reserve(n); break;
            case ColumnType::Int64:   i64_.reserve(n); break;
            case ColumnType::Int32:   i32_.reserve(n); break;
            case ColumnType::Int16:   i16_.reserve(n); break;
            case ColumnType::Int8:    i8_.reserve(n); break;
            case ColumnType::UInt64:  u64_.reserve(n); break;
            case ColumnType::UInt32:  u32_.reserve(n); break;
            case ColumnType::UInt16:  u16_.reserve(n); break;
            case ColumnType::UInt8:   u8_.reserve(n); break;
            case ColumnType::String:  codes_.reserve(n); break;
            default: break;
        }
    }
    /*!
     * \brief Size the column and hand out its buffer.
     *
     * For a loader that knows where each of its pieces belongs and wants to
     * write them straight into place. That is the difference between a parser
     * that produces the column and one that produces something which is then
     * copied into the column -- at a hundred million rows the copy is the
     * dominant cost and the peak is twice what it needs to be.
     *
     * Bool and String have no raw buffer here on purpose: bool is bit-packed,
     * so two writers can collide inside one word, and a string needs its
     * dictionary. Both take a staging array and one pass at the end.
     */
    void resize(std::size_t n) {
        n_ = n;
        switch (type_) {
            case ColumnType::Float64: f64_.assign(n, 0.0); break;
            case ColumnType::Float32: f32_.assign(n, 0.0f); break;
            case ColumnType::Int64:   i64_.assign(n, 0); break;
            case ColumnType::Int32:   i32_.assign(n, 0); break;
            case ColumnType::Int16:   i16_.assign(n, 0); break;
            case ColumnType::Int8:    i8_.assign(n, 0); break;
            case ColumnType::UInt64:  u64_.assign(n, 0); break;
            case ColumnType::UInt32:  u32_.assign(n, 0); break;
            case ColumnType::UInt16:  u16_.assign(n, 0); break;
            case ColumnType::UInt8:   u8_.assign(n, 0); break;
            case ColumnType::String:  codes_.assign(n, 0); break;
            case ColumnType::Bool:    bits_.assign(n, false); break;
        }
    }
    /*!
     * \brief Size the column WITHOUT initialising it.
     *
     * For a reader that is about to write every element. resize() zeroes first,
     * which on a large file is a full extra pass over the memory; this is what
     * the malloc it replaced did. Anything not written is whatever was in the
     * page, so a caller that does not fill the column must use resize().
     */
    void resize_uninitialized(std::size_t n) {
        n_ = n;
        switch (type_) {
            case ColumnType::Float64: f64_.resize(n); break;
            case ColumnType::Float32: f32_.resize(n); break;
            case ColumnType::Int64:   i64_.resize(n); break;
            case ColumnType::Int32:   i32_.resize(n); break;
            case ColumnType::Int16:   i16_.resize(n); break;
            case ColumnType::Int8:    i8_.resize(n); break;
            case ColumnType::UInt64:  u64_.resize(n); break;
            case ColumnType::UInt32:  u32_.resize(n); break;
            case ColumnType::UInt16:  u16_.resize(n); break;
            case ColumnType::UInt8:   u8_.resize(n); break;
            case ColumnType::String:  codes_.resize(n); break;
            case ColumnType::Bool:    bits_.assign(n, false); break;
        }
    }

    /*!
     * Grow to `n`, KEEPING what is already there.
     *
     * resize() clears, which is what a loader filling a column wants; this is
     * what a container growing one wants. Two names because getting the wrong
     * one silently loses data rather than failing.
     */
    void grow(std::size_t n) {
        if (n < n_) return;
        n_ = n;
        switch (type_) {
            case ColumnType::Float64: f64_.resize(n, 0.0); break;
            case ColumnType::Float32: f32_.resize(n, 0.0f); break;
            case ColumnType::Int64:   i64_.resize(n, 0); break;
            case ColumnType::Int32:   i32_.resize(n, 0); break;
            case ColumnType::Int16:   i16_.resize(n, 0); break;
            case ColumnType::Int8:    i8_.resize(n, 0); break;
            case ColumnType::UInt64:  u64_.resize(n, 0); break;
            case ColumnType::UInt32:  u32_.resize(n, 0); break;
            case ColumnType::UInt16:  u16_.resize(n, 0); break;
            case ColumnType::UInt8:   u8_.resize(n, 0); break;
            case ColumnType::String:  codes_.resize(n, 0); break;
            case ColumnType::Bool:    { BitMask b(n, false);
                                        for (std::size_t i = 0; i < std::min(n, bits_.size()); i++)
                                            b.set(i, bits_.test(i));
                                        bits_ = b; break; }
        }
    }

    /*!
     * Keep the first `n` elements and drop the rest, without reallocating.
     *
     * For a reader that allocated for the record count in the file and then
     * found fewer valid events -- which is every photon-record reader, because invalid
     * and overflow records are not events.
     */
    void truncate(std::size_t n) {
        if (n > n_) return;
        n_ = n;
        // ONLY the active type. Resizing all of them looks harmless because the
        // others are empty -- and it is the opposite: resize() on an empty
        // vector GROWS it, so every column allocated and zeroed one array per
        // type it does not hold. It cost 60% of the test suite's runtime.
        switch (type_) {
            case ColumnType::Float64: f64_.resize(n); break;
            case ColumnType::Float32: f32_.resize(n); break;
            case ColumnType::Int64:   i64_.resize(n); break;
            case ColumnType::Int32:   i32_.resize(n); break;
            case ColumnType::Int16:   i16_.resize(n); break;
            case ColumnType::Int8:    i8_.resize(n); break;
            case ColumnType::UInt64:  u64_.resize(n); break;
            case ColumnType::UInt32:  u32_.resize(n); break;
            case ColumnType::UInt16:  u16_.resize(n); break;
            case ColumnType::UInt8:   u8_.resize(n); break;
            case ColumnType::String:  codes_.resize(n); break;
            case ColumnType::Bool:    break;   // bit-packed; n_ is the length
        }
    }
    double* f64_data() { return f64_.data(); }
    short* i16_data() { return i16_.data(); }
    signed char* i8_data() { return i8_.data(); }
    unsigned long long* u64_data() { return u64_.data(); }
    unsigned int* u32_data() { return u32_.data(); }
    unsigned short* u16_data() { return u16_.data(); }
    unsigned char* u8_data() { return u8_.data(); }
    float* f32_data() { return f32_.data(); }
    long long* i64_data() { return reinterpret_cast<long long*>(i64_.data()); }
    int* codes_data() { return reinterpret_cast<int*>(codes_.data()); }

    /// Append raw values of the column's own type, without an intermediate copy.
    void append_f64(const double* v, std::size_t n) { f64_.insert(f64_.end(), v, v + n); n_ = f64_.size(); }
    void append_f32(const float* v, std::size_t n) { f32_.insert(f32_.end(), v, v + n); n_ = f32_.size(); }
    void append_i64(const long long* v, std::size_t n) { i64_.insert(i64_.end(), v, v + n); n_ = i64_.size(); }
    void append_codes(const int* v, std::size_t n) { codes_.insert(codes_.end(), v, v + n); n_ = codes_.size(); }

    // --- combining and subsetting -----------------------------------------
    //
    // The three primitives DataStore::append_rows and DataStore::take are built
    // from. They live here because they need the typed vectors, and putting the
    // eleven-way switch in one place is the whole point: a twelfth column type
    // is one line in visit_pair rather than three loops to find.

    /*!
     * \brief Append `src`'s values to this column.
     *
     * \throws std::invalid_argument if the types differ. Promoting silently --
     *         a float32 column meeting a float64 one -- would lose the dtype
     *         this whole class exists to keep, and quietly.
     */
    void append_from(const Column& src) {
        if (src.type_ != type_)
            throw std::invalid_argument(
                    "cannot append a " + std::string(column_type_name(src.type_)) +
                    " column to a " + column_type_name(type_) + " one: '" + name_ + "'");
        const std::size_t before = n_, add = src.n_;
        if (type_ == ColumnType::String) {
            for (std::size_t i = 0; i < add; i++) push_string(src.string_at(i));
        } else if (type_ == ColumnType::Bool) {
            BitMask b = bits_;
            b.assign(before + add, false);
            for (std::size_t i = 0; i < before; i++) b.set(i, bits_.test(i));
            for (std::size_t i = 0; i < add; i++) b.set(before + i, src.bits_.test(i));
            bits_ = b;
            n_ = before + add;
        } else {
            visit_pair(*this, src, [&](auto& dst, const auto& s) {
                dst.insert(dst.end(), s.begin(), s.end());
            });
            n_ = before + add;
        }
        append_mask_from(src, before, add);
    }

    /*!
     * \brief Extend by `n` rows that were never measured.
     *
     * The values are zeroed and the column says so, which is what lets a column
     * absent from one store keep its dtype through a concat instead of being
     * widened to hold a NaN.
     *
     * Recorded as one range, not as `n` zero bits: the rows are contiguous by
     * construction -- they are one store's whole contribution -- so a bit per
     * row would be a million copies of one fact. \param why travels with it,
     * which is the part a bit cannot carry. \see add_na_range, which decides
     * between the two representations.
     */
    void append_missing(std::size_t n, const std::string& why = std::string()) {
        const std::size_t before = n_;
        if (type_ == ColumnType::String) {
            for (std::size_t i = 0; i < n; i++) push_string(std::string());
        } else if (type_ == ColumnType::Bool) {
            BitMask b;
            b.assign(before + n, false);
            for (std::size_t i = 0; i < before; i++) b.set(i, bits_.test(i));
            bits_ = b;
            n_ = before + n;
        } else {
            visit_pair(*this, *this, [&](auto& dst, const auto&) {
                dst.resize(before + n);
            });
            n_ = before + n;
        }
        add_na_range(before, before + n, why);
    }

    /// Gather rows `rows[0..n)` of `src` into this column, which is emptied first.
    void take_from(const Column& src, const int* rows, std::size_t n) {
        set_metadata(src.metadata_);
        name_ = src.name_;
        units_ = src.units_;
        type_ = src.type_;
        if (type_ == ColumnType::String) {
            dictionary_.clear(); lookup_.clear(); codes_.clear(); n_ = 0;
            for (std::size_t k = 0; k < n; k++) push_string(src.string_at(rows[k]));
        } else if (type_ == ColumnType::Bool) {
            bits_.assign(n, false);
            for (std::size_t k = 0; k < n; k++) bits_.set(k, src.bits_.test(rows[k]));
            n_ = n;
        } else {
            visit_pair(*this, src, [&](auto& dst, const auto& s) {
                dst.clear();
                dst.resize(n);
                for (std::size_t k = 0; k < n; k++) dst[k] = s[rows[k]];
            });
            n_ = n;
        }
        // A gather reorders rows, so a range describing the SOURCE's rows says
        // nothing true about these. The validity is kept and the ranges are not:
        // the reason a row is missing survives only while the rows it names still
        // mean what they meant.
        if (!na_.empty()) { set_attribute_json("na", std::string()); }
        if (!src.has_missing()) {
            mask_.clear();
        } else {
            mask_.assign(n, true);
            for (std::size_t k = 0; k < n; k++)
                mask_.set(k, src.valid(static_cast<std::size_t>(rows[k])));
        }
    }

    // --- reading ----------------------------------------------------------

    const std::vector<std::string>& dictionary() const { return dictionary_; }
    const RawVector<std::int32_t>& codes() const { return codes_; }

    const std::string& string_at(std::size_t i) const {
        static const std::string empty;
        if (type_ != ColumnType::String || i >= codes_.size()) return empty;
        return dictionary_[codes_[i]];
    }

    /*!
     * \brief The value of row `i` as a double, whatever the column holds.
     *
     * For a string column this is the dictionary CODE, not the text -- which is
     * the only numeric thing a string has, and is what a category axis wants.
     */
    inline double value_at(std::size_t i) const {
        // Bounded, unlike the raw indexing this used to do. An out-of-range row
        // read past the allocation: at a small overrun it returned 0.0 -- a
        // perfectly plausible measurement -- and far out it was a SIGSEGV
        // (BUGS 2026-08-11). `string_at` a dozen lines above has always
        // checked; this is the same class, the same shape, and did not. The
        // throw is out of line so the per-row callers (masking, comparisons,
        // the histogram fill callbacks) keep only a predicted branch.
        if (i >= n_) datastore_index_out_of_range(i, n_);
        switch (type_) {
            case ColumnType::Float64: return f64_[i];
            case ColumnType::Float32: return static_cast<double>(f32_[i]);
            case ColumnType::Int64:   return static_cast<double>(i64_[i]);
            case ColumnType::Int32:   return static_cast<double>(i32_[i]);
            case ColumnType::Int16:   return static_cast<double>(i16_[i]);
            case ColumnType::Int8:    return static_cast<double>(i8_[i]);
            // Above 2^53 a uint64 does not survive a double. Macro times reach
            // that on long acquisitions, so a caller that needs them exactly
            // must take the typed view rather than value_at.
            case ColumnType::UInt64:  return static_cast<double>(u64_[i]);
            case ColumnType::UInt32:  return static_cast<double>(u32_[i]);
            case ColumnType::UInt16:  return static_cast<double>(u16_[i]);
            case ColumnType::UInt8:   return static_cast<double>(u8_[i]);
            case ColumnType::Bool:    return bits_.test(i) ? 1.0 : 0.0;
            case ColumnType::String:  return static_cast<double>(codes_[i]);
        }
        return 0.0;
    }

    /*!
     * \brief Rows `[first, first + n)` widened to double, into `out`.
     *
     * One switch on the type per call rather than one per row, which is what
     * makes a two-column scan run at the speed of the memory rather than of
     * the dispatch: \ref value_at is the right call for one value and the
     * wrong one for a million. For a String column the dictionary codes; for a
     * Bool column 0 or 1. Rows past the end are not written; the return value
     * says how many were.
     */
    std::size_t copy_f64(std::size_t first, std::size_t n, double* out) const {
        if (first >= n_) return 0;
        const std::size_t take = std::min(n, n_ - first);
        switch (type_) {
            case ColumnType::Float64: std::memcpy(out, f64_.data() + first, take * sizeof(double)); break;
            case ColumnType::Float32: widen(f32_.data() + first, take, out); break;
            case ColumnType::Int64:   widen(i64_.data() + first, take, out); break;
            case ColumnType::Int32:   widen(i32_.data() + first, take, out); break;
            case ColumnType::Int16:   widen(i16_.data() + first, take, out); break;
            case ColumnType::Int8:    widen(i8_.data() + first, take, out); break;
            case ColumnType::UInt64:  widen(u64_.data() + first, take, out); break;
            case ColumnType::UInt32:  widen(u32_.data() + first, take, out); break;
            case ColumnType::UInt16:  widen(u16_.data() + first, take, out); break;
            case ColumnType::UInt8:   widen(u8_.data() + first, take, out); break;
            case ColumnType::String:  widen(codes_.data() + first, take, out); break;
            case ColumnType::Bool:
                for (std::size_t i = 0; i < take; i++) out[i] = bits_.test(first + i) ? 1.0 : 0.0;
                break;
        }
        return take;
    }
    // Raw typed pointers, for a scan that wants the column in its own type
    // rather than one double at a time through a switch.
    const double* f64_ptr() const { return f64_.data(); }
    const float* f32_ptr() const { return f32_.data(); }
    const std::int64_t* i64_ptr() const { return i64_.data(); }
    const std::int32_t* i32_ptr() const { return i32_.data(); }
    const short* i16_ptr() const { return i16_.data(); }
    const signed char* i8_ptr() const { return i8_.data(); }
    const unsigned long long* u64_ptr() const { return u64_.data(); }
    const unsigned int* u32_ptr() const { return u32_.data(); }
    const unsigned short* u16_ptr() const { return u16_.data(); }
    const unsigned char* u8_ptr() const { return u8_.data(); }
    const std::int32_t* codes_ptr() const { return codes_.data(); }

    /// Zero-copy views, valid while the column is alive and unmodified.
    void get_f64_view(double** view, int* n) {
        *view = f64_.empty() ? nullptr : f64_.data(); *n = static_cast<int>(f64_.size());
    }
    void get_f32_view(float** view, int* n) {
        *view = f32_.empty() ? nullptr : f32_.data(); *n = static_cast<int>(f32_.size());
    }
    void get_i64_view(long long** view, int* n) {
        *view = i64_.empty() ? nullptr : reinterpret_cast<long long*>(i64_.data());
        *n = static_cast<int>(i64_.size());
    }
    void get_i32_view(int** view, int* n) {
        *view = i32_.empty() ? nullptr : reinterpret_cast<int*>(i32_.data());
        *n = static_cast<int>(i32_.size());
    }
    void get_i16_view(short** view, int* n) {
        *view = i16_.empty() ? nullptr : i16_.data(); *n = static_cast<int>(i16_.size());
    }
    void get_i8_view(signed char** view, int* n) {
        *view = i8_.empty() ? nullptr : i8_.data(); *n = static_cast<int>(i8_.size());
    }
    void get_u64_view(unsigned long long** view, int* n) {
        *view = u64_.empty() ? nullptr : u64_.data(); *n = static_cast<int>(u64_.size());
    }
    void get_u32_view(unsigned int** view, int* n) {
        *view = u32_.empty() ? nullptr : u32_.data(); *n = static_cast<int>(u32_.size());
    }
    void get_u16_view(unsigned short** view, int* n) {
        *view = u16_.empty() ? nullptr : u16_.data(); *n = static_cast<int>(u16_.size());
    }
    void get_u8_view(unsigned char** view, int* n) {
        *view = u8_.empty() ? nullptr : u8_.data(); *n = static_cast<int>(u8_.size());
    }
    void get_codes_view(int** view, int* n) {
        *view = codes_.empty() ? nullptr : reinterpret_cast<int*>(codes_.data());
        *n = static_cast<int>(codes_.size());
    }

    /*!
     * \brief The buffer itself, untyped.
     *
     * For a reader or writer that moves a whole column at once and already
     * knows its type -- an HDF5 dataset, a memory-mapped block. Untyped because
     * the alternative is a switch that names all eleven typed accessors at
     * every such call site, and the caller has just read \ref type to decide
     * which one it would name.
     *
     * Null for a Bool column, whose values are bit-packed and so do not exist
     * as elements to point at.
     */
    void* data_ptr() {
        switch (type_) {
            case ColumnType::Float64: return f64_.empty() ? nullptr : f64_.data();
            case ColumnType::Float32: return f32_.empty() ? nullptr : f32_.data();
            case ColumnType::Int64:   return i64_.empty() ? nullptr : i64_.data();
            case ColumnType::Int32:   return i32_.empty() ? nullptr : i32_.data();
            case ColumnType::Int16:   return i16_.empty() ? nullptr : i16_.data();
            case ColumnType::Int8:    return i8_.empty() ? nullptr : i8_.data();
            case ColumnType::UInt64:  return u64_.empty() ? nullptr : u64_.data();
            case ColumnType::UInt32:  return u32_.empty() ? nullptr : u32_.data();
            case ColumnType::UInt16:  return u16_.empty() ? nullptr : u16_.data();
            case ColumnType::UInt8:   return u8_.empty() ? nullptr : u8_.data();
            case ColumnType::String:  return codes_.empty() ? nullptr : codes_.data();
            case ColumnType::Bool:    return nullptr;
        }
        return nullptr;
    }
    /// \see data_ptr
    const void* data_ptr() const {
        return const_cast<Column*>(this)->data_ptr();
    }

    // --- validity ---------------------------------------------------------

    /*!
     * \brief Whether a bit mask is allocated.
     *
     * Storage, not meaning: a column whose missing rows are recorded as ranges
     * answers `false` here and still has missing rows. Ask \ref has_missing for
     * the question that is usually meant, and \ref valid for one row.
     */
    bool has_mask() const { return !mask_.empty(); }
    const BitMask& mask() const { return mask_; }

    /// Whether any row is not valid, in whichever form this column stores it.
    bool has_missing() const { return !mask_.empty() || !na_.empty(); }

    /*!
     * \brief Validity as a bit mask, whatever the storage.
     *
     * Empty when every row is valid, which is the same convention \ref mask
     * uses. Ranges are expanded here rather than kept expanded, so a column
     * that nobody asks pays nothing: the whole point of the range form is that
     * a million rows of one fact do not become a million bits.
     */
    BitMask validity() const {
        if (na_.empty()) return mask_;
        BitMask m;
        m.assign(n_, true);
        for (const NaRange& r : na_) {
            const std::size_t last = r.last < n_ ? r.last : n_;
            for (std::size_t i = r.first; i < last; i++) m.set(i, false);
        }
        if (!mask_.empty())
            for (std::size_t i = 0; i < n_ && i < mask_.size(); i++)
                if (!mask_.test(i)) m.set(i, false);
        return m;
    }

    /// The runs recorded as not measured, in the order they were recorded.
    const std::vector<NaRange>& na_ranges() const { return na_; }

    /*!
     * \brief Record `[first, last)` as never measured, with a reason.
     *
     * Stored in \ref metadata under `na`, so it travels with the column through
     * any format that carries a description and costs about forty bytes rather
     * than one bit per row. A column that already carries a bit mask gets bits
     * instead -- mixing the two representations in one column would make every
     * reader carry both paths for no gain.
     */
    void add_na_range(std::size_t first, std::size_t last, const std::string& why);

    /// The packed bits of a Bool column, for a writer that moves memory rather
    /// than values. Empty for every other type, which have \ref data_ptr.
    const BitMask& bits() const { return bits_; }

    /// \see bits. The inverse, for a loader reading packed words off disk --
    /// eight times less to read than a byte per row, and no expansion pass.
    void set_bits(const std::uint64_t* w, std::size_t n_bits) {
        type_ = ColumnType::Bool;
        bits_.assign_words(w, n_bits);
        n_ = n_bits;
    }
    /// \see mask. The inverse, same reason.
    void set_mask_bits(const std::uint64_t* w, std::size_t n_bits) {
        mask_.assign_words(w, n_bits);
    }
    void set_mask(const unsigned char* m, int n) { mask_.from_bytes(m, n); }
    void clear_mask() { mask_.clear(); }

    /*!
     * \brief Whether row `i` holds a measurement.
     *
     * The interface, whichever way the answer is stored. The range scan is a
     * loop over as many entries as there were files in the merge, and it is
     * reached only by a column that has ranges at all -- a column with neither
     * form costs the one branch it always cost.
     */
    inline bool valid(std::size_t i) const {
        if (!mask_.empty() && !mask_.test(i)) return false;
        for (std::size_t k = 0; k < na_.size(); k++)
            if (i >= na_[k].first && i < na_[k].last) return false;
        return true;
    }

    /*!
     * Mark every non-finite value invalid.
     *
     * A NaN in a measured column means "not measured", and the alternative to
     * saying so is that it silently lands in a flow bin and is counted as data
     * that was merely off-scale.
     */
    std::size_t mask_non_finite() {
        if (type_ != ColumnType::Float64 && type_ != ColumnType::Float32) return 0;
        if (mask_.empty()) mask_.assign(n_, true);
        std::size_t n_masked = 0;
        for (std::size_t i = 0; i < n_; i++) {
            if (!std::isfinite(value_at(i))) { mask_.set(i, false); n_masked++; }
        }
        return n_masked;
    }

private:
    /*!
     * \brief Call `f(a.vec, b.vec)` on the typed vector this column holds.
     *
     * The one place that knows the ten numeric cases. Bool and String are not
     * here on purpose: a bit-packed payload and a dictionary are not vectors of
     * values and every caller has to treat them separately anyway.
     */
    template <typename S>
    static void widen(const S* PTOLIB_RESTRICT src, std::size_t n, double* PTOLIB_RESTRICT dst) {
        for (std::size_t i = 0; i < n; i++) dst[i] = static_cast<double>(src[i]);
    }
    template <class F>
    static void visit_pair(Column& a, const Column& b, F&& f) {
        switch (a.type_) {
            case ColumnType::Float64: f(a.f64_, b.f64_); break;
            case ColumnType::Float32: f(a.f32_, b.f32_); break;
            case ColumnType::Int64:   f(a.i64_, b.i64_); break;
            case ColumnType::Int32:   f(a.i32_, b.i32_); break;
            case ColumnType::Int16:   f(a.i16_, b.i16_); break;
            case ColumnType::Int8:    f(a.i8_,  b.i8_);  break;
            case ColumnType::UInt64:  f(a.u64_, b.u64_); break;
            case ColumnType::UInt32:  f(a.u32_, b.u32_); break;
            case ColumnType::UInt16:  f(a.u16_, b.u16_); break;
            case ColumnType::UInt8:   f(a.u8_,  b.u8_);  break;
            default: break;
        }
    }

    /// Give the first `n` rows an explicit all-valid mask, so a later set()
    /// does not read as "everything before this was missing too".
    void materialise_mask(std::size_t n) {
        if (!mask_.empty()) { if (mask_.size() < n_) { BitMask m; m.assign(n_, true);
                for (std::size_t i = 0; i < mask_.size(); i++) m.set(i, mask_.test(i));
                mask_ = m; } return; }
        mask_.assign(n_, true);
        (void) n;
    }

    /// Carry `src`'s validity into rows [`at`, `at` + `n`) of this column.
    void append_mask_from(const Column& src, std::size_t at, std::size_t n) {
        // The source's ranges shift by `at` and stay ranges, which is what keeps
        // a concat of twenty files from materialising twenty masks. Only when
        // one side already has bits does everything become bits -- see
        // add_na_range, which makes that decision in one place.
        if (mask_.empty() && !src.has_mask()) {
            for (const NaRange& r : src.na_) {
                const std::size_t last = (r.last < n ? r.last : n) + at;
                add_na_range(r.first + at, last, r.why);
            }
            return;                     // the appended rows are otherwise valid
        }
        if (!has_missing() && !src.has_missing()) return;  // all valid, stays implicit
        materialise_mask(at);
        for (std::size_t i = 0; i < n; i++)
            mask_.set(at + i, src.valid(i));
    }

    //: The authority. `name_`, `units_` and `na_` are caches kept in step by
    //: the setters -- there are two ways in, and both have to update all of
    //: them, or a column reports one name and serialises another.
    //:
    //: `na_` is a cache for a second reason: `valid()` is called once per row
    //: by every fill and every gather, and parsing JSON per row is not a thing
    //: that can happen.
    std::string metadata_;
    std::string name_;
    std::string units_;
    std::vector<NaRange> na_;
    ColumnType type_ = ColumnType::Float64;
    std::size_t n_ = 0;

    RawVector<double> f64_;
    RawVector<float> f32_;
    RawVector<std::int64_t> i64_;
    RawVector<std::int32_t> i32_;
    RawVector<short> i16_;
    RawVector<signed char> i8_;
    RawVector<unsigned long long> u64_;
    RawVector<unsigned int> u32_;
    RawVector<unsigned short> u16_;
    RawVector<unsigned char> u8_;
    BitMask bits_;                       // Bool columns

    std::vector<std::string> dictionary_;
    std::unordered_map<std::string, int> lookup_;
    RawVector<std::int32_t> codes_;

    BitMask mask_;                       // validity
};

/*!
 * \brief What one live store is, for the registry.
 */
struct DataStoreInfo {
    int id = 0;
    std::string label;
    std::size_t n_rows = 0;
    int n_columns = 0;
    /// The whole tree: this store's columns plus every group under it.
    std::size_t nbytes = 0;
    std::size_t n_selected = 0;
    /*!
     * Direct children, so a root that looks empty is not mistaken for one.
     *
     * Only a count. A per-group breakdown belongs to
     * `DataStore.memory_report()`, which is where a caller who wants it is
     * already looking -- putting one here would mean allocating a second vector
     * per store while the registry mutex is held.
     */
    int n_groups = 0;
};

class DataStore;

/*!
 * \brief Every store currently alive in the process.
 *
 * A session accumulates these without meaning to: a photon stream is one, the
 * bursts extracted from it are another, an SMLM localisation table a third, and
 * each is potentially most of the memory in the process. Without somewhere to
 * look, "why is this using 12 GB" has no answer short of a profiler.
 *
 * Stores register themselves and deregister on destruction, so the list is
 * always what is actually there rather than what someone remembered to record.
 * It holds raw pointers deliberately -- a registry that kept the stores alive
 * would be the leak it exists to diagnose.
 */
class DataStoreRegistry {
public:
    /*!
     * The one registry in the process.
     *
     * Defined in DataStore.cpp, NOT inline here. A function-local static in an
     * inline function is merged across translation units only when the symbol
     * is exported, and the Python extension is built with hidden visibility --
     * so core and the extension each got their own registry, and a store created
     * through one was invisible to a listing taken through the other. It looked
     * exactly like the registration not happening.
     */
    static DataStoreRegistry& instance();

    int add(DataStore* s) {
        std::lock_guard<std::mutex> lock(mutex_);
        const int id = ++next_id_;
        stores_.emplace_back(id, s);
        return id;
    }
    void remove(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < stores_.size(); i++) {
            if (stores_[i].first == id) { stores_.erase(stores_.begin() + i); return; }
        }
    }
    /// Snapshot of what is live. Defined after DataStore, which it inspects.
    std::vector<DataStoreInfo> list() const;
    std::size_t total_bytes() const;
    /// Number of live stores.
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stores_.size();
    }

private:
    DataStoreRegistry() = default;
    mutable std::mutex mutex_;
    std::vector<std::pair<int, DataStore*>> stores_;
    int next_id_ = 0;
};

/*!
 * \brief A table of columns, all the same length.
 *
 * Plus a row mask, which is the selection: "the bursts currently shown". A fill
 * honours it and the column's own validity mask together, so neither a
 * deselected row nor an unmeasured value can reach a histogram.
 */
class DataStore {
public:
    /*!
     * Registration is per INSTANCE, including copies.
     *
     * A copy is a second table holding a second lot of memory, and the registry
     * exists to show exactly that. Move transfers the identity instead, because
     * a moved-from store holds nothing.
     */
    DataStore() { id_ = DataStoreRegistry::instance().add(this); }
    explicit DataStore(std::string label) : label_(std::move(label)) {
        id_ = DataStoreRegistry::instance().add(this);
    }
    ~DataStore() { if (id_ != 0) DataStoreRegistry::instance().remove(id_); }

    DataStore(const DataStore& o)
            : columns_(o.columns_), n_rows_(o.n_rows_), row_mask_(o.row_mask_),
              label_(o.label_) {
        clone_groups_from(o);               // deep: a group belongs to one parent
        id_ = DataStoreRegistry::instance().add(this);
    }
    DataStore& operator=(const DataStore& o) {
        if (this != &o) {
            // Copying an ancestor into one of its own groups would read the tree
            // it is in the middle of rewriting. One pointer compare for a store
            // with no groups, which is every store that never uses them.
            if (o.contains(this))
                throw std::invalid_argument(
                        "assigning a store into its own subtree would make a cycle");
            columns_ = o.columns_; n_rows_ = o.n_rows_;
            row_mask_ = o.row_mask_; label_ = o.label_;
            groups_.clear();
            clone_groups_from(o);
        }
        return *this;                       // keeps its own registry identity
    }
    DataStore(DataStore&& o) noexcept
            : columns_(std::move(o.columns_)), n_rows_(o.n_rows_),
              row_mask_(std::move(o.row_mask_)), label_(std::move(o.label_)),
              groups_(std::move(o.groups_)) {
        o.n_rows_ = 0;
        id_ = DataStoreRegistry::instance().add(this);
    }
    // No cycle guard here, deliberately: this is noexcept, so throwing would
    // terminate. See operator=(const DataStore&), which is the reachable route.
    DataStore& operator=(DataStore&& o) noexcept {
        if (this != &o) {
            columns_ = std::move(o.columns_); n_rows_ = o.n_rows_;
            row_mask_ = std::move(o.row_mask_); label_ = std::move(o.label_);
            groups_ = std::move(o.groups_);
            o.n_rows_ = 0;
        }
        return *this;
    }

    /// Registry identity. Stable for the life of this instance.
    int id() const { return id_; }
    /// What this store is, for someone reading the registry listing.
    const std::string& label() const { return label_; }
    void set_label(std::string s) { label_ = std::move(s); }

    /*!
     * \brief Drop every column and free the memory, keeping the object.
     *
     * For a caller who knows a table is finished with but cannot drop the
     * handle -- a cache eviction, a plot that has been closed. Views handed out
     * before this DO keep their own memory alive, because they hold a reference
     * to the column; what is freed is this store's claim on it.
     *
     * **Drops the groups too**, so nbytes() really does go to zero. A release
     * that left most of a tree alive would defeat the one thing it is for.
     * Handles into the tree die with it, exactly as for clear_groups().
     */
    void release() {
        columns_.clear();
        columns_.shrink_to_fit();
        row_mask_.clear();
        n_rows_ = 0;
        groups_.clear();
    }

    std::size_t n_rows() const { return n_rows_; }
    int n_columns() const { return static_cast<int>(columns_.size()); }

    //! A compiled expression together with the buffers it is bound to.
    /*! Held so a repeated gate -- which is what an interactive selection is --
        costs only its evaluation, and so the referenced columns are widened
        once rather than per call. */
    struct ExpressionProgram;
    mutable std::map<std::string, std::shared_ptr<ExpressionProgram> >
        expression_cache_;

    Column& column(int i) { return columns_.at(i); }
    const Column& column(int i) const { return columns_.at(i); }

    /*!
     * \brief The index of a column of this store, or -1.
     *
     * Never looks in a group, and must not start. This is the hottest lookup in
     * the class -- column_by_name goes through it, and so does every
     * where/region/interval/histogram call from a binding -- and a group is not
     * a column, so a hit here could not be returned anyway. It is also what
     * keeps `store["meta"]` unambiguous when there is both a column and a group
     * called meta: the accessors differ, so nothing has to guess.
     */
    int find(const std::string& name) const {
        for (std::size_t i = 0; i < columns_.size(); i++)
            if (columns_[i].name() == name) return static_cast<int>(i);
        return -1;
    }
    Column& column_by_name(const std::string& name) {
        const int i = find(name);
        if (i < 0) throw std::invalid_argument("no column named " + name);
        return columns_[i];
    }
    const Column& column_by_name(const std::string& name) const {
        const int i = find(name);
        if (i < 0) throw std::invalid_argument("no column named " + name);
        return columns_[i];
    }

    /// The columns of this store. A group is not a column and never appears
    /// here; \see group_names.
    std::vector<std::string> column_names() const {
        std::vector<std::string> out;
        out.reserve(columns_.size());
        for (const Column& c : columns_) out.push_back(c.name());
        return out;
    }

    /// Add an empty column and return its index.
    int add_column(const std::string& name, ColumnType type) {
        if (find(name) >= 0) throw std::invalid_argument("duplicate column " + name);
        columns_.emplace_back(name, type);
        return static_cast<int>(columns_.size()) - 1;
    }

    /*!
     * \brief Drop a column and free it.
     *
     * Indices of the columns after it shift down by one, which is the price of
     * keeping them contiguous; a caller holding indices must adjust them.
     */
    void remove_column(int i) {
        if (i < 0 || i >= n_columns()) return;
        columns_.erase(columns_.begin() + i);
    }

    /*!
     * Declare the row count.
     *
     * Columns are checked against it rather than resized: a column of the wrong
     * length is a mistake upstream, and silently padding it turns that into a
     * plot nobody can explain.
     */
    void set_n_rows(std::size_t n) { n_rows_ = n; }

    /*!
     * \brief Every column of THIS store that disagrees with n_rows(), by name.
     *
     * Does not look into the groups, and must not: a group with a different row
     * count from its parent is the normal shape, not a fault. A results table
     * has one row per pixel and the meta beside it has one row, and reporting
     * that would make the arrangement this feature exists for look broken.
     */
    std::vector<std::string> inconsistent_columns() const {
        std::vector<std::string> bad;
        for (const Column& c : columns_)
            if (c.size() != n_rows_) bad.push_back(c.name());
        return bad;
    }

    // --- combining and subsetting ------------------------------------------
    //
    // The two binary operations `concat` is a fold over, and the one that
    // materialises a selection. Binary rather than variadic because a fold is
    // the same thing and is far easier to wrap for four languages; the
    // variadic `concat(stores, axis=..., join=...)` lives in each binding.
    //
    // They act on THIS store's columns, not on the tree. Concatenating two
    // roots that each hold a `results` group is ambiguous -- are the results
    // tables being stacked, or the roots? -- and row counts differ per group
    // anyway. Concatenating groups is `a.group("r").append_rows(b.group("r"))`,
    // said out loud. Same rule as the histogram fill, which also takes one
    // store: there is no cross-group operation anywhere in this class, and this
    // adds none.

    /// How a column present in one store and not the other is treated.
    enum class Join {
        Outer,   ///< keep it; the rows that had no value are marked not-measured
        Inner,   ///< keep only the columns both stores have
    };

    /// What to do when both stores have a column of the same name.
    enum class OnDuplicate {
        Refuse,     ///< say which name clashed, and change nothing
        KeepFirst,  ///< keep this store's, drop the other's
    };

    /*!
     * \brief Append `other`'s rows to this store.
     *
     * Columns line up by NAME, not by position: two burst tables written by
     * different runs need not have listed their columns in the same order. A
     * column whose type differs between the two throws rather than being
     * promoted -- widening a float32 to meet a float64 loses the dtype the
     * store exists to keep, and does it silently.
     *
     * With \ref Join::Outer a column missing from one side keeps its dtype and
     * the rows that had no value are marked not-measured. That is the advantage
     * over a data frame here, and it is not a small one: pandas has to widen an
     * int64 column to float64 to hold a NaN, and the dtype cannot be recovered
     * afterwards.
     *
     * \throws std::invalid_argument on a type conflict, naming the column.
     */
    void append_rows(const DataStore& other, Join join = Join::Outer);

    /*!
     * \brief Put `other`'s columns beside this store's.
     *
     * For files describing the SAME rows -- several analyses of one burst
     * table. Row counts must match: a mismatch throws and names both counts
     * rather than skipping the file, because a merge that quietly drops a
     * measurement is worse than one that stops.
     *
     * \throws std::invalid_argument on a row-count mismatch, or on a duplicate
     *         column name unless \ref OnDuplicate::KeepFirst is asked for.
     */
    void append_columns(const DataStore& other,
                        OnDuplicate on_duplicate = OnDuplicate::Refuse);

    /*!
     * \brief Fill `out` with rows `take_rows[0..n)` of this store.
     *
     * A copy, not a view: a `Column` owns its buffer and cannot borrow one, and
     * making it able to is a far larger change than this is worth. Column
     * order, dtypes, metadata, dictionaries and validity all come across. The
     * row selection does not, because the result IS the selection.
     *
     * Groups are not descended into -- their row counts are their own, so one
     * index list cannot mean anything across them.
     */
    void take_into(DataStore& out, const int* take_rows, int n_take_rows) const;

    /// \see take_into, for the rows the selection currently keeps. What every
    /// `select_*` was missing: 29 ways to express a gate and not one that
    /// yielded a store you could hand on or write back out.
    void compact_into(DataStore& out) const;

    /*!
     * \brief Fill `out` with an independent copy of this store, tree and all.
     *
     * Deep: every column owns its own buffer afterwards, so writing through one
     * store's values does not touch the other's. The group tree, dtypes,
     * dictionaries, validity, descriptions, labels and the row selection all
     * come across -- unlike \ref take_into and \ref compact_into, which drop
     * the selection because their result *is* one.
     *
     * The copy constructor did this already. It is a named method as well
     * because that is what a caller looks for, and because a constructor is not
     * how the other three bindings say it: `take_into` and `compact_into` are
     * reachable from all four and this has to be too, or "copy the store before
     * mutating it" is a Python-only idea.
     */
    void copy_into(DataStore& out) const;

    // --- selection --------------------------------------------------------

    bool has_row_mask() const { return !row_mask_.empty(); }
    const BitMask& row_mask() const { return row_mask_; }
    void set_row_mask(const unsigned char* m, int n) { row_mask_.from_bytes(m, n); }
    /// \see BitMask::assign_words -- for a loader taking the selection back off
    /// disk in the form it is held in.
    void set_row_mask_bits(const std::uint64_t* w, std::size_t n_bits) {
        row_mask_.assign_words(w, n_bits);
    }
    void clear_row_mask() { row_mask_.clear(); }
    std::size_t n_selected() const {
        return row_mask_.empty() ? n_rows_ : row_mask_.count();
    }
    inline bool row_selected(std::size_t i) const {
        return row_mask_.empty() || row_mask_.test(i);
    }

    // --- building a selection ---------------------------------------------

private:
    /*!
     * Evaluate `p` over a typed column, writing 64 results at a time.
     *
     * The obvious loop -- value_at(i) then mask.set(i, ...) -- costs a switch on
     * the column type and a read-modify-write of a word for EVERY row, and
     * measured four times slower than the numpy expression it is meant to
     * replace. Reading the column in its own type and accumulating a word in a
     * register before storing it once is what makes it cheaper instead: the
     * scan becomes bound by reading the column, which is the least any
     * implementation can do.
     */
    template<typename T, typename Pred>
    static void scan_typed(const T* v, std::size_t n, BitMask& m, Pred p) {
        std::uint64_t* w = m.words();
        const std::size_t nw = m.n_words();
        // Branchless, and that is the whole trick. `if (p(v[i])) bits |= ...`
        // is a branch on a comparison over unsorted measurement data, so it
        // mispredicts about half the time -- which on data this size costs more
        // than the comparison, the load and the store put together. Turning the
        // predicate into a 0 or 1 and shifting it leaves a loop with no branches
        // at all, which the compiler can also unroll.
        const std::size_t full = n / 64;
        for (std::size_t k = 0; k < full; k++) {
            const T* q = v + k * 64;
            unsigned char hits[64];
            for (int i = 0; i < 64; i++) hits[i] = p(q[i]) ? 1 : 0;
            std::uint64_t bits = 0;
            for (int j = 0; j < 8; j++) {
                std::uint64_t eight;
                std::memcpy(&eight, hits + 8 * j, 8);
                bits |= detail::pack8(eight) << (8 * j);
            }
            w[k] = bits;
        }
        if (full < nw) {                       // the partial last word
            std::uint64_t bits = 0;
            for (std::size_t i = full * 64; i < n; i++) {
                bits |= static_cast<std::uint64_t>(p(v[i]) ? 1 : 0) << (i - full * 64);
            }
            w[full] = bits;
            for (std::size_t k = full + 1; k < nw; k++) w[k] = 0;
        }
    }

    /*!
     * Evaluate a predicate on a PAIR of columns, word at a time.
     *
     * The two-dimensional counterpart of scan_column, and what a region drawn
     * on a scatter plot needs. Both columns are read in their own type; the
     * predicate sees doubles because that is what a shape is defined in.
     */
    template<typename Pred>
    void scan_xy(const Column& cx, const Column& cy, BitMask& m, Pred p) const {
        const std::size_t n = std::min(n_rows_, std::min(cx.size(), cy.size()));
        std::uint64_t* w = m.words();
        const std::size_t nw = m.n_words();
        // A block of each column widened to double once, then the predicate
        // over the pair as a branchless loop: one type switch per 512 rows
        // instead of two per row, which measured five times faster than
        // value_at() per point.
        const std::size_t kBlock = 512;
        double bx[512], by[512];
        for (std::size_t base = 0; base < n; base += kBlock) {
            const std::size_t len = std::min(kBlock, n - base);
            cx.copy_f64(base, len, bx);
            cy.copy_f64(base, len, by);
            // The predicate writes a byte per row -- the loop shape a
            // vectoriser takes -- and the bytes pack eight at a time.
            unsigned char hits[512];
            for (std::size_t j = 0; j < len; j++) hits[j] = p(bx[j], by[j]) ? 1 : 0;
            if (len < kBlock) std::memset(hits + len, 0, kBlock - len);
            for (std::size_t i = 0; i < len; i += 64) {
                std::uint64_t bits = 0;
                for (int j = 0; j < 8; j++) {
                    std::uint64_t eight;
                    std::memcpy(&eight, hits + i + 8 * j, 8);
                    bits |= detail::pack8(eight) << (8 * j);
                }
                w[(base + i) >> 6] = bits;
            }
        }
        for (std::size_t k = (n + 63) / 64; k < nw; k++) w[k] = 0;
        // A point whose position is unknown cannot be shown to be inside a
        // shape, and admitting it would quietly widen every selection.
        if (cx.has_missing()) m.and_with(cx.validity());
        if (cy.has_missing()) m.and_with(cy.validity());
    }

    /*!
     * Dispatch `p` over whatever type the column holds. One switch per COLUMN.
     *
     * `invalid_selected` says what a row the column marks as missing means. The
     * default -- false -- is the library's rule: "not measured" cannot satisfy a
     * condition. True is for a front end whose gates are written as comparisons
     * in a language where every comparison against a missing value is false, so
     * a missing value passes through the gate untouched. Both are defensible and
     * they are not interchangeable, so it is a parameter rather than a policy.
     */
    template<typename Pred>
    void scan_column(const Column& c, BitMask& m, Pred p,
                     bool invalid_selected = false) const {
        const std::size_t n = std::min(n_rows_, c.size());
        switch (c.type()) {
            case ColumnType::Float64: scan_typed(c.f64_ptr(), n, m, p); break;
            case ColumnType::Float32: scan_typed(c.f32_ptr(), n, m, p); break;
            case ColumnType::Int64:   scan_typed(c.i64_ptr(), n, m, p); break;
            case ColumnType::Int32:   scan_typed(c.i32_ptr(), n, m, p); break;
            case ColumnType::Int16:   scan_typed(c.i16_ptr(), n, m, p); break;
            case ColumnType::Int8:    scan_typed(c.i8_ptr(), n, m, p); break;
            case ColumnType::UInt64:  scan_typed(c.u64_ptr(), n, m, p); break;
            case ColumnType::UInt32:  scan_typed(c.u32_ptr(), n, m, p); break;
            case ColumnType::UInt16:  scan_typed(c.u16_ptr(), n, m, p); break;
            case ColumnType::UInt8:   scan_typed(c.u8_ptr(), n, m, p); break;
            case ColumnType::String:  scan_typed(c.codes_ptr(), n, m, p); break;
            case ColumnType::Bool: {
                for (std::size_t i = 0; i < n; i++) m.set(i, p(c.value_at(i) != 0.0));
                break;
            }
        }
        if (c.has_missing()) {
            if (invalid_selected) {
                // The missing rows pass the gate whatever the predicate made of
                // whatever was in the buffer for them.
                BitMask missing = c.validity();
                missing.invert();
                m.or_with(missing);
            } else {
                // A column that says a value is missing cannot satisfy any
                // condition.
                m.and_with(c.validity());
            }
        }
    }

public:

    /// How a new condition combines with the selection already there.
    enum class Combine { Replace, And, Or, AndNot };

    /*!
     * \brief Select the rows whose value in column `col` lies in [lo, hi).
     *
     * Evaluated in C++ over the column's own type -- a float32 column is
     * compared as float32 -- and written straight into the bit-packed
     * selection. The pattern this replaces is a bool array per condition,
     * combined with numpy and then turned into an index array: eight times the
     * memory for the mask, plus a second array of indices, plus the copy that
     * fancy-indexing makes.
     *
     * Rows the column marks invalid are never selected: "not measured" cannot
     * satisfy a range.
     */
    void select_range(int col, double lo, double hi, Combine how = Combine::Replace) {
        BitMask m(n_rows_, false);
        scan_column(column(col), m, [lo, hi](auto v) {
            const double d = static_cast<double>(v);
            return d >= lo && d < hi;
        });
        apply(m, how);
    }

    /*!
     * \brief Select the rows for which a boolean expression is true.
     *
     * The general form of a gate, where \ref select_range and the geometric
     * selectors are fixed shapes: `"(g-b)/(r-b) > 0.3"` names columns and is
     * compiled once, then evaluated over the store's own memory.
     *
     * Columns are referred to by name. Only the columns the expression names
     * are read, so a gate on two columns of a forty-column store touches two.
     * The expression is compiled by \ref ExpressionEngine and evaluated a block
     * of rows at a time, straight into the packed bits of the answer. An
     * expression over float32 columns is evaluated in single precision -- the
     * data's own -- so it agrees bit for bit with the same expression in numpy
     * or pandas; anything else is evaluated in double, with each column
     * converted as its block is read rather than widened into a copy first.
     *
     * Rows a referenced column marks invalid are never selected, the same rule
     * \ref select_range follows: "not measured" cannot satisfy a condition.
     * A row past the end of a referenced column is treated the same way.
     *
     * Python's `**` may be used for exponentiation, and `&`, `|`, `~` for the
     * elementwise boolean operators. A result that is not already boolean is
     * true where it is not zero, as `numpy.ndarray.astype(bool)` is.
     *
     * There is no second evaluator behind this one: an expression the engine
     * cannot compile is an error, stated, rather than an answer from a slower
     * path that might disagree. A Bool or String column reads as 0/1 or as
     * its dictionary code, the way \ref Column::value_at reports it.
     *
     * \param expr boolean expression over the column names
     * \param how how to combine the result with the current selection
     *
     * \throws std::invalid_argument if the expression does not compile, names
     *         an unknown column, or reads an Int64/UInt64 column whose values
     *         cannot be represented exactly. Python callers see this as a
     *         ValueError.
     */
    void select_expression(const std::string& expr,
                           Combine how = Combine::Replace);

    //! The rows an expression selects, without touching the selection.
    /*! Shared by \ref select_expression and \ref count_expression so the two
        cannot drift apart. */
    BitMask expression_mask(const std::string& expr) const;

    //! Drop compiled expressions; anything that moves a column must call this.
    void clear_expression_cache() const;

    //! Clear the bit of every row a referenced column marks as not measured.
    void apply_validity(const std::vector<int>& indices, BitMask& m) const;

    /*!
     * \brief How many rows an expression selects, without changing the
     *        selection or materialising a mask.
     *
     * The cheapest form of the question, for a caller that only wants the
     * count.
     */
    std::size_t count_expression(const std::string& expr) const;

    /// Select the rows whose value in `col` equals `value`. For categories and
    /// integer columns, where a range is the wrong question.
    void select_equal(int col, double value, Combine how = Combine::Replace) {
        BitMask m(n_rows_, false);
        scan_column(column(col), m,
                    [value](auto v) { return static_cast<double>(v) == value; });
        apply(m, how);
    }

    /*!
     * \brief Select the rows whose value in `col` lies in an interval whose
     *        endpoints may each be open or closed.
     *
     * \ref select_range is the library's own rule -- half-open, missing values
     * dropped -- and is unchanged. This is for reproducing a gate defined
     * somewhere else, where those two decisions were made differently and
     * changing them would change which points a published figure contains:
     *
     *  - **Closed endpoints.** Most gates a scientist draws are `lo <= v <= hi`.
     *    A half-open interval quietly drops the points exactly on the upper
     *    edge, which for integer-valued or binned columns is not a rounding
     *    detail but a visible bite out of the population.
     *  - **`invalid_selected`.** A front end that writes its gate as
     *    `(v >= lo) & (v <= hi)` in numpy keeps every NaN, because both
     *    comparisons are false and the point is never excluded by THIS gate --
     *    it is left for a separate "drop the non-finite" step to decide. Passing
     *    true reproduces that; the default reproduces the library's rule.
     *
     * `lo` and `hi` may be infinite, which is how a one-sided gate is written.
     * An infinite bound is a real comparison, not a missing value: with
     * `hi = +inf` a stored `+inf` is inside a closed interval and outside an
     * open one, exactly as the arithmetic says.
     */
    void select_interval(int col, double lo, double hi,
                         bool lo_closed = true, bool hi_closed = true,
                         bool invalid_selected = false,
                         Combine how = Combine::Replace) {
        BitMask m(n_rows_, false);
        scan_column(column(col), m,
                    [lo, hi, lo_closed, hi_closed, invalid_selected](auto v) {
                        const double d = static_cast<double>(v);
                        // NaN fails every comparison, so the two conventions
                        // differ on it and only on it.
                        if (d != d) return invalid_selected;
                        const bool above = lo_closed ? (d >= lo) : (d > lo);
                        const bool below = hi_closed ? (d <= hi) : (d < hi);
                        return above && below;
                    },
                    invalid_selected);
        apply(m, how);
    }

    /// Select the rows where every listed column has a finite, valid value.
    void select_finite(const std::vector<int>& cols, Combine how = Combine::Replace) {
        BitMask acc(n_rows_, true);
        for (int ci : cols) {
            const Column& c = column(ci);
            // An integer cannot be a NaN or an infinity, so there is nothing to
            // scan for: only what the column itself marks missing can exclude a
            // row. Worth the branch because a pixel coordinate is an integer
            // column and this runs on every redraw.
            if (!is_floating(c.type())) {
                if (c.has_missing()) acc.and_with(c.validity());
                continue;
            }
            BitMask m(n_rows_, false);
            scan_column(c, m,
                        [](auto v) { return std::isfinite(static_cast<double>(v)); });
            acc.and_with(m);
        }
        apply(acc, how);
    }

    // --- regions -----------------------------------------------------------
    //
    // The shapes a user draws on a scatter plot: a rectangle, an ellipse, a
    // lasso, a painted mask. Evaluated here rather than in the front end
    // because the data is here -- the alternative is handing out two columns,
    // testing them elsewhere, and handing back a mask the size of the table.

    /// Rows inside the axis-aligned rectangle [x0, x1) x [y0, y1).
    void select_rectangle(int col_x, int col_y, double x0, double y0,
                          double x1, double y1, Combine how = Combine::Replace) {
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        BitMask m(n_rows_, false);
        scan_xy(column(col_x), column(col_y), m, [=](double x, double y) {
            return x >= x0 && x < x1 && y >= y0 && y < y1;
        });
        apply(m, how);
    }

    /*!
     * Rows inside an ellipse centred at (cx, cy) with semi-axes (rx, ry),
     * rotated by `angle` radians.
     *
     * The rotation is folded into two constants so the inner test is a pair of
     * multiply-adds and a comparison -- no trigonometry per point.
     */
    void select_ellipse(int col_x, int col_y, double cx, double cy,
                        double rx, double ry, double angle = 0.0,
                        Combine how = Combine::Replace) {
        const double ca = std::cos(-angle), sa = std::sin(-angle);
        const double irx = rx != 0.0 ? 1.0 / rx : 0.0;
        const double iry = ry != 0.0 ? 1.0 / ry : 0.0;
        BitMask m(n_rows_, false);
        scan_xy(column(col_x), column(col_y), m, [=](double x, double y) {
            const double dx = x - cx, dy = y - cy;
            const double u = (dx * ca - dy * sa) * irx;
            const double v = (dx * sa + dy * ca) * iry;
            return u * u + v * v <= 1.0;
        });
        apply(m, how);
    }

    /*!
     * \brief Rows where a quadratic form about (cx, cy) is at most `threshold`.
     *
     * `(dx, dy) M (dx, dy)^T <= threshold` with `M = [[a, b], [b, c]]`. With `M`
     * the inverse of a covariance matrix and `threshold` the square of a number
     * of standard deviations, this is a Mahalanobis gate -- the ellipse drawn
     * around a fitted 2-D Gaussian population.
     *
     * \ref select_ellipse says the same thing in centre-radii-angle form and is
     * what a drawn shape has. This says it in the form a FIT has, and the
     * difference is not presentation: converting one to the other means
     * diagonalising `M`, and a comparison against a boundary computed through
     * two square roots and an arctangent does not agree bit for bit with one
     * computed from the coefficients directly. For a gate that decides which
     * points appear in a published population, reproducing the arithmetic is
     * worth a second entry point. It also stays meaningful when `M` is not
     * positive definite -- a covariance from a failed fit -- where the radii
     * form has no answer at all.
     */
    void select_quadratic(int col_x, int col_y, double cx, double cy,
                          double a, double b, double c, double threshold,
                          Combine how = Combine::Replace) {
        BitMask m(n_rows_, false);
        scan_xy(column(col_x), column(col_y), m, [=](double x, double y) {
            const double dx = x - cx, dy = y - cy;
            return a * dx * dx + 2.0 * b * dx * dy + c * dy * dy <= threshold;
        });
        apply(m, how);
    }

    /*!
     * \brief Combine a row-per-byte boolean mask into the selection.
     *
     * The way in for a condition the store has no primitive for. A caller can
     * read the one or two columns it needs as zero-copy views, decide in
     * whatever language it is written in, and combine the answer here -- rather
     * than taking the selection out, combining outside, and putting a whole new
     * one back, which is where a second representation of the selection starts.
     */
    void select_mask_rows(const unsigned char* m, int n,
                          Combine how = Combine::Replace) {
        BitMask b(n_rows_, false);
        const std::size_t k = std::min<std::size_t>(static_cast<std::size_t>(n < 0 ? 0 : n),
                                                    n_rows_);
        for (std::size_t i = 0; i < k; i++) if (m[i]) b.set(i, true);
        apply(b, how);
    }

    /*!
     * Rows inside a polygon, by the crossing-number rule.
     *
     * A lasso has a hundred vertices and the test is O(vertices) per point, so
     * the bounding box is checked first: a drawn region covers a small part of
     * the plane, most points fail four comparisons and never touch the edge
     * loop, and that is the difference between this being usable and not.
     */
    void select_polygon(int col_x, int col_y,
                        const double* xs, int n_xs, const double* ys, int n_ys,
                        Combine how = Combine::Replace) {
        const int nv = std::min(n_xs, n_ys);
        BitMask m(n_rows_, false);
        if (nv < 3) { apply(m, how); return; }

        double bx0 = xs[0], bx1 = xs[0], by0 = ys[0], by1 = ys[0];
        for (int i = 1; i < nv; i++) {
            bx0 = std::min(bx0, xs[i]); bx1 = std::max(bx1, xs[i]);
            by0 = std::min(by0, ys[i]); by1 = std::max(by1, ys[i]);
        }
        scan_xy(column(col_x), column(col_y), m, [=](double x, double y) {
            if (x < bx0 || x > bx1 || y < by0 || y > by1) return false;
            bool in = false;
            for (int i = 0, j = nv - 1; i < nv; j = i++) {
                if ((ys[i] > y) != (ys[j] > y) &&
                    x < (xs[j] - xs[i]) * (y - ys[i]) / (ys[j] - ys[i]) + xs[i]) {
                    in = !in;
                }
            }
            return in;
        });
        apply(m, how);
    }

    /*!
     * Rows whose (x, y) falls on a set pixel of a painted mask.
     *
     * `image` is `ny` rows of `nx` bytes -- row-major, y varying slowest --
     * covering [x0, x1) x [y0, y1). The parameter order says so: a 2-D numpy
     * array binds its first dimension to the first length, and that dimension
     * is the row count. Naming the row count `nx` and then indexing
     * `iy * nx + ix` transposes every non-square mask, silently and only for
     * non-square masks, which is exactly the bug this order exists to prevent.
     *
     * One multiply-add and a load per point, whatever the shape painted --
     * which is why an arbitrary drawing is no more expensive than a rectangle.
     */
    void select_mask_image(int col_x, int col_y,
                           const unsigned char* image, int ny, int nx,
                           double x0, double y0, double x1, double y1,
                           Combine how = Combine::Replace) {
        BitMask m(n_rows_, false);
        if (image == nullptr || nx < 1 || ny < 1 || x1 <= x0 || y1 <= y0) {
            apply(m, how);
            return;
        }
        const double sx = nx / (x1 - x0), sy = ny / (y1 - y0);
        scan_xy(column(col_x), column(col_y), m, [=](double x, double y) {
            // floor, not a cast: casting truncates towards zero, so a point one
            // half-pixel to the LEFT of the extent lands in column 0 instead of
            // -1 and is accepted by the range check below.
            const double fx = std::floor((x - x0) * sx);
            const double fy = std::floor((y - y0) * sy);
            if (!(fx >= 0.0 && fx < nx && fy >= 0.0 && fy < ny)) return false;
            const std::size_t ix = static_cast<std::size_t>(fx);
            const std::size_t iy = static_cast<std::size_t>(fy);
            return image[iy * static_cast<std::size_t>(nx) + ix] != 0;
        });
        apply(m, how);
    }

    /// Everything selected.
    void select_all() { row_mask_.clear(); }
    /// Nothing selected.
    void select_none() { row_mask_.assign(n_rows_, false); }

    /*!
     * \brief Ungate this store and every group under it.
     *
     * A selection does NOT propagate down a tree: groups have different row
     * counts, so a mask over one of them means nothing over another. Clearing
     * every gate at once is the exception, because it is the one tree-wide
     * operation callers actually want -- "show me all of it again" -- and it is
     * well defined whatever the row counts are.
     *
     * This is the one that also frees the masks, select_all() being a clear
     * rather than a fill.
     */
    void select_all_recursive() {
        select_all();
        for (const auto& g : groups_) g.second->select_all_recursive();
    }

    /// \see select_all_recursive. Selects nothing, everywhere; keeps the masks.
    void select_none_recursive() {
        select_none();
        for (const auto& g : groups_) g.second->select_none_recursive();
    }
    /*!
     * Flip the selection.
     *
     * Every row flips, including rows whose coordinates a region could not be
     * evaluated on because a column marks them missing. That is what "flip"
     * means and it is the only thing a mask operation can mean, but it is
     * usually not what a caller inverting a REGION wants: a point whose position
     * is unknown cannot be shown to be outside a shape any more than inside it.
     * Express that as `select_finite({x, y})` followed by the region with
     * Combine::AndNot, which keeps the missing rows out under both polarities.
     */
    void invert_selection() {
        if (row_mask_.empty()) { select_none(); return; }
        row_mask_.invert();
    }

    // -- groups ---------------------------------------------------------------
    //
    // A store is both a table (its own columns) and a container (its groups);
    // either may be empty. Each group is a full store -- its own columns, row
    // count, selection and label -- because that is what the data needs: a
    // results table has one row per pixel and the meta beside it has one row.
    //
    // Column and group namespaces are separate. A store may have a column and a
    // group of the same name, and nothing has to disambiguate, because the
    // accessors differ. In particular find() and column_by_name() never see a
    // group: they are the hottest lookup in the class and are not touched.
    //
    // Paths: "" and "/" mean this store, a leading and a trailing separator are
    // both optional, and "a/b" nests. add_group takes a NAME, the rest take a
    // path. An empty component ("a//b"), "." or ".." as a component, and a NUL
    // are rejected by throwing rather than by being reinterpreted.

    /// Direct children only.
    int n_groups() const { return static_cast<int>(groups_.size()); }

    /// Drop every group, and everything under them. Invalidates any handle into
    /// the tree; the store's own columns are untouched.
    void clear_groups() { groups_.clear(); }

    bool has_group(const std::string& path) const { return resolve(path) != nullptr; }

    /*!
     * \brief The group at `path`.
     *
     * A borrowed reference into the tree, owned by the root. It survives any
     * number of later add_group calls and the removal of unrelated siblings;
     * what invalidates it is removing it, or an ancestor.
     *
     * \note A group has no registry identity -- `id()` is 0 and it never appears
     *       in the live-store listing. The root's entry reports the whole tree.
     * \throws std::invalid_argument if there is no such group.
     */
    DataStore& group(const std::string& path) {
        DataStore* g = resolve(path);
        if (g == nullptr) throw std::invalid_argument("no group '" + path + "'");
        return *g;
    }
    const DataStore& group(const std::string& path) const {
        const DataStore* g = resolve(path);
        if (g == nullptr) throw std::invalid_argument("no group '" + path + "'");
        return *g;
    }

    /*!
     * \brief Add an empty group as a direct child.
     *
     * \param name a name, not a path -- a separator in it throws, because
     *        add_group("a/b") reads as "make b inside a" and does not.
     * \throws std::invalid_argument if a group of that name is already there.
     *         Replacing one is remove_group then add_group, said out loud.
     */
    DataStore& add_group(const std::string& name) {
        check_component(name, name);
        if (name.find(kGroupSeparator) != std::string::npos)
            throw std::invalid_argument(
                    "add_group takes a name, not a path: '" + name + "'");
        if (child(name) != nullptr)
            throw std::invalid_argument("group '" + name + "' is already there");
        groups_.emplace_back(name, std::unique_ptr<DataStore>(new DataStore(ChildTag{})));
        return *groups_.back().second;
    }

    /// The group at `path`, creating it and any intermediate it needs.
    /// Idempotent: calling it twice gives the same group, not two.
    DataStore& ensure_group(const std::string& path) {
        DataStore* cur = this;
        for (const std::string& name : split_path(path)) {
            DataStore* next = cur->child(name);
            cur = next != nullptr ? next : &cur->add_group(name);
        }
        return *cur;
    }

    /// False when there was no such group. Handles into the removed subtree
    /// die with it; handles to anything else survive.
    bool remove_group(const std::string& path) {
        const std::vector<std::string> parts = split_path(path);
        if (parts.empty()) return false;            // "" and "/" are this store
        DataStore* parent = this;
        for (std::size_t i = 0; i + 1 < parts.size(); i++) {
            parent = parent->child(parts[i]);
            if (parent == nullptr) return false;
        }
        for (auto it = parent->groups_.begin(); it != parent->groups_.end(); ++it)
            if (it->first == parts.back()) { parent->groups_.erase(it); return true; }
        return false;
    }

    /// Direct children, in the order they were added. Not alphabetical:
    /// insertion order is what a round trip has to preserve.
    std::vector<std::string> group_names() const {
        std::vector<std::string> out;
        out.reserve(groups_.size());
        for (const auto& g : groups_) out.push_back(g.first);
        return out;
    }

    /// Every descendant, depth first and parent before child, so every
    /// intermediate appears before anything under it.
    std::vector<std::string> group_paths() const {
        std::vector<std::string> out;
        append_paths(std::string(), out);
        return out;
    }

    /*!
     * \brief Total bytes held, by this store and every group under it.
     *
     * Columns and row masks only -- not the group names, not the container --
     * so the total stays exactly the sum of the per-column figures that
     * memory_report() lists. A subtotal that counted anything else would make
     * the two disagree.
     *
     * \warning Called by DataStoreRegistry::list() and total_bytes() while they
     *          hold a non-recursive mutex. Nothing this reaches -- including the
     *          recursion into groups -- may touch the registry, or the first
     *          data_store_report() deadlocks. Groups are unregistered, which is
     *          what makes that safe.
     */
    std::size_t nbytes() const {
        std::size_t b = row_mask_.nbytes();
        for (const Column& c : columns_) b += c.nbytes();
        for (const auto& g : groups_) b += g.second->nbytes();
        return b;
    }

private:
    /*!
     * \brief A store that belongs to a parent rather than to the registry.
     *
     * The registry holds ROOTS. A child that registered itself would make
     * total_bytes() double-count the moment nbytes() recurses, and would put an
     * entry in the listing that nobody can drop independently. Leaving id_ at 0
     * is all it takes -- the destructor already reads that as "nothing to
     * deregister".
     */
    struct ChildTag {};
    explicit DataStore(ChildTag) {}
    DataStore(ChildTag, const DataStore& o)
            : columns_(o.columns_), n_rows_(o.n_rows_), row_mask_(o.row_mask_),
              label_(o.label_) {
        clone_groups_from(o);
    }

    /// Deep-copy o's groups into this store's, as children.
    void clone_groups_from(const DataStore& o) {
        if (o.groups_.empty()) return;      // the common case, and it costs nothing
        groups_.reserve(o.groups_.size());
        for (const auto& g : o.groups_)
            // Not make_unique: it is not a friend and cannot see ChildTag.
            groups_.emplace_back(g.first, std::unique_ptr<DataStore>(
                    new DataStore(ChildTag{}, *g.second)));
    }

    /// Is p this store, or anywhere under it? Walks nothing when there are no
    /// groups, which is the only case the hot paths ever reach.
    bool contains(const DataStore* p) const {
        if (this == p) return true;
        for (const auto& g : groups_)
            if (g.second->contains(p)) return true;
        return false;
    }

    static const char kGroupSeparator = '/';

    /// One path component, or the name handed to add_group.
    static void check_component(const std::string& c, const std::string& whole) {
        if (c.empty())
            throw std::invalid_argument("empty group name in '" + whole + "'");
        if (c == "." || c == "..")
            throw std::invalid_argument("'" + c + "' is not a group name");
        if (c.find('\0') != std::string::npos)
            throw std::invalid_argument("a group name cannot contain a NUL");
    }

    /// The components of a path. Empty for "" and "/", which mean this store.
    static std::vector<std::string> split_path(const std::string& path) {
        std::vector<std::string> out;
        std::size_t b = 0, e = path.size();
        if (b < e && path[b] == kGroupSeparator) b++;              // leading, optional
        if (e > b && path[e - 1] == kGroupSeparator) e--;          // trailing, optional
        if (b >= e) return out;
        while (b < e) {
            std::size_t cut = path.find(kGroupSeparator, b);
            if (cut == std::string::npos || cut > e) cut = e;
            const std::string part = path.substr(b, cut - b);
            check_component(part, path);
            out.push_back(part);
            b = cut + 1;
        }
        return out;
    }

    DataStore* child(const std::string& name) {
        for (const auto& g : groups_)
            if (g.first == name) return g.second.get();
        return nullptr;
    }
    const DataStore* child(const std::string& name) const {
        return const_cast<DataStore*>(this)->child(name);
    }

    /// The store a path names, or nullptr. The single-component case is the one
    /// that happens, so it does not allocate a vector to find one child.
    const DataStore* resolve(const std::string& path) const {
        if (path.empty() || path == std::string(1, kGroupSeparator)) return this;
        if (path.find(kGroupSeparator) == std::string::npos) {
            check_component(path, path);
            return child(path);
        }
        const DataStore* cur = this;
        for (const std::string& name : split_path(path)) {
            cur = cur->child(name);
            if (cur == nullptr) return nullptr;
        }
        return cur;
    }
    DataStore* resolve(const std::string& path) {
        return const_cast<DataStore*>(
                static_cast<const DataStore*>(this)->resolve(path));
    }

    void append_paths(const std::string& prefix, std::vector<std::string>& out) const {
        for (const auto& g : groups_) {
            // A local copy, not out.back(): the recursive call push_backs into
            // the same vector and reallocates the reference away.
            const std::string path = prefix + g.first;
            out.push_back(path);
            g.second->append_paths(path + kGroupSeparator, out);
        }
    }

    void apply(BitMask& m, Combine how) {
        if (row_mask_.empty() && how != Combine::Replace) row_mask_.assign(n_rows_, true);
        switch (how) {
            case Combine::Replace: row_mask_ = m; break;
            case Combine::And:     row_mask_.and_with(m); break;
            case Combine::Or:      row_mask_.or_with(m); break;
            case Combine::AndNot:  row_mask_.andnot_with(m); break;
        }
    }

    // A deque, not a vector: adding a column must not invalidate a Column
    // reference already handed out. Nothing here needs the columns contiguous.
    std::deque<Column> columns_;
    std::size_t n_rows_ = 0;
    BitMask row_mask_;
    std::string label_;
    int id_ = 0;

    /*!
     * A vector of unique_ptr, not of DataStore.
     *
     * A handle to a group must survive a sibling being added AND removed, and
     * must survive the parent itself being moved -- and behind a pointer the
     * child never moves for any of the three. This is the bug the columns deque
     * exists to avoid, one level up: a reference handed out and then quietly
     * reallocated away reads freed memory and reports an empty name rather than
     * raising.
     *
     * (deque<pair<string, DataStore>> is not an option in any case: DataStore is
     * incomplete inside its own definition, and deque -- unlike vector -- may
     * not be instantiated on an incomplete type.)
     *
     * A vector because groups are few and ordered; lookup is a linear scan on
     * the name. The public API would be identical over any container, so if
     * sizeof(DataStore) ever matters this member and the special members above
     * are the whole of what would change.
     */
    std::vector<std::pair<std::string, std::unique_ptr<DataStore>>> groups_;
};

inline std::vector<DataStoreInfo> DataStoreRegistry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DataStoreInfo> out;
    out.reserve(stores_.size());
    for (const auto& kv : stores_) {
        const DataStore* s = kv.second;
        DataStoreInfo i;
        i.id = kv.first;
        i.label = s->label();
        i.n_rows = s->n_rows();
        i.n_columns = s->n_columns();
        i.nbytes = s->nbytes();          // the whole tree; groups are not listed
        i.n_selected = s->n_selected();
        i.n_groups = s->n_groups();
        out.push_back(i);
    }
    return out;
}

inline std::size_t DataStoreRegistry::total_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t b = 0;
    for (const auto& kv : stores_) b += kv.second->nbytes();
    return b;
}

/// Every store currently alive, largest first.
inline std::vector<DataStoreInfo> live_data_stores() {
    std::vector<DataStoreInfo> v = DataStoreRegistry::instance().list();
    std::sort(v.begin(), v.end(),
              [](const DataStoreInfo& a, const DataStoreInfo& b) { return a.nbytes > b.nbytes; });
    return v;
}

/// Total bytes held by every live store.
inline std::size_t live_data_store_bytes() { return DataStoreRegistry::instance().total_bytes(); }
// ===========================================================================
// ExpressionEngine -- compiled boolean/arithmetic expressions over columns
// ===========================================================================

/*!
 * \brief The layout of one column handed to the evaluator.
 *
 * Mirrors the numeric column types of \ref DataStore, kept as its own enum so
 * the engine does not depend on the store -- a caller with a plain array uses
 * it just as well.
 */
enum class ExprScalarType {
    Float64,
    Float32,
    Int64,
    Int32,
    Int16,
    Int8,
    UInt64,
    UInt32,
    UInt16,
    UInt8
};

/*!
 * \brief One input column, as a pointer and a type.
 *
 * The engine reads the caller's memory in place; nothing is copied into it and
 * nothing is widened ahead of time. A column whose \ref is_vector is false is a
 * single value at \ref data that broadcasts over every row, which is how a fit
 * parameter enters an equation without becoming an array.
 */
struct ExprColumn {
    /// First element of the column, or of the one value when broadcasting.
    const void* data = nullptr;
    /// How to read \ref data.
    ExprScalarType type = ExprScalarType::Float64;
    /// False for a single value broadcast over all rows.
    bool is_vector = true;
};

/*!
 * \brief An expression compiled to a block-vectorised program.
 *
 * Compile once, evaluate many times. The compiled form carries no per-call
 * state, so the same engine may be evaluated over different row counts and
 * different buffers without reparsing. Scratch buffers are kept between calls
 * -- allocating the block stack per call was measured to be most of the cost
 * for the few-hundred-point curves a fit evaluates -- which makes one engine
 * instance **not** safe to evaluate from two threads at once.
 *
 * \see DataStore::select_expression, which is this engine's first caller.
 */
class ExpressionEngine {
public:
    ExpressionEngine() = default;

    /*!
     * \brief Compile an expression, or report that this evaluator cannot.
     *
     * \param expression the expression, in the Python spelling a query is
     *        written in
     * \return true when a program was produced; false when the expression is
     *         malformed, names a function this evaluator does not implement, or
     *         nests deeper than the block stack allows. Nothing is thrown: a
     *         caller that has a fallback wants an answer, not an exception.
     *
     * On false the engine is left empty, and \ref ready answers false.
     */
    bool compile(const std::string& expression);

    /// The expression as it was handed to \ref compile.
    const std::string& expression() const { return expression_; }

    /*!
     * \brief The free names of the expression, in first-appearance order.
     *
     * These are the columns \ref compute_mask and \ref compute_values expect,
     * in the order they expect them. Names the engine resolves itself -- `pi`,
     * `e`, and every function it implements -- are not among them.
     */
    const std::vector<std::string>& variables() const { return variables_; }

    /// Whether a program is compiled and can be evaluated.
    bool ready() const { return !program_.empty(); }

    /// How many instructions the compiled program holds. For tests that pin
    /// what the folder and the subexpression pass actually did.
    std::size_t program_size() const { return program_.size(); }

    /// How many block-sized slots the shared-subexpression cache asked for.
    int cache_slots() const { return program_cache_size_; }

    /*!
     * \brief Evaluate as a gate, writing one bit per row.
     *
     * \param columns one per name in \ref variables, in that order
     * \param n_rows how many rows to evaluate
     * \param words destination, `(n_rows + 63) / 64` words, bit *i* of word
     *        *i/64* being row *i*. Every word written is overwritten whole, and
     *        bits past \p n_rows in the final word are cleared.
     *
     * The answer to a gate is a bit, and returning it as an array of doubles
     * costs eight bytes a row to say one. The block loop already carries its
     * booleans as one byte per row, so only the block's closing store differs
     * from \ref compute_values: the bytes are packed into words as the block
     * finishes, and no full-length intermediate exists at any point.
     *
     * A result that is not already boolean -- `"g"`, or `"g*2"` -- is taken as
     * true where it is not zero, which is numpy's cast to bool.
     *
     * \throws std::domain_error if no program is compiled, or if \p columns is
     *         not one per variable.
     */
    void compute_mask(const std::vector<ExprColumn>& columns,
                      std::size_t n_rows, std::uint64_t* words) const;

    /*!
     * \brief Evaluate as a curve, writing one double per row.
     *
     * \param columns one per name in \ref variables, in that order
     * \param n_rows how many rows to evaluate
     * \param out destination, \p n_rows doubles
     *
     * The arithmetic itself is done in whatever type the columns are -- float32
     * columns are evaluated in float32 -- and only the result is written as a
     * double.
     *
     * \throws std::domain_error if no program is compiled, or if \p columns is
     *         not one per variable.
     */
    void compute_values(const std::vector<ExprColumn>& columns,
                        std::size_t n_rows, double* out) const;

    /*!
     * \brief Rewrite the Python and pandas spellings into the engine's own.
     *
     * `**` becomes `^`, and `&`, `|`, `~` become `and`, `or`, `not`. Exposed
     * because a caller with a second evaluator behind this one has to hand it
     * the same string, and the two must not drift.
     *
     * \param s the expression as the user wrote it
     * \return the same expression in the engine's spelling
     */
    static std::string normalise(const std::string& s);

    /*!
     * \brief The free names of an already-normalised expression.
     *
     * \param s an expression, after \ref normalise
     * \return the names, deduplicated, in first-appearance order
     *
     * Exposed for the same reason as \ref normalise: a caller that resolves
     * columns before compiling needs exactly this list.
     */
    static std::vector<std::string> free_variables(const std::string& s);

private:
    //! One instruction of the block-vectorised program.
    struct VecOp {
        int kind = 0;       //!< constant, variable, operator, function, ...
        double value = 0.0; //!< the constant, or a constant exponent
        int index = 0;      //!< operator/function id, variable slot, cache slot
    };

    /*!
     * \brief Block scratch, one set per working type.
     *
     * Kept alive between calls. A six-deep program needs a 24 kB stack in
     * double, and allocating and freeing that on every call was most of the
     * cost at the curve lengths that actually occur.
     *
     * A stack slot is one of three things at any moment -- a block of numbers,
     * a block of one-byte booleans, or a single folded scalar -- and the flags
     * say which. Getting that reconciliation wrong at the boundaries is where
     * three real bugs lived, so the type is tracked rather than assumed.
     */
    template <typename T>
    struct Blocks {
        std::vector<T> stack;
        std::vector<unsigned char> bstack;
        std::vector<char> is_bool;
        std::vector<char> is_scalar;
        std::vector<T> scalar_value;
        std::vector<T> cache;
        std::vector<unsigned char> cache_bool;
        std::vector<char> cache_is_bool;
        std::vector<char> cache_is_scalar;
        std::vector<T> cache_scalar;
        std::vector<unsigned char> pack_scratch;
    };

    //! Tokenise and shunting-yard into \ref program_, then fold and share.
    bool compile_program(const std::string& normalised);

    //! Compute a repeated subtree once and reuse it, where that is cheaper.
    bool eliminate_common_subexpressions();

    //! The block loop. Exactly one of \p out and \p words is non-null.
    template <typename T>
    void run(const std::vector<ExprColumn>& columns, std::size_t n_rows,
             double* out, std::uint64_t* words, Blocks<T>& s) const;

    //! Check the columns and pick the working type, then \ref run.
    void dispatch(const std::vector<ExprColumn>& columns, std::size_t n_rows,
                  double* out, std::uint64_t* words) const;

    std::string expression_;
    std::string normalised_;
    std::vector<std::string> variables_;
    std::vector<VecOp> program_;
    int program_depth_ = 0;
    int program_cache_size_ = 0;

    mutable Blocks<double> scratch64_;
    mutable Blocks<float> scratch32_;
};

/*!
 * \brief The SIMD instruction set the expression engine evaluates with.
 *
 * `"avx2"`, `"sse2"`, `"neon"` or `"scalar"`. Chosen once, at the first
 * evaluation, from what the CPU reports: the header is compiled for the
 * baseline of its architecture and carries the wider tiers beside it, so one
 * binary runs everywhere and still uses AVX2 where it exists. Every tier
 * produces the same bits, so this is a statement about speed and never about
 * which rows a gate selects.
 */
const char* simd_tier();

/*!
 * \brief Choose the tier by name.
 *
 * For tests, which check every tier against the scalar one, and for
 * benchmarks. Returns false, changing nothing, for a name that is not one of
 * the four or a tier this CPU cannot run. Not for use while another thread is
 * evaluating: the choice is read at the start of each evaluation.
 */
bool set_simd_tier(const char* name);
#define PTOLIB_HAS_SIMD_TIER 1

// ===========================================================================
// Codecs -- built in, named, overridable
// ===========================================================================

/*!
 * \brief A compression codec, by name.
 *
 * The standard codec names are `zstd`, `brotli`, `lz4` and `deflate`.
 * CMake selects each provider with PTOLIB_<NAME>_PROVIDER: bundled, system,
 * or disabled. Bundled sources are compiled separately and require no
 * installed codec libraries. PTOLIB_DECODE_ONLY and
 * PTOLIB_DECODE_ONLY_CODECS omit encoding callbacks globally or by name.
 *
 * A codec may provide encoding, decoding, or both. Use can_compress() and
 * can_decompress() to query supported operations; has_codec() reports whether
 * the name is registered. register_codec() replaces a codec of the same name.
 * A missing callback denotes an unsupported operation. Brotli's encoder
 * dictionary remains private to its separately compiled encoder.
 *
 * `compress` writes the whole of `in` as one stream into `out`, at `level`
 * (`-1` for the codec's default: zstd 3, brotli 5, lz4 0 which is its fast
 * coder; lz4 3 and above is its HC coder; deflate 6, zlib's default). Brotli
 * writes with the largest standard window (lgwin 24) at every quality, so a
 * stream written at quality 11 is byte-identical to the reference
 * `brotli -q 11 -w 24` tool. `decompress` writes exactly `raw_size`
 * bytes into `out`, or, when `raw_size` is 0 because the writer did not
 * record it, as many as the stream holds. Both return false on failure and
 * must never throw.
 */
#ifndef SWIG
struct Codec {
    std::string name;
    std::function<bool(const unsigned char* in, std::size_t n, int level,
                       std::vector<unsigned char>& out)> compress;
    std::function<bool(const unsigned char* in, std::size_t n, std::size_t raw_size,
                       std::vector<unsigned char>& out)> decompress;
};
/// Register a codec, replacing one of the same name. At least one callback
/// must be present; a missing callback denotes an unsupported operation.
void register_codec(const Codec& codec);
/// Forget a codec. For tests that need to see what a reader does without one.
void unregister_codec(const std::string& name);
/// A copy of the codec registered under this name -- how a caller takes a
/// built-in out for a refusal test and puts it back. False if unknown.
bool codec_by_name(const std::string& name, Codec& out);
/// Compress with a registered codec. False if the codec is unknown or failed.
bool compress_bytes(const std::string& codec, const unsigned char* in, std::size_t n,
                    int level, std::vector<unsigned char>& out);
/// \see Codec::decompress. False if the codec is unknown or the stream is bad.
bool decompress_bytes(const std::string& codec, const unsigned char* in, std::size_t n,
                      std::size_t raw_size, std::vector<unsigned char>& out);
#endif
/// Whether a codec of this name is registered.
bool has_codec(const std::string& name);
/// Whether this build or a registered codec can encode the named format.
bool can_compress(const std::string& name);
/// Whether this build or a registered codec can decode the named format.
bool can_decompress(const std::string& name);
/// The registered codec names.
std::vector<std::string> codecs();

/*!
 * \brief An object encoding split at its codec suffix.
 *
 * `"dstore+zstd"` is a `.dstore` payload compressed with zstd; `"f32.col+brotli"`
 * a column of float32 under brotli; `"row16+zstd+delta"` rows that were
 * delta-coded by the application and then compressed with zstd. The part
 * before the first `+` says what the bytes are, the word after it names the
 * codec, and anything after that is the application's own transform, applied
 * before the codec and undone by the application after it -- the container
 * knows the codec and nothing else. An encoding with no `+` has an empty
 * `codec`.
 */
struct Encoding {
    std::string inner;      ///< what the bytes are once decoded: `dstore`, `row16`, `json`
    std::string codec;      ///< `zstd`, `brotli`, `lz4`, `deflate`, or empty
    std::string extra;      ///< the application's transforms after the codec: `delta`, or empty
};
/// \see Encoding
Encoding split_encoding(const std::string& encoding);

/*!
 * \brief How a store is written: which codec, if any, and how hard.
 *
 * With `codec` empty the file is what it always was, version 3, every blob
 * raw and mappable. With a codec named, blobs of at least `min_bytes` are
 * compressed and the file is version 4; a reader without that codec refuses
 * it, naming the codec. Columns are transformed first when `transform` is on
 * -- integers of 16 bits and wider are delta-coded, so a monotonic macro
 * time becomes a run of small numbers, and floats are byte-shuffled, so the
 * exponents and the mantissas each sit together -- which is most of the
 * ratio on measurement data. A column overrides all of this with a `codec`
 * attribute: a codec name, or `none` to stay raw.
 */
struct StoreOptions {
    std::string codec;          ///< "" for raw; "zstd", "brotli", ...
    int level = -1;             ///< the codec's default
    bool transform = true;      ///< delta for integers, shuffle for floats
    std::size_t min_bytes = 4096;   ///< smaller blobs stay raw
};

// ===========================================================================
// .dstore -- the native store file
// ===========================================================================

/// The magic at the head of a store file, and the customary extension.
// Inline constexpr, not extern data: MSVC cannot import data symbols
// across module DLLs, and SWIG wrappers reference these directly.
static constexpr const char* kStoreMagic = "TTTRSTOR";
static constexpr const char* kStoreExtension = ".dstore";

/*!
 * \brief Write a store, and everything under it, to `filename`.
 *
 * Goes to a temporary beside the target and is renamed into place, so a failed
 * write never leaves a half file where a good one was.
 *
 * \return false if the file could not be written. The reason goes to stderr.
 */
bool write_store(const std::string& filename, const DataStore& store);
/// \see write_store, with the codec and transforms \ref StoreOptions names.
/// False, with the reason on stderr, if the codec is not registered.
bool write_store(const std::string& filename, const DataStore& store,
                 const StoreOptions& options);

/*!
 * \brief Read a store file into `out`, replacing whatever it held.
 *
 * \throws std::runtime_error if the file is missing, not a store file, written
 *         by a newer version, byte-swapped, truncated, or corrupt in its
 *         directory. All of those are stated rather than guessed at: a reader
 *         that silently returns an empty table cannot be told apart from one
 *         that read an empty table.
 */
void read_store_into(DataStore& out, const std::string& filename);

/*!
 * \brief \see read_store_into, but only the named columns of each table.
 *
 * The directory says where every column is, so the ones not asked for are never
 * read -- two columns out of a four-gigabyte store costs two seeks. Names that
 * are not in the file are skipped silently; the group tree is rebuilt whole,
 * because it is the directory and costs nothing.
 */
void read_store_into(DataStore& out, const std::string& filename,
                     const std::vector<std::string>& columns);

/// \see read_store_into. Returns by value, which copies the whole tree at the
/// peak -- prefer the in-place form from a binding.
DataStore read_store(const std::string& filename);

/// Whether this file begins with the store magic. Silent on any input,
/// including a missing file: probing is a normal thing for a caller to do.
bool is_store_file(const std::string& filename);

#ifndef SWIG
/*!
 * \brief Write a store into an already-open file, at its current position.
 *
 * Every offset the directory records is relative to where the store starts, so
 * the result is a self-contained store file that happens to live inside
 * something bigger -- a PTO container, say. Nothing is buffered and no
 * temporary file is used, so a multi-gigabyte table costs its own bytes.
 *
 * Not exposed to the bindings: a FILE* is not something a binding can hold.
 *
 * \return bytes written, or 0 on failure.
 */
std::uint64_t write_store_at(std::FILE* f, const DataStore& store);
/// \see write_store_at, with \ref StoreOptions.
std::uint64_t write_store_at(std::FILE* f, const DataStore& store,
                             const StoreOptions& options);

/*!
 * \brief Read a store that is already in memory.
 *
 * What a store compressed as a whole inside a container comes back as, and
 * what a caller that fetched the bytes from somewhere that is not a file
 * has. The same reader, the same knobs: an empty `columns` means every
 * column, `n_rows` of 0 means to the end, an empty `group` the root.
 */
void read_store_into(DataStore& out, const unsigned char* bytes, std::size_t n);
void read_store_into(DataStore& out, const unsigned char* bytes, std::size_t n,
                     const std::vector<std::string>& columns,
                     std::uint64_t first_row = 0, std::uint64_t n_rows = 0,
                     const std::string& group = std::string());
/// \see store_columns, for a store in memory.
std::vector<std::string> store_columns(const unsigned char* bytes, std::size_t n,
                                       const std::string& group = "");
/// \see store_groups, for a store in memory.
std::vector<std::string> store_groups(const unsigned char* bytes, std::size_t n);
#endif
/// The format version word of a store file: 3 for a raw file, 4 for one with
/// compressed blobs. 0 for a file that is not a store.
std::uint32_t store_format_version(const std::string& filename);

/*!
 * \brief Read a store that begins `base` bytes into `filename`.
 *
 * \param bytes the length of the region, or 0 for "to the end of the file".
 * \see write_store_at, and \ref read_store_into for the whole-file case.
 */
void read_store_into(DataStore& out, const std::string& filename,
                     std::uint64_t base, std::uint64_t bytes);

/*!
 * \brief Both knobs at once: a column subset of a store embedded at `base`.
 *
 * The combination a container makes routine. A store inside a PTO is always the
 * `base`/`bytes` case, so without this a caller reading one could never ask for
 * a subset of its columns -- the single combination that matters was the single
 * one the API omitted.
 *
 * \see pto_read_store, which is the reason this exists.
 */
void read_store_into(DataStore& out, const std::string& filename,
                     std::uint64_t base, std::uint64_t bytes,
                     const std::vector<std::string>& columns);

/*!
 * \brief A column subset **and** a row range of an embedded store.
 *
 * A row range is a projection along the other axis from a column subset, and
 * costs the same kind of nothing: the directory records where every column's
 * blob begins and how wide its elements are, so a range is an offset and a
 * length per column. What a table viewer needs -- paging a million-row burst
 * table otherwise decodes a million rows to show fifty.
 *
 * Fixed-width columns are exact. A bit-packed column (bool, and every validity
 * mask) reads only the words its range falls in and is repacked to start at bit
 * zero. A dictionary-encoded text column reads its codes for the range and the
 * whole dictionary, which is small by construction.
 *
 * The range is applied to every table in the tree, each clamped to its own
 * length: a group with fewer rows than `first_row` comes back empty rather than
 * throwing. `n_rows` of 0 means "to the end".
 *
 * The store that comes back reports the range's length as its `n_rows`. It is a
 * window, not the file: writing it back would write the window.
 */
void read_store_into(DataStore& out, const std::string& filename,
                     std::uint64_t base, std::uint64_t bytes,
                     const std::vector<std::string>& columns,
                     std::uint64_t first_row, std::uint64_t n_rows,
                     const std::string& group = std::string());

/*!
 * \brief Read ONE group as the root of `out`.
 *
 * The third knob, and the cheapest of the three: the directory names every node
 * and every column's offset, so reaching a group is a scan of the directory --
 * a few kilobytes -- and not one byte of any group stepped over, however large
 * they are.
 *
 * A leading and a trailing separator are optional, as everywhere else a group
 * path is taken. The empty string is the root, which is the whole file.
 *
 * The tree BELOW the group comes back with it; the tree above it does not. That
 * is what makes the result a store in its own right rather than a view -- it
 * writes straight back out as a file whose root is the group asked for.
 *
 * \throws std::runtime_error if the group is not in the file. A caller who
 *         wants to ask rather than to read has \ref store_has.
 */
void read_store_into(DataStore& out, const std::string& filename,
                     const std::string& group);

/*!
 * \brief Whether the file holds this group.
 *
 * A directory read, touching no payload. Silent on any input, including files
 * that are not ours: probing is a normal thing to do.
 */
bool store_has(const std::string& filename, const std::string& group = "");

/// The column names of the root table, in order, without reading any data.
/// Empty for a file that is not one of ours.
std::vector<std::string> store_columns(const std::string& filename,
                                       const std::string& group = "");

/// \see store_columns, for a store that begins `base` bytes into `filename`.
std::vector<std::string> store_columns(const std::string& filename,
                                       std::uint64_t base, std::uint64_t bytes,
                                       const std::string& group = "");

/// Every group path in the file, depth first, without reading any data.
/// Empty for a file that is not one of ours.
std::vector<std::string> store_groups(const std::string& filename);

/// \see store_groups, for a store that begins `base` bytes into `filename`.
std::vector<std::string> store_groups(const std::string& filename,
                                      std::uint64_t base, std::uint64_t bytes);

/*!
 * \brief How many bytes the store reader has moved since the process started.
 *
 * A counter, because the claim "the reader did not touch those bytes" is not
 * one a wall clock can make: a warm page cache measures the cache. Every read
 * the store reader does goes through one place, and this is what that place
 * counts. Test scaffolding, and cheap enough to leave in.
 *
 * Not synchronised, because nothing in this library reads a store from more
 * than one thread. Take a difference around a single call and compare it to a
 * difference around another; the absolute value means nothing.
 */
std::uint64_t store_bytes_read();

// ===========================================================================
// PTO -- Portable Tagged Objects, the container
// ===========================================================================

/// What a \ref PtoTag carries. Covers PicoQuant's twelve header types, plus the
/// two object references that make provenance expressible.
enum class PtoType {
    Empty = 0,  ///< the tag's presence is the information
    UInt,       ///< PtoTag::u
    Int,        ///< PtoTag::i
    Float,      ///< PtoTag::d
    Date,       ///< PtoTag::i, nanoseconds since 2001-01-01 UTC (the EBML epoch)
    Text,       ///< PtoTag::text
    Bytes,      ///< PtoTag::bytes
    UID,        ///< PtoTag::u -- a reference to an object in this file
    UIDs,       ///< PtoTag::uids
    Floats,     ///< PtoTag::floats
    Ints,       ///< PtoTag::ints
};

/*!
 * \brief One piece of typed metadata, and what it is about.
 *
 * \par What PTO guarantees, and what it does not
 * A tag names its subject with \ref target, and may name another object as its
 * value with \ref PtoType::UID. That is a labelled edge, and it is the whole of
 * what the container provides. PTO does not define `derived_from`, does not
 * check that a referenced UID exists, and does not detect cycles: applications
 * disagree about all three, and a container that picks a winner is wrong for
 * everyone else.
 */
struct PtoTag {
    std::string name;               ///< opaque to the container; `pto.` is reserved
    PtoType type = PtoType::Empty;
    std::uint64_t target = 0;       ///< the object described; 0 means the file

    /*!
     * Position within an array, or -1 for a scalar.
     *
     * A PicoQuant header repeats a tag name once per element rather than
     * storing a list, and this is what lets such a header survive verbatim.
     */
    int index = -1;

    /// The type code this tag had in the format it came from, so a PTU header
    /// can be written back bit-exact. Zero when it came from nowhere.
    std::uint32_t source_type = 0;

    std::uint64_t u = 0;
    long long i = 0;
    double d = 0.0;
    std::string text;
    std::vector<unsigned char> bytes;
    std::vector<double> floats;
    std::vector<long long> ints;
    std::vector<std::uint64_t> uids;
};

/// A note somebody wrote down. Prose for people, as against \ref PtoTag, which
/// is values for programs.
struct PtoAnnotation {
    std::uint64_t target = 0;       ///< 0 means the file
    std::uint64_t first_row = 0;
    std::uint64_t last_row = 0;     ///< one past the end; both zero means all of it
    std::string text;
    std::string author;
    long long when = 0;             ///< nanoseconds since 2001-01-01 UTC, 0 if unset
};

/*!
 * \brief Reserved tag: the object this one accompanies, as a \ref PtoType::UID.
 *
 * A Becker & Hickl `.spc` keeps half its header in a `.set` beside it, so the
 * two have to travel together and be handed to the reader together. That makes
 * it container business rather than application business -- unlike
 * "derived from", which PTO deliberately leaves undefined -- and it is the one
 * relation the container names itself.
 */
static constexpr const char* kPtoSidecarTag = "pto.sidecar_of";

/// A run of free space inside the file. \see File::free_extents.
struct PtoExtent {
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
};

/// What the container knows about one payload without decoding it.
struct PtoObject {
    std::uint64_t uid = 0;
    std::string kind;               ///< photons, table, spectrum, image, attachment
    std::string encoding;           ///< dstore, ptu, hdf5, tiff, raw, ...
    std::string name;
    std::string media_type;
    std::string description;
    std::uint64_t rows = 0;         ///< advisory; 0 if the writer did not say
    /// The decoded size, when \ref encoding carries a codec suffix and the
    /// writer recorded it; 0 otherwise. \see split_encoding
    std::uint64_t raw_size = 0;

    /// Where the payload is in the file, and how much room it has. `capacity`
    /// is what an in-place update has to fit inside; see \ref File::update.
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t capacity = 0;
};

class File;

/*!
 * \brief What a file on disk would become inside a container.
 *
 * The three things an object needs before its bytes: what it is for, how to
 * decode it, and what to call it when it leaves again. \see pto_classify_path.
 */
struct PtoFileType {
    std::string kind;        ///< photons, table, spectrum, image, attachment
    std::string encoding;    ///< ptu, spc-130, csv, png, raw, ...
    std::string media_type;  ///< RFC 6838 type, empty when none is standard
};

/*!
 * \brief What a name alone says a file is.
 *
 * A table of extensions: `csv` is a `table`, `png` an `image`, `pdf` an
 * `attachment` with its media type, and anything the table does not know is
 * an `attachment` encoded `raw` -- carried, named, and left alone. Nothing here
 * opens the file. \ref File::classify is the hook an application overrides to
 * look at the bytes as well.
 */
PtoFileType classify_by_extension(const std::string& path);

// -- tables -----------------------------------------------------------------
//
// Declared before File so their default arguments are stated once: the
// friend declarations inside the class must not repeat them.

/*!
 * \brief Add a DataStore as an object, encoded as `dstore`.
 *
 * Serialised straight into the container with no intermediate copy and no
 * temporary file, so a photon stream of any size costs its own bytes and
 * nothing more.
 *
 * \param reserve extra bytes for a later in-place \ref File::update.
 * \return the new object's UID, or 0.
 */
std::uint64_t pto_add_store(File& file, const std::string& kind,
                            const std::string& name, const DataStore& store,
                            std::uint64_t reserve = 0);
/// \see pto_add_store, written with \ref StoreOptions: the object stays
/// encoded `dstore`, and the compression is inside it, per column, so the
/// directory and every partial read still work.
std::uint64_t pto_add_store(File& file, const std::string& kind,
                            const std::string& name, const DataStore& store,
                            std::uint64_t reserve, const StoreOptions& options);

/// Replace a `dstore` object's payload from a store, in place where it fits.
bool pto_update_store(File& file, std::uint64_t uid, const DataStore& store);
/// \see pto_update_store, with \ref StoreOptions.
bool pto_update_store(File& file, std::uint64_t uid, const DataStore& store,
                      const StoreOptions& options);

/// Record that `uid` accompanies `primary`. \see kPtoSidecarTag.
void pto_mark_sidecar(File& file, std::uint64_t uid, std::uint64_t primary);

/// Read a `dstore` object back. \throws std::runtime_error if it is not one.
void pto_read_store(const File& file, std::uint64_t uid, DataStore& out);

/*!
 * \brief \see pto_read_store, but only the named columns.
 *
 * The reason \ref read_store_into grew an overload taking both a region and a
 * column subset. An embedded store is always a region, so before that existed a
 * caller reading one had to take every column of it -- which is exactly the
 * all-or-nothing the container exists to avoid.
 *
 * Two columns out of a four-gigabyte table costs two seeks; the group tree comes
 * back whole either way, being the directory.
 */
void pto_read_store(const File& file, std::uint64_t uid, DataStore& out,
                    const std::vector<std::string>& columns);

/*!
 * \brief \see pto_read_store, for a window of rows as well as of columns.
 *
 * What a table viewer needs: paging a million-row burst table otherwise decodes
 * a million rows to show fifty. An empty `columns` means all of them.
 */
void pto_read_store(const File& file, std::uint64_t uid, DataStore& out,
                    const std::vector<std::string>& columns,
                    std::uint64_t first_row, std::uint64_t n_rows);

/*!
 * \brief Where a `dstore` object's payload lies, ready to be read.
 *
 * The seam between the container and the `.dstore` reader, made namable: a caller
 * with its own reason to reach a store hands the returned `offset` and `size`
 * to any of the region-taking \ref read_store_into overloads. Everything below
 * is this plus one call.
 *
 * \throws std::runtime_error if there is no such object, or it is not a store.
 */
PtoObject pto_store_region(const File& file, std::uint64_t uid);

/// An embedded store's column names, without reading a single column.
/// Empty if the object is not a `dstore`.
std::vector<std::string> pto_store_columns(const File& file, std::uint64_t uid,
                                           const std::string& group = "");

/// An embedded store's group paths, without reading any data.
std::vector<std::string> pto_store_groups(const File& file, std::uint64_t uid);

/*!
 * \brief One entry of a cue table: where an event ordinal sits in a payload.
 *
 * Advisory, always. A cue that is wrong must cost a slower decode and never a
 * wrong answer, which is why a reader seeks to the nearest cue *at or before*
 * what it wants and decodes forward from there.
 */
struct PtoCue {
    std::uint64_t event = 0;        ///< event ordinal within the payload
    std::uint64_t offset = 0;       ///< byte offset into the payload
    std::uint64_t time = 0;         ///< macro time at that event, 0 if unrecorded
};


/// One element of the file as \ref File::elements reports it.
struct Element {
    std::uint32_t id = 0;
    std::string name;                 ///< the element's name, or "" if unknown here
    std::uint64_t offset = 0;         ///< where its header begins
    std::uint64_t data_offset = 0;    ///< where its payload begins
    std::uint64_t size = 0;           ///< payload octets
    std::uint64_t total = 0;          ///< header plus payload
    int depth = 0;                    ///< 0 for a top-level element
};

/// One thing \ref File::verify found.
struct Problem {
    std::uint64_t offset = 0;
    std::string message;
    bool error = true;                ///< false for a warning (alignment, unknown ids)
};

/// The name of a known element id, or "" -- for tools that print a tree.
const char* element_name(std::uint32_t id);

/*!
 * \brief What \ref File::create writes into the banner by default.
 *
 * A container may outlive every program that reads it. The banner is a plain
 * text element near the head of the file saying how to decode the framing, so
 * that `strings run.pto | head` is enough to start. Applications append where
 * their reader lives.
 */
static constexpr const char* kDefaultBanner =
        "pto\n"
        "This is a .pto container -- PTO, Portable Tagged Objects: an EBML document, DocType \"pto\".\n"
        "Reader and specification: https://github.com/tpeulen/ptolib\n"
        "\n"
        "=== HOW TO DECODE THIS BINARY ===\n"
        "1. Framing: EBML Document (DocType \"pto\"). Header Magic: 0x1A45DFA3. Segment Magic: 0x18538067.\n"
        "2. VINT Integer Decoding (1-8 bytes): First byte's leading zero count N determines VINT byte width (N+1).\n"
        "   Mask highest 1-bit for sizes; preserve all bits for Element IDs.\n"
        "3. Target Payload Elements:\n"
        "   - AttachedFile (0x61A7): Container of one object.\n"
        "   - FileUID (0x46AE): 64-bit uint object handle.\n"
        "   - FileName (0x466E), PtoKind (0x1E54F001), PtoEncoding (0x1E54F002): ASCII strings.\n"
        "   - FileData (0x465C): Binary payload (8-byte aligned on disk).\n"
        "\n"
        "=== ASCII C99 DECODER PSEUDOCODE ===\n"
        "size_t read_vint(const uint8_t *b, uint64_t *v, int mask) {\n"
        "    int n = 1; uint8_t m = 0x80;\n"
        "    while ((b[0] & m) == 0) { m >>= 1; n++; }\n"
        "    *v = mask ? (b[0] & ~m) : b[0];\n"
        "    for (int i = 1; i < n; i++) *v = (*v << 8) | b[i];\n"
        "    return n;\n"
        "}\n"
        "/* Walk Segment -> Attachments -> AttachedFile (0x61A7) -> FileData (0x465C) */\n";

/*!
 * \brief A PTO file, open for reading or for writing.
 *
 * Everything except payload bytes is held in memory, which is a few kilobytes
 * for any realistic file, so listing objects and reading tags costs one open.
 * Payloads are read on demand and are never held.
 *
 * Existing payloads can change before \ref commit; closing does not roll back
 * edits. Readers must not access the file during editing. The two SeekHeads
 * protect index selection, not payload rollback or power-loss durability.
 */
class File {
public:
    File();
    virtual ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    /*!
     * \brief Create an empty container, replacing anything already at `filename`.
     *
     * Takes the writer lock (see \ref open) *before* truncating, so being
     * refused cannot destroy a container someone else is writing.
     *
     * \param banner text written into the container's banner element, for
     *        whoever opens the file with no reader at hand. \ref kDefaultBanner
     *        says how to decode the framing; an application passes its own to
     *        add where a reader is found. Empty writes no banner.
     */
    bool create(const std::string& filename, const std::string& title = "",
                const std::string& banner = kDefaultBanner);

    /*!
     * \brief Open an existing one. \param writable false opens it read-only.
     *
     * \par One writer at a time
     * A writable open takes an exclusive advisory lock on the file and returns
     * false at once -- never blocking -- if another process or another
     * `File` already holds it; \ref error then says it is open for writing
     * elsewhere. Two writers each carry their own slot table, freelist and
     * generation counter, so concurrent writers would corrupt each other's
     * state. A read-only open takes no lock and provides no snapshot isolation:
     * callers must coordinate readers with the writer externally.
     *
     * The lock lives on the descriptor: \ref close drops it, so does a failed
     * open, and so does the process ending, however it ends.
     */
    bool open(const std::string& filename, bool writable = false);

    /*!
     * \brief How a container is open. \see open
     *
     * The three differ in what they lock and in when what they wrote becomes
     * readable, which is the distinction that matters during an acquisition.
     */
    enum class Mode {
        /*!
         * No lock. Only read a container while its contents are stable;
         * concurrent edits can invalidate the reader's offsets and payloads.
         */
        ReadOnly = 0,
        /*!
         * Exclusive advisory lock. Some edits modify the file immediately;
         * \ref commit publishes the directory, without rollback or isolation.
         */
        ReadWrite = 1,
        /*!
         * Exclusive advisory lock, and records are appended and committed as
         * they arrive. Reached through a streaming writer built on this class
         * rather than by opening directly, because a stream owns the object it
         * is filling.
         */
        Stream = 2,
    };

    /// How this container is currently open. \see Mode
    Mode mode() const;

    bool is_open() const;
    void close();
    const std::string& filename() const;

    /// Why the last call returned false, or empty.
    const std::string& error() const;

    // -- what the file says about itself --------------------------------------

    std::string title() const;
    void set_title(const std::string& s);
    std::string writing_app() const;
    /// The banner text near the head of the file, or "" if it has none.
    /// \ref compact carries it over; \ref create takes it as a parameter.
    std::string banner() const;
    void set_writing_app(const std::string& s);
    /// 16 random bytes, identifying this file across copies and renames.
    std::vector<unsigned char> uuid() const;
    /// Which commit this is. Rises by one each time; 0 for a file never committed.
    std::uint64_t generation() const;
    /// The `DocTypeVersion` the EBML header declares. This writer writes 2.
    std::uint64_t doctype_version() const;

    // -- objects ---------------------------------------------------------------

    int n_objects() const;
    /*!
     * \brief Every object, **in the order they were written**.
     *
     * The order is part of the contract, not an accident of the container
     * layout: a name is a label rather than an identity, so re-running an
     * analysis with a changed setting writes a second object with the same
     * `(kind, name)` and the older one deliberately stays reachable. Write
     * order is then the only thing that says which is which, and every reader
     * needs it to mean the same thing -- see \ref find and \ref find_all.
     */
    std::vector<PtoObject> objects() const;
    bool has(std::uint64_t uid) const;
    /// \throws std::invalid_argument if there is no such object.
    PtoObject object(std::uint64_t uid) const;
    /*!
     * \brief The **most recently written** object with this name, or 0.
     *
     * Names are labels, not identities: a container may hold several objects
     * with one name, and this resolves the tie the way a caller asking for
     * "the" object almost always means -- the newest, which is the result of
     * the latest run.
     *
     * \note This returned the *oldest* match before 2026-08-11, which silently
     *       handed back the stalest analysis in the container to whoever used
     *       the most obvious call. Use \ref find_all to see every one, and
     *       \ref objects for the full write order.
     */
    std::uint64_t find(const std::string& name) const;
    /*!
     * \brief Every object with this name, oldest first, empty if none.
     *
     * What \ref find hides. A reader that wants to compare runs, or to notice
     * that there is more than one, asks here rather than re-deriving
     * "newest wins" from \ref objects -- which is how two readers come to
     * disagree about which result a container is showing.
     */
    std::vector<std::uint64_t> find_all(const std::string& name) const;

    /*!
     * \brief Add an object, and return its UID.
     *
     * \param reserve bytes to leave after the payload so a later \ref update can
     *        grow into it without the object moving. Costs nothing but disk.
     * \return 0 on failure; see \ref error.
     */
    std::uint64_t add(const std::string& kind, const std::string& encoding,
                      const std::string& name, const unsigned char* data,
                      std::size_t n, std::uint64_t reserve = 0);


    /*!
     * \brief Replace an object's payload, keeping its UID.
     *
     * Rewrites in place when the new payload fits the room the old one had,
     * which is the point of the whole format: nothing else in the file moves,
     * whatever else is in it. When it does not fit, the object is written
     * elsewhere and the old space becomes free -- the UID survives, the offset
     * does not.
     */
    bool update(std::uint64_t uid, const unsigned char* data, std::size_t n);

    /// Drop an object. Tags targeting it are left alone, because an application
    /// may want to remember that something was there.
    bool remove(std::uint64_t uid);

    /*!
     * \brief Add an object whose payload is a file on disk.
     *
     * The mirror image of \ref extract, and the way in for something too big to
     * hold: \ref add takes a pointer and a length, so embedding a four-gigabyte
     * instrument file through it means having four gigabytes in hand first --
     * in Python, a `bytes` object the size of the file.
     *
     * This is not a second code path. \ref add already writes the header and
     * then streams the payload after it; this sizes the payload with
     * a stat and replaces that one write with a block
     * loop. Same header, same slot bookkeeping, same bytes on disk.
     *
     * \return 0 if the path is missing or unreadable, or on a write failure;
     *         see \ref error.
     */
    std::uint64_t add_file(const std::string& kind, const std::string& encoding,
                           const std::string& name, const std::string& path,
                           std::uint64_t reserve = 0);

    /*!
     * \brief Bundle a file on disk, letting the file say what it is.
     *
     * \ref add_file with \ref pto_classify_path in front of it: the caller
     * hands over a path and gets an object whose kind, encoding and media type
     * come from the file itself, named after it. The difference is who decides
     * -- `add_file` is for a caller that knows what it embedded, this is for
     * one that has a directory of files and wants them carried.
     *
     * Every argument after the path overrides what was inferred, so a caller
     * that knows better about one field does not lose the other two.
     *
     * \param name what the object is called, and what \ref disassemble writes
     *        it back out as. Defaults to the filename without its directory.
     * \return 0 if the path is missing or unreadable; see \ref error.
     */
    std::uint64_t attach(const std::string& path, const std::string& name = "",
                         const std::string& kind = "",
                         const std::string& encoding = "",
                         const std::string& media_type = "");


    /*!
     * \brief Add an object compressed with a registered codec.
     *
     * The object's encoding becomes `inner_encoding + "+" + codec` -- see
     * \ref split_encoding -- and its decoded size is recorded, so
     * \ref read hands the original bytes back and a listing can say how big
     * they are. \param level the codec's level, -1 for its default.
     * \return the uid, or 0 if the codec is not registered or failed; see
     *         \ref error.
     */
    std::uint64_t add_coded(const std::string& kind, const std::string& inner_encoding,
                            const std::string& codec, const std::string& name,
                            const unsigned char* data, std::size_t n,
                            int level = -1, std::uint64_t reserve = 0);
    /// \see update, for an object added with \ref add_coded: the new payload is
    /// compressed with the object's own codec, in place where it fits.
    bool update_coded(std::uint64_t uid, const unsigned char* data, std::size_t n,
                      int level = -1);
    /*!
     * \brief The payload, decoded.
     *
     * An object whose encoding carries a codec suffix comes back decompressed,
     * as the bytes it was added with; every other object comes back as
     * stored. \ref read_stored is the bytes on disk either way.
     *
     * \throws std::runtime_error if there is no such object, or its codec is
     *         not registered -- the message names the codec.
     */
    std::vector<unsigned char> read(std::uint64_t uid) const;
    /// The payload exactly as stored, compressed or not.
    /// \throws std::runtime_error if there is no such object.
    std::vector<unsigned char> read_stored(std::uint64_t uid) const;

    /*!
     * \brief `n` bytes of a payload, starting `at` bytes into it.
     *
     * The binding-facing half of \ref read_at: a caller pages through a payload
     * at whatever granularity suits, instead of materialising all of it to look
     * at part of it. Reads short at the end of the payload rather than
     * throwing, like a file read does. Pages the bytes as stored: a compressed
     * object is paged compressed, since a window of a compressed stream has no
     * meaning on its own -- \ref read decodes it whole.
     *
     * \throws std::runtime_error if there is no such object.
     */
    std::vector<unsigned char> read(std::uint64_t uid, std::uint64_t at,
                                    std::size_t n) const;

#ifndef SWIG
    /*!
     * \brief `n` bytes of a payload into a caller's buffer, no copy in between.
     *
     * \return how many bytes were actually read -- short at the end of the
     *         payload, and 0 for an object that is not there.
     */
    std::size_t read_at(std::uint64_t uid, std::uint64_t at,
                        void* into, std::size_t n) const;

    /*!
     * \brief Hand a payload to a sink in blocks, never holding it whole.
     *
     * The way to stream an object somewhere that is not a file -- a socket, a
     * hash, a decoder. \ref extract is this with a file-writing sink, which is
     * what it already was internally.
     *
     * \param sink called with each block in order; returning false stops the
     *        copy and makes this return false.
     */
    bool stream(std::uint64_t uid,
                const std::function<bool(const void*, std::size_t)>& sink) const;
    /// \see stream, for the bytes as stored: a coded payload arrives compressed.
    bool stream_stored(std::uint64_t uid,
                       const std::function<bool(const void*, std::size_t)>& sink) const;
#endif

    /*!
     * \brief Write an object's payload out as a file of its own.
     *
     * The way back out of the container: a measurement is saved as one `.pto`
     * holding the original instrument file and everything computed from it, and
     * this is how the instrument file becomes a `.ptu` again for something that
     * only reads those.
     *
     * Copied in blocks, so the payload is never held whole -- extracting an
     * eight-gigabyte stream costs eight gigabytes of disk and a few kilobytes
     * of memory.
     *
     * \return false if there is no such object, or the file could not be
     *         written; see \ref error.
     */
    bool extract(std::uint64_t uid, const std::string& filename) const;

    /*!
     * \brief Take the container apart: every object out into a directory.
     *
     * The way back to separate files. A measurement saved as one `.pto` holding
     * the instrument file and everything computed from it becomes a `.ptu` and
     * a table again, for tools that read only those.
     *
     * Sidecars land beside what they belong to, under their own names, which is
     * what a Becker & Hickl `.spc` needs: its reader looks for the `.set` next
     * to it on disk, and would otherwise silently read half a header.
     *
     * Each object is named by its \ref PtoObject::name, or by its UID when it has none
     * -- and when two share a name, the later ones get the UID as well, because
     * a name is a label and nothing stops two objects having the same one.
     *
     * A name is a relative path and is checked against \ref pto_object_names
     * before **anything** is written: one object that would land outside
     * `directory` fails the whole call, so a caller who sees the failure does
     * not also have half a directory. This is the check that stands between a
     * container somebody else wrote and the filesystem.
     *
     * \param on_written called with each path as it is written, for a caller
     *        that wants to report progress. Optional -- and the reason this
     *        exists: the CLI used to replicate the naming and the loop to get
     *        its progress ticks, which is how it came to be missing the check
     *        above. A second implementation is a second place to fix.
     *
     * \return the paths written, in object order. Empty if nothing could be;
     *         see \ref error for why.
     */
    std::vector<std::string> disassemble(
            const std::string& directory,
            const std::function<void(const std::string&)>& on_written =
                    std::function<void(const std::string&)>()) const;

    // -- metadata ---------------------------------------------------------------

    std::vector<PtoTag> tags() const;
    /// Tags whose target is `uid`. Pass 0 for the tags describing the file.
    std::vector<PtoTag> tags_for(std::uint64_t uid) const;
    /// Append a tag. A tag identical in every field to one already present is
    /// not appended again: re-describing an object must not make the container
    /// claim the same fact twice (a parent edge re-written on every re-run
    /// once accumulated one copy per analysis).
    void add_tag(const PtoTag& tag);
    /*!
     * \brief State a fact, replacing what was stated before.
     *
     * Removes every tag with the same `(target, name, index)`, then appends
     * `tag`. This is "the row grain IS x" as against \ref add_tag's "x is
     * also true" — the difference between the two is exactly the difference
     * between a scalar tag and an edge, and callers re-running an analysis
     * want this one for everything scalar. Tags describing other objects and
     * other names are untouched.
     */
    void set_tag(const PtoTag& tag);
    void set_tags(const std::vector<PtoTag>& tags);
    void clear_tags();
    /// Remove every tag with this `target` and `name`, any index.
    void clear_tags(std::uint64_t target, const std::string& name);

    std::vector<PtoAnnotation> annotations() const;
    void add_annotation(const PtoAnnotation& note);
    void clear_annotations();

    // -- making it stick ---------------------------------------------------------

    /*!
     * \brief Publish the current directory in the alternate generation index.
     *
     * Writes the index that is not currently live, with a higher generation and
     * a correct checksum. Payload and metadata edits may already be visible;
     * failure does not roll them back, and flushing is not a durability barrier.
     */
    bool commit();

    // -- cues -----------------------------------------------------------------


    /// The cue table for an object, ascending by event. Empty when it has none.
    std::vector<PtoCue> cues(std::uint64_t uid) const;

    /// Drop an object's cues. They are also dropped when the object is removed
    /// or its payload replaced, because a cue into bytes that changed is worse
    /// than no cue at all.
    void clear_cues(std::uint64_t uid);

    /*!
     * \brief Replace an object's cue table wholesale.
     *
     * What a caller that indexed the payload itself hands back -- the
     * container knows nothing about what an "event" is, so building a cue
     * table is the application's job (a photon library walks its records to
     * do it) and storing one is this. Written on the next \ref commit.
     */
    void set_cues(std::uint64_t uid, const std::vector<PtoCue>& cues);

    /// Push buffered writes to the operating system, so a second handle on
    /// \ref File::filename sees what this one wrote.
    void flush() const;

    /// Where the EBML header begins: 0 for a plain container, more for an
    /// executable bundle that carries a stub in front of it.
    std::uint64_t ebml_offset() const;

    /*!
     * \brief Every element in the file, in order, with its depth.
     *
     * The framing only -- no payload is read -- so this is what a `tree`
     * command or an inspector shows, and what \ref verify walks.
     */
    std::vector<Element> elements() const;

    /*!
     * \brief Check the file against the format, without trusting this reader.
     *
     * Every element header must decode, no element may reach past its parent,
     * a master's children must fill it exactly, the Segment must end at the
     * end of the file, each index's CRC-32 must verify, and payloads should
     * start on an 8-octet boundary (a warning, or an error when
     * `strict_alignment`). What `pto verify` prints; empty means clean.
     */
    std::vector<Problem> verify(bool strict_alignment = false) const;

    /*!
     * \brief What a path would become inside this container. \see attach
     *
     * The default is \ref classify_by_extension: a table of names, and
     * `attachment`/`raw` for anything it does not know. An application that
     * can recognise a file by its bytes -- a photon library with format
     * sniffers -- overrides this, and \ref attach and \ref pto_bundle_files
     * then pick its answer up.
     */
    virtual PtoFileType classify(const std::string& path) const;

    /*!
     * \brief Where an object of size zero keeps its payload, if anywhere.
     *
     * A container may carry an object as a *reference* to a file beside it
     * rather than as bytes inside it; how that reference is recorded is an
     * application's vocabulary, not the container's. \ref stream and
     * \ref extract ask here when an object has no bytes of its own. The
     * default knows no such convention and returns the empty string.
     */
    virtual std::string external_payload_path(std::uint64_t uid) const;

    /// The free space in the file. For tests, and for deciding whether a file
    /// has accumulated enough holes to be worth compacting.
    std::vector<PtoExtent> free_extents() const;

    /*!
     * \brief Copy the live objects to a new file, dropping the free space.
     *
     * The only way space comes back — the same bargain HDF5 makes with
     * `h5repack`. UIDs are preserved and offsets are not, so nothing but the
     * index may hold an offset. Tags, annotations and cues come across: a cue
     * addresses a byte offset *into* a payload, and this moves payloads without
     * changing a byte inside one.
     *
     * Payloads are streamed, never held, so compacting an eight-gigabyte
     * container costs a megabyte of memory.
     *
     * The two knobs are the trade between a small file and a file that stays
     * small. Neither is right for everyone, which is why neither is the only
     * behaviour:
     *
     * \param tight drop the padding that puts each payload on an 8-byte
     *        boundary as well, so the result carries no reclaimable `Void` at
     *        all. The file is as small as the format allows and its payloads
     *        can no longer be mapped and used in place. For an archive or a
     *        copy that is about to be sent somewhere; alignment is a SHOULD, so
     *        the result is still conformant. \see \ref pto_align.
     * \param reserve room to leave after every object, as a fraction of its
     *        payload — 0.25 gives a 4 MiB table a megabyte to grow into. The
     *        opposite trade: a bigger file that absorbs the next few updates
     *        without relocating anything, which is what a container being
     *        edited wants. Default 0, which is what a container being archived
     *        wants.
     *
     * The default is neither: holes gone, payloads aligned, nothing reserved.
     */
    bool compact(const std::string& to, bool tight = false, double reserve = 0.0);

protected:
    /// Record why a call in a derived class failed, so \ref error reports it
    /// the way it reports the base class's own failures.
    void set_error(const std::string& why);

private:
    struct Impl;
    Impl* p_;

    // These three put a DataStore straight into the container, which needs the
    // layout, not the public API: a photon stream is serialised into the file
    // where it will live rather than into a buffer first.
    friend std::uint64_t pto_add_store(File&, const std::string&,
                                       const std::string&, const DataStore&,
                                       std::uint64_t);   // defaults: see above
    friend std::uint64_t pto_add_store(File&, const std::string&,
                                       const std::string&, const DataStore&,
                                       std::uint64_t, const StoreOptions&);
    friend bool pto_update_store(File&, std::uint64_t, const DataStore&);
    friend bool pto_update_store(File&, std::uint64_t, const DataStore&,
                                 const StoreOptions&);
    friend void pto_read_store(const File&, std::uint64_t, DataStore&);
    // The rest of the store entry points need no friendship: they go through
    // pto_store_region and filename(), which is the whole point of it existing.
    friend PtoObject pto_store_region(const File&, std::uint64_t);
};
/*!
 * \brief Bundle files and directories into an open container, one object each.
 *
 * The way a measurement scattered over a directory becomes one file: the
 * instrument file, its settings sidecar, the analysis that produced the burst
 * table, the protocol PDF and the note somebody left. Each keeps its name, so
 * \ref File::disassemble puts the directory back as it was.
 *
 * \par What it does that a loop over \ref File::attach does not
 * - **A directory means everything under it**, recursively, with each object
 *   named by its path relative to that directory -- `raw/m001.ptu`, not
 *   `m001.ptu` -- so two files of the same name in different folders stay two
 *   files. Entries are visited in sorted order, so the same directory bundles
 *   to the same object order twice running.
 * - **A `.set` is tied to the `.spc` it belongs to** with \ref
 *   kPtoSidecarTag, which is what makes the pair readable afterwards: a
 *   Becker & Hickl reader handed the `.spc` alone silently reads half a header.
 *
 * Nothing is committed. The caller decides when the container becomes visible,
 * because bundling is usually one step of building it -- see \ref
 * File::commit.
 *
 * \param link_sidecars false to bundle a `.set` as a plain object, for a caller
 *        that wants to say what accompanies what itself.
 * \return the objects made, in the order they were written. Short of `paths`
 *         if something could not be read; \ref File::error says what.
 * \throws std::runtime_error if a directory cannot be walked to the end. A walk
 *         that stopped early would bundle some of a directory and report that
 *         it bundled the directory, which is the one outcome nobody could
 *         detect afterwards.
 */
std::vector<PtoObject> pto_bundle_files(File& file,
                                        const std::vector<std::string>& paths,
                                        bool link_sidecars = true);
/// True for a file that begins with an EBML header whose DocType is "pto".
/// Silent on any input, including a missing file.
bool is_pto_file(const std::string& filename);
}  // namespace pto

#endif  // PTOLIB_H
