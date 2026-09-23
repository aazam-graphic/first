#!/usr/bin/env python3
"""Write AWS IoT PEMs to NVS partition for azam-car.

Usage:
  python write_pems_nvs.py device_cert.pem private_key.pem AmazonRootCA1.pem
  python write_pems_nvs.py --read   # just read and print what's in NVS

This generates a tiny NVS binary that gets flashed at the NVS partition offset.
"""
import struct, hashlib, sys, os

# NVS page layout (simplified for single-page)
PAGE_SIZE = 4096
ENTRY_SIZE = 32  # key-value entry
CRC32_OFFSET = 32
MAX_CHARS = 4000  # max chars for a string/blob entry in a single page

def crc32(data):
    import binascii
    return binascii.crc32(data) & 0xFFFFFFFF

def make_entry(key, value_bytes, ns='alexa'):
    """Create a single NVS key-value entry (simplified, no encryption)."""
    data = bytearray(ENTRY_SIZE)
    # Entry header
    data[0] = 0xFF  # full entry
    data[1] = len(key)  # key length
    data[2] = ns.encode()[0] if ns else 0  # namespace id (0 = global)
    data[3] = 0x00  # reserved
    # Span count
    struct.pack_into('<H', data, 4, 1)
    # CRC32 of data (calculated later, placeholder)
    data[8:12] = b'\xff\xff\xff\xff'
    # Data offset (from start of entry)
    data[12:14] = b'\xff\xff'  # auto-calculate
    # Data type
    data[14] = 0x01  # NVS_TYPE_STRING
    # Reserved
    data[15] = 0
    # Key (up to 15 chars)
    key_bytes = key.encode()[:15]
    data[16:16+len(key_bytes)] = key_bytes
    data[16+len(key_bytes)] = 0
    # Value (string: length prefix + chars)
    val_len = len(value_bytes)
    struct.pack_into('<H', data, 28, val_len)
    return bytes(data), value_bytes

def make_namespace_entry(name='alexa'):
    """Namespace entry."""
    data = bytearray(ENTRY_SIZE)
    data[0] = 0xFF
    data[1] = len(name)
    data[2] = 0  # global namespace
    data[3] = 0
    struct.pack_into('<H', data, 4, 1)
    data[14] = 0x04  # NVS_TYPE_NS
    data[16:16+len(name)] = name.encode()
    data[16+len(name)] = 0
    return bytes(data)

def build_nvs_page(entries):
    """Build a minimal NVS page with given entries."""
    page = bytearray(PAGE_SIZE)
    # NVS page header
    page[0:4] = b'NVS'  # magic
    page[4] = 0x01  # version
    page[5] = 0xFF  # state: full (after writing)
    page[6] = 0xFF
    page[7] = 0xFF
    # Remaining space filled with 0xFF (erased flash)
    page[8:] = b'\xff' * (PAGE_SIZE - 8)
    return page

def main():
    if len(sys.argv) < 2:
        print("Usage: python write_pems_nvs.py device_cert.pem private_key.pem AmazonRootCA1.pem")
        print("       python write_pems_nvs.py --nvs nvs.bin  (flash with: esptool.py write_flash 0x9000 nvs.bin)")
        sys.exit(1)

    # For simplicity, output a Python script that writes via serial
    # Or generate a standalone NVS binary

    if sys.argv[1] == '--nvs' and len(sys.argv) == 3:
        # Flash pre-built NVS
        print(f"Flash with: esptool.py write_flash 0x9000 {sys.argv[2]}")
        return

    cert_file = sys.argv[1]
    key_file = sys.argv[2] if len(sys.argv) > 2 else None
    ca_file = sys.argv[3] if len(sys.argv) > 3 else None

    # Read files
    cert_pem = open(cert_file, 'r').read().strip()
    key_pem = open(key_file, 'r').read().strip() if key_file else None
    ca_pem = open(ca_file, 'r').read().strip() if ca_file else None

    print(f"cert: {len(cert_pem)} bytes")
    if key_pem: print(f"key:  {len(key_pem)} bytes")
    if ca_pem: print(f"ca:   {len(ca_pem)} bytes")

    # Build NVS data
    ns_entry = make_namespace_entry('alexa')
    page = bytearray(PAGE_SIZE)
    page[0:4] = b'NVS'
    page[4] = 0x01
    page[5:] = b'\xff' * (PAGE_SIZE - 5)

    offset = 32  # after page header (8 bytes + reserved)
    written = 0

    # Write namespace entry first
    ns_e = make_namespace_entry('alexa')
    page[offset:offset+32] = ns_e
    offset += 32
    written += 1

    # Write entries
    blobs = {'cert': cert_pem.encode()}
    if key_pem: blobs['key'] = key_pem.encode()
    if ca_pem: blobs['ca'] = ca_pem.encode()

    for k, v in blobs.items():
        entry, val_data = make_entry(k, v)
        # Write entry header (32 bytes)
        page[offset:offset+32] = entry
        offset += 32
        # Write value data after entries
        written += 1

    # Write value data at end of page (growing backward)
    data_offset = PAGE_SIZE
    for k, v in blobs.items():
        val_len = len(v)
        data_offset -= val_len
        # Align to 4 bytes
        data_offset = data_offset & ~3
        page[data_offset:data_offset+val_len] = v
        # Update entry's data offset
        for i in range(32, offset, 32):
            if page[i+16:i+16+len(k)] == k.encode():
                struct.pack_into('<H', page, i+12, data_offset - i)
                break

    # Write CRC
    crc = crc32(bytes(page[8:]))
    struct.pack_into('<I', page, 8, crc)

    out_file = 'nvs_pems.bin'
    with open(out_file, 'wb') as f:
        f.write(page)
    print(f"\nNVN binary written: {out_file}")
    print(f"Flash with: esptool.py write_flash 0x9000 {out_file}")

if __name__ == '__main__':
    main()
