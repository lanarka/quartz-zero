"""
Quartz - The Computer Assembler
================================

quartz/data.py - processes the assembler's %data directive.

The %data directive points at a small JSON descriptor file listing named
byte "blobs" to be concatenated into the .bin image's data segment (see
write_bin() in qasm.py), which the VM copies into RAM at load time (see
Program.data/data_addr in the VM's vm.h). Each blob can come from a literal
list of byte values, from the raw contents of an external file, or from a
string literal (encoded as one byte per character). This module only turns
that descriptor into a flat byte list plus a name->offset map (`addrs`,
later written out via write_data_map() for debugging); it has no knowledge
of assembly syntax or opcodes.

Expected JSON shape:
    {
      "start": "0x2000",
      "blobs": [
        {"my_bytes":  {"from_bytes": [1, 2, 3, "0x0A"]}},
        {"my_file":   {"from_file": "sprite.bin"}},
        {"my_string": {"from_string": "hello\\n"}}
      ]
    }
"""

import json

def to_int(s):
	# Parses a value as a plain decimal integer first, falling back to hex
	# (used for %data blob entries that may be given as e.g. "10" or "0x0A").
	try:
		val = int(str(s))
	except ValueError:
		val = int(str(s), 16)
	return val


def from_file(filename):
	# Reads a whole file's raw bytes to be embedded as a %data blob (used by
	# the "from_file" blob kind).
	with open(filename, "rb") as f:
		data = f.read()
	length = len(list(data))
	return length, list(data)


def from_string(string):
	# Encodes a string literal one byte per character (used by the
	# "from_string" blob kind) - note this is NOT UTF-8 encoding, just each
	# character's ordinal value, so it's only suitable for ASCII content
	# matching what the VM's text/font routines expect.
	length = len(string)
	done = []
	for ch in string:
		done.append(ord(ch))
	return length, done


def proc_data(filename, content):
	# Parses the already-read JSON `content` of a %data descriptor and
	# concatenates every blob (in declaration order) into one flat byte
	# list. Each blob's starting offset within that combined data segment is
	# recorded in `addrs` under its given name, so qasm.py can later expose
	# those offsets to assembly code (e.g. as %data blob addresses/labels)
	# and via write_data_map() for the .data-map debug file.
	size = 0
	data = []
	addrs = []
	obj = json.loads(content)
	for blob in obj["blobs"]:
		name = next(iter(blob.keys()))
		val = next(iter(blob.values()))
		
		if "from_bytes" in val:
			blob_data = val["from_bytes"]
			length = len(blob_data)
			addrs.append((name, size))
			size += length
			for i in blob_data:
				data.append(to_int(i))

		if "from_file" in val:
			blob_fn = val["from_file"]
			length, blob_data = from_file(blob_fn)
			addrs.append((name, size))
			size += length
			for i in blob_data:
				data.append(i)

		if "from_string" in val:
			blob_fn = val["from_string"]
			length, blob_data = from_string(blob_fn)
			addrs.append((name, size))
			size += length
			for i in blob_data:
				data.append(i)

	addr = to_int(obj["start"])
	print(f"Data segment {filename} successfully processed")
	return size, addr, data, addrs


def add_data(filename):
	# Entry point called from qasm.py's write_bin(): reads and processes the
	# %data descriptor file. On a missing file, prints an error and returns
	# an "empty data segment" triple rather than raising, so a missing/typo'd
	# %data file degrades to "no data segment" instead of crashing the whole
	# assembly run.
	try:
		with open(filename, "r") as f:
			content = f.read()
	except FileNotFoundError:
		raise Exception(f"Error: File not found {filename}: Data not processed!")
	return proc_data(filename, content)
