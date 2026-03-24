#include "pattern_scanner.hpp"
#include <psapi.h>
#include <cstdio>

BOOL PatternMatch(const BYTE *data, const BYTE *pattern, const char *mask,
                  SIZE_T length) {
	for (SIZE_T i = 0; i < length; i++) {
		if (mask[i] == '?')
			continue;
		if (data[i] != pattern[i])
			return FALSE;
	}
	return TRUE;
}

void *FindPattern(void *baseAddr, SIZE_T searchSize, const BYTE *pattern,
                  const char *mask, SIZE_T patternLen) {
	BYTE *current = (BYTE *)baseAddr;
	BYTE *end = current + searchSize - patternLen;

	while (current < end) {
		if (PatternMatch(current, pattern, mask, patternLen)) {
			return current;
		}
		current++;
	}

	return NULL;
}

void *FindPatternInModule(HMODULE module, const BYTE *pattern, const char *mask,
                          SIZE_T patternLen) {
	MODULEINFO modInfo;
	if (!GetModuleInformation(GetCurrentProcess(), module, &modInfo,
	                          sizeof(modInfo))) {
		return NULL;
	}

	return FindPattern(modInfo.lpBaseOfDll, modInfo.SizeOfImage, pattern, mask,
	                   patternLen);
}

BOOL ParsePatternString(const char *patternStr, ParsedPattern *out) {
	out->length = 0;
	const char *p = patternStr;

	while (*p && out->length < 256) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;

		if (*p == '?') {
			out->bytes[out->length] = 0;
			out->mask[out->length] = '?';
			out->length++;
			p++;
			if (*p == '?')
				p++;
			continue;
		}

		if (!isxdigit(*p)) {
			p++;
			continue;
		}

		char hex[3] = {0};
		hex[0] = *p++;
		if (*p && isxdigit(*p)) {
			hex[1] = *p++;
		}

		unsigned int value;
		if (sscanf(hex, "%02X", &value) == 1) {
			out->bytes[out->length] = (BYTE)value;
			out->mask[out->length] = 'x';
			out->length++;
		}
	}

	return out->length > 0;
}

void *FindPatternString(HMODULE module, const char *patternStr) {
	ParsedPattern parsed;
	if (!ParsePatternString(patternStr, &parsed)) {
		return NULL;
	}

	return FindPatternInModule(module, parsed.bytes, parsed.mask,
	                           parsed.length);
}

void *FindPatternWithOffset(HMODULE module, const char *patternStr,
                            int offset) {
	void *found = FindPatternString(module, patternStr);
	if (!found)
		return NULL;

	return (BYTE *)found + offset;
}
