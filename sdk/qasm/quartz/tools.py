"""
Quartz - The Computer Assembler
================================

quartz/tools.py - low-level byte/number encoding helpers shared by the
assembler (qasm.py) and disassembler (qdisasm.py).

Covers two independent concerns:
  - to_bytes_le()/from_bytes_le(): little-endian integer <-> byte-list
    conversion, used for every multi-byte operand (addresses, 16/32-bit
    immediates, etc.), matching the byte order the VM expects.
  - encode_float32/64() / decode_float32/64(): manual IEEE-754 bit-pattern
    encoding/decoding. These exist because Python's built-in `struct` module
    would work just as well, but implementing the conversion by hand keeps
    this module dependency-free and makes the exact bit-level behavior for
    edge cases (signed zero, NaN, infinity, subnormals) explicit and easy to
    match against the VM's own C implementation of the same encoding.
"""

import math

def to_bytes_le(val, size):
    """Encodes `val` as `size` little-endian bytes, masking it into the
    unsigned range first so negative Python ints are wrapped to their
    two's-complement byte representation."""
    val &= (1 << (size * 8)) - 1
    return [(val >> (8 * i)) & 0xFF for i in range(size)]


def from_bytes_le(data):
    """Inverse of to_bytes_le(): reassembles a little-endian byte sequence
    into a single unsigned integer."""
    val = 0
    for i, b in enumerate(data):
        val |= b << (8 * i)
    return val


def encode_float32(value):
    # Manual IEEE-754 binary32 encoder. Handles the special cases (signed
    # zero, NaN, infinity) explicitly up front, then derives sign/exponent/
    # mantissa from Python's arbitrary-precision float via math.frexp()
    # before biasing the exponent and rounding the mantissa into 23 bits,
    # including the subnormal (exp_biased <= 0) and mantissa-overflow ->
    # exponent-increment edge cases.
    if value == 0.0:
        return 0x80000000 if math.copysign(1.0, value) < 0 else 0
    if math.isnan(value):
        return 0x7FC00000
    if math.isinf(value):
        return 0xFF800000 if value < 0 else 0x7F800000

    sign = 0
    if value < 0:
        sign = 1
        value = -value

    mantissa, exponent = math.frexp(value)
    exponent -= 1
    mantissa *= 2
    exp_biased = exponent + 127

    if exp_biased <= 0:
        shift = 1 - exp_biased
        mantissa = mantissa / (2 ** shift)
        exp_biased = 0
        mantissa_bits = int(round(mantissa * (1 << 23)))
    elif exp_biased >= 255:
        return (sign << 31) | (0xFF << 23)
    else:
        mantissa_bits = int(round((mantissa - 1.0) * (1 << 23)))
        if mantissa_bits == (1 << 23):
            mantissa_bits = 0
            exp_biased += 1
            if exp_biased >= 255:
                return (sign << 31) | (0xFF << 23)

    return (sign << 31) | (exp_biased << 23) | (mantissa_bits & 0x7FFFFF)


def decode_float32(bits):
    # Inverse of encode_float32(): reconstructs a Python float from a raw
    # 32-bit IEEE-754 bit pattern, handling the subnormal (exp == 0), normal,
    # and special (exp == 255: inf/NaN) cases separately.
    sign = -1.0 if (bits >> 31) & 1 else 1.0
    exp = (bits >> 23) & 0xFF
    mantissa = bits & 0x7FFFFF
    if exp == 0:
        if mantissa == 0:
            return sign * 0.0
        return sign * (mantissa / (1 << 23)) * (2.0 ** -126)
    elif exp == 255:
        return sign * math.inf if mantissa == 0 else math.nan
    else:
        return sign * (1 + mantissa / (1 << 23)) * (2.0 ** (exp - 127))


def encode_float64(value):
    # Same approach as encode_float32(), but for IEEE-754 binary64 (11-bit
    # exponent, 52-bit mantissa) - used for the f64 typed-immediate size.
    if value == 0.0:
        return 0x8000000000000000 if math.copysign(1.0, value) < 0 else 0
    if math.isnan(value):
        return 0x7FF8000000000000
    if math.isinf(value):
        return 0xFFF0000000000000 if value < 0 else 0x7FF0000000000000

    sign = 0
    if value < 0:
        sign = 1
        value = -value

    mantissa, exponent = math.frexp(value)
    exponent -= 1
    mantissa *= 2
    exp_biased = exponent + 1023

    if exp_biased <= 0:
        shift = 1 - exp_biased
        mantissa = mantissa / (2 ** shift)
        exp_biased = 0
        mantissa_bits = int(round(mantissa * (1 << 52)))
    elif exp_biased >= 2047:
        return (sign << 63) | (0x7FF << 52)
    else:
        mantissa_bits = int(round((mantissa - 1.0) * (1 << 52)))
        if mantissa_bits == (1 << 52):
            mantissa_bits = 0
            exp_biased += 1
            if exp_biased >= 2047:
                return (sign << 63) | (0x7FF << 52)

    return (sign << 63) | (exp_biased << 52) | (mantissa_bits & 0xFFFFFFFFFFFFF)


def decode_float64(bits):
    # Inverse of encode_float64() - see decode_float32() for the general
    # approach, sized up to the 64-bit exponent/mantissa widths.
    sign = -1.0 if (bits >> 63) & 1 else 1.0
    exp = (bits >> 52) & 0x7FF
    mantissa = bits & 0xFFFFFFFFFFFFF
    if exp == 0:
        if mantissa == 0:
            return sign * 0.0
        return sign * (mantissa / (1 << 52)) * (2.0 ** -1022)
    elif exp == 2047:
        return sign * math.inf if mantissa == 0 else math.nan
    else:
        return sign * (1 + mantissa / (1 << 52)) * (2.0 ** (exp - 1023))
