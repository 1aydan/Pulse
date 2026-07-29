// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Minimal deterministic CSV accumulator. Deliberately NOT a wrapper around the engine's
 * FDiagnosticTableWriterCSV: that class is compiled out entirely under !ALLOW_DEBUG_FILES, and its
 * destructor check()s on a pending row, so a forgotten final CycleRow() is a crash rather than a
 * warning. Hand-rolling the ~20 lines removes both hazards and guarantees byte-identical output:
 * RFC 4180 quoting, a fixed "\n" terminator on every platform, UTF-8 without BOM.
 */
class FPulseCsvWriter
{
public:
	/** Appends one row. Fields are RFC 4180-quoted as needed; an empty array emits a blank line. */
	void WriteRow(const TArray<FString>& Fields);

	/** Saves the accumulated rows as UTF-8 without BOM. False fills OutError with the path. */
	bool SaveToFile(const FString& Path, FString& OutError) const;

private:
	FString Buffer;
};
