// Copyright Nikita Zolotukhin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IoStorePackageMap.h"
#include "UObject/ObjectResource.h"
#include "UObject/PackageFileSummary.h"

class FIoStorePackageMap;
class FIoStoreReader;

// Because FPackageFileSummary::SetPackageFlags is not marked as COREUOBJECT_API for whatever fucking reason
struct FUglyPackageSummaryPackageFlagsAccessWorkaround
{
	int32 Tag;
	FPackageFileVersion FileVersionUE;
	int32 FileVersionLicenseeUE;
	FCustomVersionContainer CustomVersionContainer;
	uint32 PackageFlags;
};

struct FExportPreloadDependencyList
{
	FPackageIndex OwnerIndex;
	TArray<FPackageIndex, TInlineAllocator<4>> CreateBeforeCreateDependencies;
	TArray<FPackageIndex, TInlineAllocator<4>> SerializeBeforeCreateDependencies;
	TArray<FPackageIndex, TInlineAllocator<4>> CreateBeforeSerializeDependencies;
	TArray<FPackageIndex, TInlineAllocator<4>> SerializeBeforeSerializeDependencies;

	void AddDependency( uint32 CurrentCommand, FPackageIndex FromIndex, uint32 FromCommand );
};

struct FAssetSerializationContext
{
	FPackageId PackageId;
	FString PackageHeaderFilename;
	FPackageMapExportBundleEntry* BundleData;
	FIoStoreReader* IoStoreReader;
	
	FPackageFileSummary Summary;
	int32 PackageSummaryEndOffset;
	int32 ExportMapStartOffset;

	TArray<FName> NameMap;
	TMap<FName, int32> NameReverseLookupMap;
	bool bNameMapWrittenToFile{false};
	bool bSerializingNameMap{false};
	
	TArray<FObjectImport> ImportMap;
	TArray<FObjectExport> ExportMap;
	TArray<FExportPreloadDependencyList> PreloadDependencies;
	TSet<int32> ProcessedExportBundles;
	/** Fix-ups to apply to import class paths after both imports and exports of this package are resolved */
	TMap<int32, FPackageIndex> ImportClassPathFixup;
};

class FAssetSerializationWriter : public FArchiveProxy
{
	FAssetSerializationContext* Context;
public:
	FAssetSerializationWriter( FArchive& Ar, FAssetSerializationContext* Context );

	virtual FArchive& operator<<(FName& Value) override;
	virtual void SetFilterEditorOnly(bool InFilterEditorOnly) override;
};

struct FSavedPackageInfo
{
	TArray<FIoChunkId> ExportBundleChunks;
	TArray<FIoChunkId> BulkDataChunks;
};

struct FReadOrder
{
	FString Filename;
	uint32 PakOrder;

	FReadOrder(const FString& InFilename)
	{
		FString FullFilename = FPaths::GetBaseFilename(InFilename);
		Filename = FullFilename;
		PakOrder = 0;
		if (Filename.EndsWith(TEXT("_P")))
		{
			uint32 ChunkVersionNumber = 1;
			Filename = FullFilename.LeftChop(2);
			int32 VersionEndIndex = FullFilename.Find("_", ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (VersionEndIndex != INDEX_NONE && VersionEndIndex > 0)
			{
				int32 VersionStartIndex = FullFilename.Find("_", ESearchCase::CaseSensitive, ESearchDir::FromEnd, VersionEndIndex - 1);
				if (VersionStartIndex != INDEX_NONE)
				{
					VersionStartIndex++;
					FString VersionString = FullFilename.Mid(VersionStartIndex, VersionEndIndex - VersionStartIndex);
					if (VersionString.IsNumeric())
					{
						Filename = FullFilename.Left(VersionStartIndex - 1);
						int32 ChunkVersionSigned = FCString::Atoi(*VersionString);
						if (ChunkVersionSigned >= 1)
						{
							ChunkVersionNumber = (uint32)ChunkVersionSigned + 1;
						}
					}
				}
			}
			PakOrder = 100 * ChunkVersionNumber;
		}
	}
};

class ZENTOOLS_API FCookedAssetWriter
{
protected:
	TSharedPtr<FIoStorePackageMap> PackageMap;
	TMap<FString, TArray<FString>> SavedPackages;
	TMap<TSharedPtr<FIoStoreReader>, FReadOrder> Readers;
	FString RootOutputDir;
	int32 NumPackagesWritten;
	TMap<FIoChunkId, FString> ChunkIdToSavedFileMap;
	TMap<FName, FSavedPackageInfo> SavedPackageMap;
public:
	FCookedAssetWriter( const TSharedPtr<FIoStorePackageMap>& InPackageMap, const TMap<TSharedPtr<FIoStoreReader>, FReadOrder>& ContainerReaders,  const FString& InOutputDir );
	
	FString GetReaderName(const TSharedPtr<FIoStoreReader>& Reader) { return Readers.FindChecked(Reader).Filename; };
	
	void WritePackages(const TArray<FString>& FilterStrings);
	void WriteGlobalScriptObjects( const TSharedPtr<FIoStoreReader>& Reader ) const;
	void WritePackageStoreManifest() const;
	void WriteResponseFiles() const;

	FORCEINLINE int32 GetTotalNumPackagesWritten() const { return NumPackagesWritten; }
private:
	TFunction<bool(const FPackageId&)> MakePackageFilterFunction(const TArray<FString>& FilterStrings) const;
	void WriteSinglePackage( FPackageId PackageId, const FPackageInfo& PackageInfo);
	void ProcessPackageSummaryAndNamesAndExportsAndImports( FAssetSerializationContext& Context, const EZenPackageVersion ZenPackageVersion) const;
	static FExportBundleEntry BuildPreloadDependenciesFromExportBundle( int32 ExportBundleIndex, FAssetSerializationContext& Context );
	static void BuildPreloadDependenciesFromArcs( FAssetSerializationContext& Context );
	static void BuildPreloadDependenciesFromExports( FAssetSerializationContext& Context );
	static void ReorderPackageImports( const TArray<int32>& OriginalImportOrder, FAssetSerializationContext& Context );
	
	FPackageIndex CreateScriptObjectImport( const FPackageObjectIndex& PackageObjectIndex, FAssetSerializationContext& Context ) const;
	FPackageIndex CreateExternalPackageObjectReference( const FPublicExportKey& PackageImport, FAssetSerializationContext& Context ) const;
	FPackageIndex CreateExternalPackageReference( const FPackageId& PackageId, FAssetSerializationContext& Context ) const;
	FPackageIndex CreatePackageExportReference( const FPackageMapExportBundleEntry* ExternalPackageData, int32 ExportIndex, FAssetSerializationContext& Context ) const;
	FPackageIndex ResolvePackageLocalRef( const FPackageMapExportBundleEntry* ExternalPackageData, const FPackageLocalObjectRef& ObjectRef, FAssetSerializationContext& Context ) const;
	static FPackageIndex CreatePackageImport( FName PackageName, FAssetSerializationContext& Context );
	FPackageIndex CreateObjectExport(const FPackageMapExportEntry& ExportData, FAssetSerializationContext& Context ) const;
	
	static int32 FindPackageExportByHash( const FPackageMapExportBundleEntry& PackageBundle, uint64 ExportHash );
	static FSoftObjectPath ResolvePackagePath( FPackageIndex PackageIndex, FAssetSerializationContext& Context );
	static FPackageIndex FindExistingObjectImport( FPackageIndex OuterIndex, FName ObjectName, FAssetSerializationContext& Context );

	static void WritePackageHeader( FArchive& Ar, FAssetSerializationContext& Context, const EZenPackageVersion ZenPackageVersion);
	static void WritePackageExports( FArchive& Ar, FAssetSerializationContext& Context );
	void WriteBulkData(const FAssetSerializationContext& Context );
};