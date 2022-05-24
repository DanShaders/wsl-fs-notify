#include "pe.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {
uint8_t *image_data;
size_t image_size;

size_t offsetPE, offsetCOFFHeader, offsetOptionalHeader, offsetDataDirectories;

struct COFFHeader {
	uint16_t machine;
	uint16_t numberOfSections;
	uint32_t timeDateStamp;
	uint32_t pointerToSymbolTable;
	uint32_t numberOfSymbols;
	uint16_t sizeOfOptionalHeader;
	uint16_t characteristics;
} coffHeader;
static_assert(sizeof(COFFHeader) == 20);

struct OptionalHeader {
	uint16_t type;
	uint8_t majorLinkerVersion;
	uint8_t minorLinkerVersion;
	uint32_t sizeOfCode;
	uint32_t sizeOfInitializedData;
	uint32_t sizeOfUninitializedData;
	uint32_t addressOfEntryPoint;
	uint32_t baseOfCode;
	uint64_t imageBase;
	uint32_t sectionAlignment;
	uint32_t fileAlignment;
	uint16_t majorOperatingSystemVersion;
	uint16_t minorOperatingSystemVersion;
	uint16_t majorImageVersion;
	uint16_t minorImageVersion;
	uint16_t majorSubsystemVersion;
	uint16_t minorSubsystemVersion;
	uint32_t win32VersionValue;
	uint32_t sizeOfImage;
	uint32_t sizeOfHeaders;
	uint32_t checkSum;
	uint16_t subsystem;
	uint16_t dllCharacteristics;
	uint64_t sizeOfStackReserve;
	uint64_t sizeOfStackCommit;
	uint64_t sizeOfHeapReserve;
	uint64_t sizeOfHeapCommit;
	uint32_t loaderFlags;
	uint32_t numberOfRvaAndSizes;
} optionalHeader;
static_assert(sizeof(OptionalHeader) == 112);

const int MAX_DATA_DIRECTORIES = 16;

struct DataDirectory {
	uint32_t virtualAddress;
	uint32_t size;
} dataDirectories[MAX_DATA_DIRECTORIES];
static_assert(sizeof(dataDirectories) == 128);

struct ImportDirectoryTable {
	uint32_t importLookupTableRVA;
	uint32_t timestamp;
	uint32_t forwarderChain;
	uint32_t nameRVA;
	uint32_t importAddressTableRVA;
};
static_assert(sizeof(ImportDirectoryTable) == 20);

bool validate_offset(size_t offset) {
	return offset < image_size;
}

bool validate_offset(size_t offset, size_t len) {
	return validate_offset(offset) && validate_offset(offset + len - 1);
}

uint64_t get_ll(size_t offset, int len) {
	assert(len > 0 && len <= 8);
	assert(validate_offset(offset, len));

	uint64_t res = 0;
	for (int i = len; i--;) {
		res *= 256;
		res += image_data[offset + i];
	}
	return res;
}

void get_into(void *dest, size_t offset, size_t len) {
	assert(validate_offset(offset, len));
	memcpy(dest, image_data + offset, len);
}

template <typename T>
T get_object(size_t offset) {
	assert(validate_offset(offset, sizeof(T)));
	T result;
	memcpy(&result, image_data + offset, sizeof(result));
	return result;
}

template <typename T>
T mget_object(size_t offset) {
	T result;
	memcpy(&result, (void *) offset, sizeof(result));
	return result;
}
}  // namespace

void for_each_import(wchar_t *filename, uint64_t image_base, import_callback_t callback) {
	FILE *image_file = _wfopen(filename, L"rb");
	assert(image_file);

	fseek(image_file, 0L, SEEK_END);
	image_size = ftell(image_file);
	rewind(image_file);

	image_data = (uint8_t *) malloc(image_size);
	assert(fread(image_data, 1, image_size, image_file));
	fclose(image_file);

	assert(get_ll(0, 2) == 0x5a4d);  // DOS MZ format magic
	offsetPE = (size_t) get_ll(0x3c, 4);
	assert(get_ll(offsetPE, 4) == 0x4550);  // PE executable magic

	offsetCOFFHeader = offsetPE + 4;
	coffHeader = get_object<COFFHeader>(offsetCOFFHeader);
	assert(coffHeader.sizeOfOptionalHeader >= 112);  // Only PE32+ images supported

	offsetOptionalHeader = offsetCOFFHeader + 20;
	optionalHeader = get_object<OptionalHeader>(offsetOptionalHeader);
	assert(optionalHeader.type == 0x20b);             // PE32+ magic
	assert(optionalHeader.numberOfRvaAndSizes >= 2);  // Executable should import something
	assert(optionalHeader.numberOfRvaAndSizes <= MAX_DATA_DIRECTORIES);
	assert(coffHeader.sizeOfOptionalHeader ==
		   112 + optionalHeader.numberOfRvaAndSizes * 8);  // Optional Header length mismatch

	offsetDataDirectories = offsetOptionalHeader + 112;
	get_into(dataDirectories, offsetDataDirectories, 8 * optionalHeader.numberOfRvaAndSizes);

	ImportDirectoryTable idt;
	size_t offset = dataDirectories[1].virtualAddress;
	while (1) {
		idt = mget_object<ImportDirectoryTable>(image_base + offset);
		if (idt.importLookupTableRVA == 0) {
			break;
		}
		assert(!idt.forwarderChain);

		uint64_t offset2 = image_base + idt.importLookupTableRVA;
		char *dll_name = (char *) (image_base + idt.nameRVA);

		while (1) {
			uint64_t ilt;
			memcpy(&ilt, (void *) (offset2), 8);

			if (ilt == 0) {
				break;
			}
			if (!(ilt & (1ull << 63))) {
				char *function_name = (char *) (image_base + ((ilt & ((1ll << 31) - 1)) + 2));
				uint64_t import_offset =
					offset2 - idt.importLookupTableRVA + idt.importAddressTableRVA;
				callback(dll_name, function_name, (uint64_t *) import_offset);
			}
			offset2 += 8;
		}
		offset += 20;
	}

	free(image_data);
}