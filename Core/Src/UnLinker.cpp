#include "Core.h"
#include "UnLinker.h"

FGenerationInfo::FGenerationInfo(INT InExportCount, INT InNameCount)
: ExportCount(InExportCount), NameCount(InNameCount)
{}

FPackageFileSummary::FPackageFileSummary()
:	Generations(),		Tag(0)
,	FileVersion(0),		PackageFlags(0)
,	NameCount(0),		NameOffset(0)
,	ExportCount(0),		ExportOffset(0)
,	ImportCount(0),		ImportOffset(0)
,	Guid(0, 0, 0, 0)
{}

ULinker::ULinker( UObject* InRoot, const TCHAR* InFilename )
:	LinkerRoot(InRoot),	Summary(), NameMap(), ImportMap(), ExportMap()
,	Success(123456), Filename(InFilename), _ContextFlags(0)
{
	guard(ULinker::ULinker);
	
	check(LinkerRoot);
	check(InFilename);
	
	if ( GIsEditor )
        _ContextFlags |= RF_LoadForEdit;
		
    if ( GIsClient )
        _ContextFlags |= RF_LoadForClient;
		
    if ( GIsServer )
        _ContextFlags |= RF_LoadForServer;
	
	unguard;
}


void ULinker::Serialize( FArchive& Ar )
{
	guard(ULinker::Serialize);
	
	UObject::Serialize(Ar);
	ImportMap.CountBytes(Ar);
	ExportMap.CountBytes(Ar);
	
	Ar << NameMap << LinkerRoot;
	
	for( INT i = 0; i < ExportMap.Num(); ++i )
		Ar << ExportMap(i).ObjectName;
	
	for( INT i = 0; i < ImportMap.Num(); ++i )
	{
		FObjectImport &Imp = ImportMap(i);
		Ar << Imp.SourceLinker;
		Ar << Imp.ClassPackage << Imp.ClassName;
	}
	
	unguard;
}


FString ULinker::GetImportFullName( INT i )
{
	guard(ULinker::GetImportFullName);
	
	FString PathName;
	
	for( INT LinkerIndex = -1 -i; LinkerIndex; LinkerIndex = ImportMap(-1 -LinkerIndex).PackageIndex)
	{
		if (LinkerIndex != (-1 -i) )
			PathName = TEXT(".") + PathName;
		
		PathName = FString(*ImportMap(-1 -LinkerIndex).ObjectName) + PathName;
	}
	
	return FString(*ImportMap(i).ClassName) + TEXT(" ") + PathName;
	
	unguard;
}


FString ULinker::GetExportFullName( INT i, const TCHAR* FakeRoot )
{
	guard(ULinker::GetExportFullName);
	
	FString PathName;
	
	for( INT LinkerIndex = 1 + i; LinkerIndex; LinkerIndex = ExportMap(-1 + LinkerIndex).PackageIndex)
	{
		if (LinkerIndex != (1 + i) )
			PathName = TEXT(".") + PathName;
		
		PathName = FString(*ExportMap(-1 + LinkerIndex).ObjectName) + PathName;
	}
	
	if (!FakeRoot)
		FakeRoot = LinkerRoot->GetPathName();
	
	FName ObjName;
	INT Index = ExportMap(i).ClassIndex;
	if (Index == 0)
		ObjName = NAME_Class;
	else if (Index < 0)
		ObjName = ImportMap(-1 -Index).ObjectName;
	else if (Index > 0)
		ObjName = ExportMap(-1 +Index).ObjectName;
	
	return FString(*ObjName) + TEXT(" ") + FakeRoot + TEXT(".") + PathName;
	
	unguard;
}


ULinkerLoad::ULinkerLoad( UObject* InParent, const TCHAR* InFilename, DWORD InLoadFlags )
: LazyLoaders(), LoadFlags(InLoadFlags)
{
	guard(ULinkerLoad::ULinkerLoad);
	
	Loader = GFileManager->CreateFileReader(InFilename, 0, GError);
	
	if (!Loader)
        appThrowf( LocalizeError("OpenFailed", TEXT("Core")) );
	
	for (INT i = 0; i < GObjLoaders.Num(); ++i )
    {
        if ( GetLoader(i)->LinkerRoot == LinkerRoot )
            appThrowf(LocalizeError("LinkerExists", TEXT("Core")), LinkerRoot->GetName());
    }
	
	GWarn->StatusUpdatef(0, 0, LocalizeProgress("Loading", TEXT("Core")), *Filename);
	
    ArVer = PACKAGE_FILE_VERSION;
    ArLicenseeVer = PACKAGE_FILE_VERSION_LICENSEE;
    ArIsPersistent = 1;
    ArIsLoading = 1;
    ArForEdit = GIsEditor;
    ArForClient = 1;
    ArForServer = 1;
	
	(*this) << Summary;
	
	ArVer = Summary.GetFileVersion();
    ArLicenseeVer = Summary.GetFileVersionLicensee();
	
	if (Cast<UPackage>(LinkerRoot))
		Cast<UPackage>(LinkerRoot)->PackageFlags = Summary.PackageFlags;
		
    if ( Summary.Tag != PACKAGE_FILE_TAG )
    {
        GWarn->Logf( LocalizeError("BinaryFormat", TEXT("Core")), *Filename);
        appThrowf(LocalizeError("Aborted", TEXT("Core")));
    }
	
    if ( Summary.GetFileVersion() < PACKAGE_MIN_VERSION )
    {
        if ( !GWarn->YesNof( LocalizeQuery("OldVersion", TEXT("Core")), *Filename ) )
			appThrowf(LocalizeError("Aborted", TEXT("Core")));
    }
	
	ImportMap.Empty( Summary.ImportCount );
	ExportMap.Empty( Summary.ExportCount );

    if ( Summary.NameCount > 0 )
    {
        Seek(Summary.NameOffset);
		
        for ( INT i = 0; i < Summary.NameCount; ++i )
        {
			FNameEntry Entry;
			
			(*this) << Entry;
			
			FName FN;
            if ( _ContextFlags & Entry.Flags )
                FN = FName(Entry.Name);
            else
                FN = FName(NAME_None);
			
			new(NameMap)FName(FN);
        }
    }
	
    if ( Summary.ImportCount > 0 )
    {
        Seek(Summary.ImportOffset);
		
		for ( INT i = 0; i < Summary.ImportCount; ++i )
		{
			FObjectImport *Imp = new(ImportMap)FObjectImport;
			(*this) << *Imp;
		}
    }
	
	if ( Summary.ExportCount > 0 )
    {
        Seek(Summary.ExportOffset);
		
		for ( INT i = 0; i < Summary.ExportCount; ++i )
		{
			FObjectExport *Exp = new(ExportMap)FObjectExport;
			
			Exp->_Object = NULL;
			Exp->_iHashNext = -1;
			
			(*this) << *Exp;
		}
    }
	
	for ( INT i = 0; i < 256; ++i )
        ExportHash[i] = -1;
	
	for ( INT i = 0; i < ExportMap.Num(); ++i )
    {
		FName Pkg = GetExportClassPackage(i);
		if (Pkg == NAME_UnrealShare)
			Pkg = NAME_UnrealI;
		
		BYTE Idx = Pkg.GetIndex() * 0x1f + GetExportClassName(i).GetIndex() * 7 + ExportMap(i).ObjectName.GetIndex();
		
		ExportMap(i)._iHashNext = ExportHash[Idx];
		ExportHash[Idx] = i; 
    }
	
	GObjLoaders( GObjLoaders.Add() ) = this;
	
    if ( LoadFlags >= 0 )
        Verify();
		
    Success = 1;
	
	unguard;
}


void ULinkerLoad::Verify()
{
	guard(ULinkerLoad::Verify);
	
	if (!Verified)
	{
		if ( Cast<UPackage>(LinkerRoot) )
			Cast<UPackage>(LinkerRoot)->PackageFlags &= ~PKG_BrokenLinks;
		
		for ( INT i = 0; i < Summary.ImportCount; ++i )
			VerifyImport(i);
	}
	
	Verified = true;
	
	unguard;
}


FName ULinkerLoad::GetExportClassPackage( INT i )
{
	guard(ULinkerLoad::GetExportClassPackage);
	
	INT Idx = ExportMap(i).ClassIndex;

	if ( Idx < 0 )
		return ImportMap( -1 - ImportMap(-1 -Idx).PackageIndex ).ObjectName;
	else if ( Idx > 0 )
		return LinkerRoot->GetFName();
	else
		return NAME_Core;

	unguard;
}


FName ULinkerLoad::GetExportClassName( INT i )
{
	guard(ULinkerLoad::GetExportClassName);
	
	INT Idx = ExportMap(i).ClassIndex;

	if ( Idx < 0 )
		return ImportMap( -1 - Idx ).ObjectName;
	else if ( Idx > 0 )
		return ExportMap( -1 + Idx ).ObjectName;
	else
		return NAME_Class;

	unguard;
}



void ULinkerLoad::Serialize( FArchive& Ar )
{
	guard(ULinkerLoad::Serialize);
	
	ULinker::Serialize(Ar);
	
	LazyLoaders.CountBytes(Ar);

	unguard;
}


void ULinkerLoad::Seek( INT InPos )
{
	guard(ULinkerLoad::Seek);
	
	Loader->Seek(InPos);
	
	unguard;
}