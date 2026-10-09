import sys

BIN_HEADER = [0x7F, 0x51, 0x00, 0x7F]

def from_bytes_le(data):
    val = 0
    for i, byte in enumerate(data):
        val |= byte << (8 * i)
    return val

def main(filename):
	with open(filename, "rb") as f:
		data = f.read()
	if data[:4] == bytes(BIN_HEADER):
		entry_point = from_bytes_le(data[6:10])
		code_size = from_bytes_le(data[10:14])
		data_size = from_bytes_le(data[14:18])
		data_addr = from_bytes_le(data[18:22])
		print('File type:\t\tQuartzVM')
		print(f'Version:\t\t{data[4]}')
		print(f'Runtime:\t\t{data[5]}')
		print(f'Entrypoint:\t\t0x{entry_point:08x}')
		print(f'Code size:\t\t{code_size} bytes')
		print(f'Data size:\t\t{data_size} bytes')
		print(f'Data address:\t\t0x{data_addr:08x}')
	else:
		print('Unknown file format')

if __name__ == '__main__':
	if len(sys.argv) < 2:
		print(f'Usage: {sys.argv[0]} <filename>')
		sys.exit(1)
	main(sys.argv[1])
