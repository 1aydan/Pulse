// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseCsvWriter.h"

#include "Misc/FileHelper.h"

// RFC 4180: a field containing a comma, double-quote, CR, or LF must be wrapped in quotes.
static bool PulseCsvFieldNeedsQuoting(const FString& Field)
{
	for (const TCHAR Char : Field)
	{
		if (Char == TEXT(',') || Char == TEXT('"') || Char == TEXT('\r') || Char == TEXT('\n'))
		{
			return true;
		}
	}
	return false;
}

void FPulseCsvWriter::WriteRow(const TArray<FString>& Fields)
{
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		if (Index > 0)
		{
			Buffer.AppendChar(TEXT(','));
		}

		const FString& Field = Fields[Index];
		if (PulseCsvFieldNeedsQuoting(Field))
		{
			Buffer.AppendChar(TEXT('"'));
			Buffer += Field.Replace(TEXT("\""), TEXT("\"\""), ESearchCase::CaseSensitive);
			Buffer.AppendChar(TEXT('"'));
		}
		else
		{
			Buffer += Field;
		}
	}

	// Always "\n", never LINE_TERMINATOR: the platform-dependent macro would make the same report
	// differ byte-for-byte between a Windows and a Linux CI runner.
	Buffer.AppendChar(TEXT('\n'));
}

bool FPulseCsvWriter::SaveToFile(const FString& Path, FString& OutError) const
{
	if (!FFileHelper::SaveStringToFile(Buffer, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write %s"), *Path);
		return false;
	}
	return true;
}
