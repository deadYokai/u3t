#pragma once
#include <windows.h>
#include <psapi.h>

typedef struct {
	BYTE bytes[256];
	char mask[256];
	SIZE_T length;
} ParsedPattern;

BOOL PatternMatch(const BYTE *, const BYTE *, const char *, SIZE_T);
void *FindPattern(void *, SIZE_T, const BYTE *, const char *, SIZE_T);
void *FindPatternInModule(HMODULE, const BYTE *, const char *, SIZE_T);
BOOL ParsePatternString(const char *, ParsedPattern *);
void *FindPatternString(HMODULE, const char *);
void *FindPatternWithOffset(HMODULE, const char *, int);
