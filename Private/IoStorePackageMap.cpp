// Copyright Nikita Zolotukhin. All Rights Reserved.

#include "IoStorePackageMap.h"
#include "Serialization/LargeMemoryReader.h"
#include "Serialization/MemoryReader.h"
#include "IO/IoContainerHeader.h"

void FIoStorePackageMap::SetDefaultZenPackageVersion(EZenPackageVersion NewDefaultPackageVersion)
{
	DefaultZenPackageVersion = NewDefaultPackageVersion;
}

EZenPackageVersion FIoStorePackageMap::GetDefaultZenPackageVersion() const
{
	return DefaultZenPackageVersion;
}

void FIoStorePackageMap::PopulateBulkData(const TSharedPtr<FIoStoreReader>& Reader)
{
	const TArray BulkDataChunkTypes{ EIoChunkType::BulkData, EIoChunkType::MemoryMappedBulkData, EIoChunkType::OptionalBulkData };

	for (TPair<FPackageId, FPackageInfo>& Package : PackageMap)
	{
		const FPackageId& PackageId = Package.Key;
		if (Package.Value.bIsOptional)
		{
			// Optional segment packages can only have optional segment bulk data
			const FIoChunkId BulkDataChunkId = CreateIoChunkId(PackageId.Value(), 1, EIoChunkType::BulkData);
			if (Reader->GetChunkInfo(BulkDataChunkId).IsOk())
			{
				Package.Value.BulkData.Add(FPackageBulkInfo(Reader, BulkDataChunkId));
			}
		}
		else
		{
			//// Required segment packages can have bulk data, memory mapped bulk data and optional bulk data
			for (const EIoChunkType BulkDataChunkType : BulkDataChunkTypes)
			{
				const FIoChunkId BulkDataChunkId = CreateIoChunkId(PackageId.Value(), 0, BulkDataChunkType);
				if (Reader->GetChunkInfo(BulkDataChunkId).IsOk())
				{
					Package.Value.BulkData.Add(FPackageBulkInfo(Reader, BulkDataChunkId));
				}
			}
		}
	}
}

void FIoStorePackageMap::PopulateFromContainer(const TSharedPtr<FIoStoreReader>& Reader)
{
	// If this is a global container, read the Script Objects from it
	TIoStatusOr<FIoBuffer> ScriptObjectsBuffer = Reader->Read(CreateIoChunkId(0, 0, EIoChunkType::ScriptObjects), FIoReadOptions());
	if (ScriptObjectsBuffer.IsOk())
	{
		ReadScriptObjects( ScriptObjectsBuffer.ValueOrDie() );
	}

	TArray<FPackageId> PackageIdsInThisContainer;
	TArray<FPackageId> OptionalPackageIdsInThisContainer;
	
	// Read the Package Headers from the Container Header of the container.
	TIoStatusOr<FIoBuffer> ContainerHeaderBuffer = Reader->Read(CreateIoChunkId(Reader->GetContainerId().Value(), 0, EIoChunkType::ContainerHeader), FIoReadOptions());
	if (ContainerHeaderBuffer.IsOk())
	{
		FMemoryReaderView Ar(MakeArrayView(ContainerHeaderBuffer.ValueOrDie().Data(), ContainerHeaderBuffer.ValueOrDie().DataSize()));
		FIoContainerHeader ContainerHeader;
		Ar << ContainerHeader;

		TArrayView<FFilePackageStoreEntry> StoreEntries(reinterpret_cast<FFilePackageStoreEntry*>(ContainerHeader.StoreEntries.GetData()), ContainerHeader.PackageIds.Num());
		TArrayView<FFilePackageStoreEntry> OptionalStoreEntries(reinterpret_cast<FFilePackageStoreEntry*>(ContainerHeader.OptionalSegmentStoreEntries.GetData()), ContainerHeader.OptionalSegmentPackageIds.Num());

		int32 PackageIndex = 0;
		for (FFilePackageStoreEntry& ContainerEntry : StoreEntries)
		{
			const FPackageId& PackageId = ContainerHeader.PackageIds[PackageIndex++];

			FPackageInfo& PackageInfo = PackageMap.FindOrAdd(PackageId);
			PackageInfo.PackageData.Insert(FPackageDataInfo(Reader), 0);
			FPackageDataInfo& PackageDataInfo = PackageInfo.PackageData[0];
			FPackageHeaderData& PackageHeader = PackageDataInfo.PackageHeader;

			PackageHeader.ImportedPackages = TArrayView<FPackageId>(ContainerEntry.ImportedPackages.Data(), ContainerEntry.ImportedPackages.Num());
			PackageHeader.ShaderMapHashes = TArrayView<FSHAHash>(ContainerEntry.ShaderMapHashes.Data(), ContainerEntry.ShaderMapHashes.Num());
			PackageHeader.ExportCount = ContainerEntry.ExportCount;
			PackageHeader.ExportBundleCount = ContainerEntry.ExportBundleCount;
			
			PackageIdsInThisContainer.Add(PackageId);
		}

		int32 OptionalPackageIndex = 0;
		for (FFilePackageStoreEntry& ContainerEntry : OptionalStoreEntries)
		{
			const FPackageId& PackageId = ContainerHeader.OptionalSegmentPackageIds[OptionalPackageIndex++];

			FPackageInfo& PackageInfo = PackageMap.FindOrAdd(PackageId);
			PackageInfo.bIsOptional = true;
			PackageInfo.PackageData.Insert(FPackageDataInfo(Reader), 0);
			FPackageDataInfo& PackageDataInfo = PackageInfo.PackageData[0];
			FPackageHeaderData& PackageHeader = PackageDataInfo.PackageHeader;
			
			PackageHeader.ImportedPackages = TArrayView<FPackageId>(ContainerEntry.ImportedPackages.Data(), ContainerEntry.ImportedPackages.Num());
			PackageHeader.ShaderMapHashes = TArrayView<FSHAHash>(ContainerEntry.ShaderMapHashes.Data(), ContainerEntry.ShaderMapHashes.Num());
			PackageHeader.ExportCount = ContainerEntry.ExportCount;
			PackageHeader.ExportBundleCount = ContainerEntry.ExportBundleCount;
			
			OptionalPackageIdsInThisContainer.Add(PackageId);
		}
	}

	TArray<FPackageId> PackageIdsToRemove;
	// Iterate package chunks from the header
	for ( const FPackageId& PackageId : PackageIdsInThisContainer )
	{
		// Optional chunk has index 1, required one has index 0
		const FIoChunkId ChunkId = CreateIoChunkId( PackageId.Value(), 0, EIoChunkType::ExportBundleData );
		
		TIoStatusOr<FIoStoreTocChunkInfo> ChunkInfo = Reader->GetChunkInfo( ChunkId );
		TIoStatusOr<FIoBuffer> PackageBuffer = Reader->Read( ChunkId, FIoReadOptions() );
		if (!PackageBuffer.IsOk())
		{
			// should only happens for patch containers
			PackageIdsToRemove.Add(PackageId);
			continue;
		}

		FPackageMapExportBundleEntry* ExportBundleEntry = ReadExportBundleData( PackageId, ChunkInfo.ValueOrDie(), PackageBuffer.ValueOrDie() );
	}

	for (const FPackageId& PackageId : PackageIdsToRemove)
	{
		PackageIdsInThisContainer.Remove(PackageId);
	}

	// Iterate optional packages from the header
	for ( const FPackageId& PackageId : OptionalPackageIdsInThisContainer )
	{
		// Optional chunk has index 1, required one has index 0
		const FIoChunkId ChunkId = CreateIoChunkId( PackageId.Value(), 1, EIoChunkType::ExportBundleData );
		
		TIoStatusOr<FIoStoreTocChunkInfo> ChunkInfo = Reader->GetChunkInfo( ChunkId );
		TIoStatusOr<FIoBuffer> PackageBuffer = Reader->Read( ChunkId, FIoReadOptions() );
		check( PackageBuffer.IsOk() );
		
		FPackageMapExportBundleEntry* ExportBundleEntry = ReadExportBundleData( PackageId, ChunkInfo.ValueOrDie(), PackageBuffer.ValueOrDie() );
	}

	FPackageContainerMetadata& Metadata = ContainerMetadata.FindOrAdd( Reader->GetContainerId() );

	Metadata.PackagesInContainer = PackageIdsInThisContainer;
	Metadata.OptionalPackagesInContainer = OptionalPackageIdsInThisContainer;
}

bool FIoStorePackageMap::FindPackageContainerMetadata(FIoContainerId ContainerId, FPackageContainerMetadata& OutMetadata) const
{
	if ( const FPackageContainerMetadata* Metadata = ContainerMetadata.Find( ContainerId ) )
	{
		OutMetadata = *Metadata;
		return true;
	}
	return false;
}

bool FIoStorePackageMap::FindPackageHeader(const FPackageId& PackageId, FPackageHeaderData& OutPackageHeader) const
{
	if ( const FPackageInfo* PackageInfo = PackageMap.Find( PackageId ) )
	{
		check( !PackageInfo->PackageData.IsEmpty() );
		OutPackageHeader = PackageInfo->PackageData[0].PackageHeader;
		return true;
	}
	return false;
}

FName FIoStorePackageMap::FindPackageName(const FPackageId& PackageId) const
{
	if ( const FPackageInfo* PackageInfo = PackageMap.Find( PackageId ) )
	{
		check(!PackageInfo->PackageData.IsEmpty() );
		return PackageInfo->PackageData[0].ExportBundleEntry.PackageName;
	}
	return NAME_None;
}

bool FIoStorePackageMap::FindScriptObject(const FPackageObjectIndex& Index, FPackageMapScriptObjectEntry& OutMapEntry) const
{
	check( Index.IsScriptImport() );
	if ( const FPackageMapScriptObjectEntry* Entry = ScriptObjectMap.Find( Index ) )
	{
		OutMapEntry = *Entry;
		return true;
	}
	return false;
}

bool FIoStorePackageMap::FindExportBundleData(const FPackageId& PackageId, FPackageMapExportBundleEntry& OutExportBundleEntry) const
{
	if (const FPackageInfo* PackageInfo = PackageMap.Find(PackageId))
	{
		check(!PackageInfo->PackageData.IsEmpty() );
		// need to find first not null entry
		for (const FPackageDataInfo& PackageData : PackageInfo->PackageData)
		{
			if (!PackageData.ExportBundleEntry.PackageName.IsNone())
			{
				OutExportBundleEntry = PackageData.ExportBundleEntry;
				return true;
			}
		}
	}
	return false;
}

bool FIoStorePackageMap::FindExportBundleDataAndReader(const FPackageId& PackageId, FPackageMapExportBundleEntry& OutExportBundleEntry, TSharedPtr<FIoStoreReader>& OutReader) const
{
	if (const FPackageInfo* PackageInfo = PackageMap.Find(PackageId))
	{
		check(!PackageInfo->PackageData.IsEmpty());
		// need to find first not null entry
		for (const FPackageDataInfo& PackageData : PackageInfo->PackageData)
		{
			if (!PackageData.ExportBundleEntry.PackageName.IsNone())
			{
				OutExportBundleEntry = PackageData.ExportBundleEntry;
				OutReader = PackageData.Reader;
				return true;
			}
		}
	}
	return false;
}

bool FIoStorePackageMap::FindPackageInfo(const FPackageId& PackageId, FPackageInfo& OutPackageInfo) const
{
	if (const FPackageInfo* PackageInfo = PackageMap.Find(PackageId))
	{
		OutPackageInfo = *PackageInfo;
		return true;
	}
	return false;
}

void FIoStorePackageMap::ReadScriptObjects(const FIoBuffer& ChunkBuffer)
{
	FLargeMemoryReader ScriptObjectsArchive(ChunkBuffer.Data(), ChunkBuffer.DataSize());
	TArray<FDisplayNameEntryId> GlobalNameMap = LoadNameBatch(ScriptObjectsArchive);

	int32 NumScriptObjects = 0;
	ScriptObjectsArchive << NumScriptObjects;

	const FScriptObjectEntry* ScriptObjectEntries = reinterpret_cast<const FScriptObjectEntry*>(ChunkBuffer.Data() + ScriptObjectsArchive.Tell());

	for (int32 ScriptObjectIndex = 0; ScriptObjectIndex < NumScriptObjects; ScriptObjectIndex++)
	{
		const FScriptObjectEntry& ScriptObjectEntry = ScriptObjectEntries[ScriptObjectIndex];
		FMappedName MappedName = ScriptObjectEntry.Mapped;
		check(MappedName.IsGlobal());
		
		FPackageMapScriptObjectEntry& ScriptObject = ScriptObjectMap.FindOrAdd( ScriptObjectEntry.GlobalIndex );
		ScriptObject.ScriptObjectIndex = ScriptObjectEntry.GlobalIndex;
		ScriptObject.ObjectName = MappedName.ResolveName(GlobalNameMap);
		ScriptObject.OuterIndex = ScriptObjectEntry.OuterIndex;
		ScriptObject.CDOClassIndex = ScriptObjectEntry.CDOClassIndex;
	}
}

FPackageLocalObjectRef FIoStorePackageMap::ResolvePackageLocalRef( const FPackageObjectIndex& PackageObjectIndex, const TArrayView<const FPackageId>& ImportedPackages, const TArrayView<const uint64>& ImportedPublicExportHashes )
{
	FPackageLocalObjectRef Result{};

	if ( PackageObjectIndex.IsExport() )
	{
		Result.bIsExportReference = true;
		Result.ExportIndex = PackageObjectIndex.ToExport();
	}
	else if ( PackageObjectIndex.IsImport() )
	{
		Result.bIsImport = true;

		if ( PackageObjectIndex.IsScriptImport() )
		{
			Result.Import.ScriptImportIndex = PackageObjectIndex;
			Result.Import.bIsScriptImport = true;
		}
		else if ( PackageObjectIndex.IsPackageImport() )
		{
			Result.Import.PackageExportKey = FPublicExportKey::FromPackageImport( PackageObjectIndex, ImportedPackages, ImportedPublicExportHashes );
			Result.Import.bIsPackageImport = true;
		}
		else
		{
			check( PackageObjectIndex.IsNull() );
			Result.Import.bIsNullImport = true;
		}
	}
	else
	{
		check( PackageObjectIndex.IsNull() );
		Result.bIsNull = true;
	}
	return Result;
}

FPackageMapExportBundleEntry* FIoStorePackageMap::ReadExportBundleData( const FPackageId& PackageId, const FIoStoreTocChunkInfo& ChunkInfo, const FIoBuffer& ChunkBuffer )
{
	const uint8* PackageSummaryData = ChunkBuffer.Data();
	const FZenPackageSummary* PackageSummary = reinterpret_cast<const FZenPackageSummary*>(PackageSummaryData);

	const TArrayView<const uint8> PackageHeaderDataView(PackageSummaryData + sizeof(FZenPackageSummary), PackageSummary->HeaderSize - sizeof(FZenPackageSummary));
	FMemoryReaderView PackageHeaderDataReader(PackageHeaderDataView);

	TOptional<FZenPackageVersioningInfo> VersioningInfo;
	EZenPackageVersion ZenPackageVersion = DefaultZenPackageVersion;
	
	if (PackageSummary->bHasVersioningInfo)
	{
		PackageHeaderDataReader << VersioningInfo.Emplace();
		ZenPackageVersion = VersioningInfo->ZenVersion;
	}
	// Load name map
	TArray<FDisplayNameEntryId> PackageNameMap = LoadNameBatch(PackageHeaderDataReader);
	
	// Load data resource table
	TArray<FBulkDataMapEntry> ResultBulkDataEntries;
	if ( ZenPackageVersion >= EZenPackageVersion::DataResourceTable )
	{
		int64 BulkDataMapSize = 0;
		PackageHeaderDataReader << BulkDataMapSize;
		
		const uint8* BulkDataMapData = PackageSummaryData + sizeof(FZenPackageSummary) + PackageHeaderDataReader.Tell();
		TArrayView<const FBulkDataMapEntry> BulkDataEntries = MakeArrayView(reinterpret_cast<const FBulkDataMapEntry*>(BulkDataMapData), BulkDataMapSize / sizeof(FBulkDataMapEntry));

		ResultBulkDataEntries.Append( BulkDataEntries );
	}
	
	const FName PackageName = PackageSummary->Name.ResolveName(PackageNameMap);

	// Find package header to resolve imported package IDs
	FPackageInfo& PackageInfo = PackageMap.FindChecked( PackageId );
	check( !PackageInfo.PackageData.IsEmpty() );
	const FPackageHeaderData& PackageHeader = PackageInfo.PackageData[0].PackageHeader;
	
	// Construct package data
	FPackageMapExportBundleEntry& PackageData = PackageInfo.PackageData[0].ExportBundleEntry;
	PackageData.PackageFilename = ChunkInfo.FileName;
	PackageData.PackageName = PackageName;
	PackageData.PackageFlags = PackageSummary->PackageFlags;
	PackageData.VersioningInfo = VersioningInfo;
	PackageData.PackageChunkId = ChunkInfo.Id;
	PackageData.BulkDataResourceTable = ResultBulkDataEntries;

	// get rid of standard filename prefix
	PackageData.PackageFilename.RemoveFromStart( TEXT("../../../") );

	// Save name map
	PackageData.NameMap.Empty(PackageNameMap.Num());
	PackageData.NameMap.AddZeroed(PackageNameMap.Num());

	for ( int32 i = 0; i < PackageNameMap.Num(); i++ )
	{
		PackageData.NameMap[i] = PackageNameMap[ i ].ToName( NAME_NO_NUMBER_INTERNAL );
	}

	/** Public export hashes for each import map entry in this package. */
	TArrayView<const uint64> ImportedPublicExportHashes = MakeArrayView<const uint64>(reinterpret_cast<const uint64*>(PackageSummaryData + PackageSummary->ImportedPublicExportHashesOffset), (PackageSummary->ImportMapOffset - PackageSummary->ImportedPublicExportHashesOffset) / sizeof(uint64));

	// Resolve import map now
	const FPackageObjectIndex* ImportMap = reinterpret_cast<const FPackageObjectIndex*>(PackageSummaryData + PackageSummary->ImportMapOffset);
	PackageData.ImportMap.SetNum((PackageSummary->ExportMapOffset - PackageSummary->ImportMapOffset) / sizeof(FPackageObjectIndex));
	
	for (int32 ImportIndex = 0; ImportIndex < PackageData.ImportMap.Num(); ++ImportIndex)
	{
		const FPackageObjectIndex& ImportMapEntry = ImportMap[ImportIndex];
		FPackageMapImportEntry& PackageMapImport = PackageData.ImportMap[ImportIndex];
		
		if ( ImportMapEntry.IsScriptImport() )
		{
			PackageMapImport.bIsScriptImport = true;
			PackageMapImport.ScriptImportIndex = ImportMapEntry;
		}
		else if ( ImportMapEntry.IsPackageImport() )
		{
			PackageMapImport.bIsPackageImport = true;
			PackageMapImport.PackageExportKey = FPublicExportKey::FromPackageImport( ImportMapEntry, PackageHeader.ImportedPackages, ImportedPublicExportHashes );
		}
		else
		{
			check( ImportMapEntry.IsNull() );
			PackageMapImport.bIsNullImport = true;
		}
	}
	
	const FExportMapEntry* ExportMap = reinterpret_cast<const FExportMapEntry*>(PackageSummaryData + PackageSummary->ExportMapOffset);
	PackageData.ExportMap.SetNum( PackageHeader.ExportCount );
	
	for (int32 ExportIndex = 0; ExportIndex < PackageData.ExportMap.Num(); ++ExportIndex)
	{
		const FExportMapEntry& ExportMapEntry = ExportMap[ ExportIndex ];
		FPackageMapExportEntry& ExportData = PackageData.ExportMap[ ExportIndex ];

		ExportData.ObjectName = ExportMapEntry.ObjectName.ResolveName( PackageNameMap );
		ExportData.FilterFlags = ExportMapEntry.FilterFlags;
		ExportData.ObjectFlags = ExportMapEntry.ObjectFlags;
		ExportData.PublicExportHash = ExportMapEntry.PublicExportHash;

		ExportData.OuterIndex = ResolvePackageLocalRef( ExportMapEntry.OuterIndex, PackageHeader.ImportedPackages, ImportedPublicExportHashes );
		ExportData.ClassIndex = ResolvePackageLocalRef( ExportMapEntry.ClassIndex, PackageHeader.ImportedPackages, ImportedPublicExportHashes );
		ExportData.SuperIndex = ResolvePackageLocalRef( ExportMapEntry.SuperIndex, PackageHeader.ImportedPackages, ImportedPublicExportHashes );
		ExportData.TemplateIndex = ResolvePackageLocalRef( ExportMapEntry.TemplateIndex, PackageHeader.ImportedPackages, ImportedPublicExportHashes );

		ExportData.SerialDataSize = ExportMapEntry.CookedSerialSize;
		ExportData.SerialDataOffset = INDEX_NONE;
	}

	// Read export bundles
	const FExportBundleHeader* ExportBundleHeaders = reinterpret_cast<const FExportBundleHeader*>(PackageSummaryData + PackageSummary->GraphDataOffset);
	const FExportBundleEntry* ExportBundleEntries = reinterpret_cast<const FExportBundleEntry*>(PackageSummaryData + PackageSummary->ExportBundleEntriesOffset);
	uint64 CurrentExportOffset = PackageSummary->HeaderSize;
	
	for ( int32 ExportBundleIndex = 0; ExportBundleIndex < PackageHeader.ExportBundleCount; ExportBundleIndex++ )
	{
		TArray<FExportBundleEntry>& ExportBundles = PackageData.ExportBundles.AddDefaulted_GetRef();
		const FExportBundleHeader* ExportBundle = ExportBundleHeaders + ExportBundleIndex;
		
		const FExportBundleEntry* BundleEntry = ExportBundleEntries + ExportBundle->FirstEntryIndex;
		const FExportBundleEntry* BundleEntryEnd = BundleEntry + ExportBundle->EntryCount;
		check(BundleEntry <= BundleEntryEnd);
		
		while (BundleEntry < BundleEntryEnd)
		{
			ExportBundles.Add( *BundleEntry );
			
			if (BundleEntry->CommandType == FExportBundleEntry::ExportCommandType_Serialize)
			{
				FPackageMapExportEntry& Export = PackageData.ExportMap[ BundleEntry->LocalExportIndex ];
				Export.SerialDataOffset = CurrentExportOffset;
				CurrentExportOffset += Export.SerialDataSize;
			}
			BundleEntry++;
		}
	}

	// Read arcs, they are needed to create a list of preload dependencies for this package
	const uint64 ExportBundleHeadersSize = sizeof(FExportBundleHeader) * PackageHeader.ExportBundleCount;
	const uint64 ArcsDataOffset = PackageSummary->GraphDataOffset + ExportBundleHeadersSize;
	const uint64 ArcsDataSize = PackageSummary->HeaderSize - ArcsDataOffset;

	FMemoryReaderView ArcsAr(MakeArrayView<const uint8>(PackageSummaryData + ArcsDataOffset, ArcsDataSize));

	int32 InternalArcsCount = 0;
	ArcsAr << InternalArcsCount;

	for ( int32 Idx = 0; Idx < InternalArcsCount; Idx++ )
	{
		FPackageMapInternalDependencyArc& InternalArc = PackageData.InternalArcs.AddDefaulted_GetRef();
		ArcsAr << InternalArc.FromExportBundleIndex;
		ArcsAr << InternalArc.ToExportBundleIndex;
	}

	for ( int32 ImportPackageIndex = 0; ImportPackageIndex < PackageHeader.ImportedPackages.Num(); ImportPackageIndex++ )
	{
		int32 ExternalArcsCount = 0;
		ArcsAr << ExternalArcsCount;

		for ( int32 Idx = 0; Idx < ExternalArcsCount; Idx++ )
		{
			FPackageMapExternalDependencyArc& ExternalArc = PackageData.ExternalArcs.AddDefaulted_GetRef();
			ArcsAr << ExternalArc.FromImportIndex;
			uint8 FromCommandType = 0;
			ArcsAr << FromCommandType;
			ExternalArc.FromCommandType = static_cast<FExportBundleEntry::EExportCommandType>(FromCommandType);
			ArcsAr << ExternalArc.ToExportBundleIndex;
		}
	}
	return &PackageData;
}