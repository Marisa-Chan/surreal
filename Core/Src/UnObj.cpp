#include "Core.h"

//Local
static UBOOL GNoGC = false;
static INT GGarbageRefCount = 0;
static INT GExitPurge = 0;


//Static
UBOOL				UObject::GObjInitialized;	// Whether initialized.
UBOOL				UObject::GObjNoRegister;		// Registration disable.
INT					UObject::GObjBeginLoadCount;	// Count for BeginLoad multiple loads.
INT					UObject::GObjRegisterCount;  // ProcessRegistrants entry counter.
INT					UObject::GImportCount;		// Imports for EndLoad optimization.
UObject*			UObject::GObjHash[4096];		// Object hash.
UObject*			UObject::GAutoRegister;		// Objects to automatically register.
TArray<UObject*>	UObject::GObjLoaded;			// Objects that might need preloading.
TArray<UObject*>	UObject::GObjRoot;			// Top of active object graph.
TArray<UObject*>	UObject::GObjObjects;		// List of all objects.
TArray<INT>			UObject::GObjAvailable;		// Available object indices.
TArray<UObject*>	UObject::GObjLoaders;		// Array of loaders.
UPackage*			UObject::GObjTransientPkg;	// Transient package.
TCHAR				UObject::GObjCachedLanguage[32]; // Language;
TArray<UObject*>	UObject::GObjRegistrants;		// Registrants during ProcessRegistrants call.
TArray<FPreferencesInfo>	UObject::GObjPreferences; // Prefereces cache.
TArray<FRegistryObjectInfo> UObject::GObjDrivers; // Drivers cache.
TMultiMap<FName,FName>*		UObject::GObjPackageRemap; // Remap table for loading renamed packages.
TCHAR				UObject::GLanguage[64];




void UObject::AddObject( INT InIndex )
{
    guard(UObject::AddObject);

    if (InIndex == -1)
    {
        if (GObjAvailable.Num())
        {
            InIndex = GObjAvailable.Pop();
            checkSlow( GObjObjects(InIndex) == NULL );
        }
        else
        {
            InIndex = GObjObjects.Add();
        }
    }
    GObjObjects(InIndex) = this;
    Index = InIndex;
    HashObject();

    unguard;
}

void UObject::HashObject()
{
    guard(UObject::HashObject);

    INT tmp = 0;
    if (Outer)
        tmp = Outer->Index;

    INT idx = (Name.GetIndex() ^ tmp) & 0xFFF;
    HashNext = GObjHash[idx];
    GObjHash[idx] = this;

    unguard;
}

void UObject::UnhashObject( INT OuterIndex )
{
    guard(UObject::UnhashObject);

    INT Removed = 0;
    UObject **ppobj = &GObjHash[ (Name.GetIndex() ^ OuterIndex) & 0xFFF ];

    while ( *ppobj )
    {
        if ( *ppobj == this )
        {
            *ppobj = (*ppobj)->HashNext;
            ++Removed;
        }
        else
        {
            ppobj = &(*ppobj)->HashNext;
        }
    }

    checkSlow( Removed != 0 );
    checkSlow( Removed == 1 );

    unguard;
}

void UObject::SetLinker( ULinkerLoad* InLinker, INT InLinkerIndex )
{
    guard(UObject::SetLinker);

    checkSlow( _Linker->ExportMap(_LinkerIndex)._Object != NULL );
    checkSlow( _Linker->ExportMap(_LinkerIndex)._Object == this );
    _Linker = InLinker;
    _LinkerIndex = InLinkerIndex;

    unguard;
}


ULinkerLoad* UObject::GetLoader( INT i )
{
    guard(UObject::GetLoader);

    return (ULinkerLoad*)GObjLoaders(i); //CHECKME

    unguard;
}

FName UObject::MakeUniqueObjectName( UObject* Parent, UClass* Class )
{
    guard(UObject::GetLoader);

    checkSlow(Class);
    TCHAR NewBase[64];
    appStrcpy(NewBase, FName::GetEntry( Class->Name.GetIndex() )->Name);

    TCHAR *tmp = &NewBase[appStrlen(NewBase)];
    while (tmp > NewBase && appIsDigit( *(tmp - 1) ) )
        tmp--;
    *tmp = 0;

    TCHAR TempIntStr[64];
    TCHAR Result[64];

    do
    {
        appSprintf(TempIntStr, TEXT("%i"), Class->ClassUnique++);
        appStrncpy(Result, NewBase, 63 - appStrlen(TempIntStr));
        appStrcat(Result, TempIntStr);
    }
    while ( StaticFindObject(NULL, Parent, Result) );

    return FName(Result);

    unguard;
}

UBOOL UObject::ResolveName( UObject*& InPackage, const TCHAR*& InName, UBOOL Create, UBOOL Throw )
{
    guard(UObject::ResolveName);

    checkSlow(InName);

    UBOOL SystemIni = appStrnicmp(InName, TEXT("ini:"), 4) == 0;
    UBOOL SystemUsr = appStrnicmp(InName, TEXT("usr:"), 4) == 0;

    if ( (SystemIni || SystemUsr) && appStrlen(InName) <= 1023 && appStrchr(InName, '.') )
    {
        TCHAR Section[256];
        TCHAR *Key = Section;

        appStrcpy(Section, InName + 4);

        while ( appStrchr(Key, '.') )
            Key = appStrchr(Key, '.') + 1;

        checkSlow(Key == Section);

        *(Key - 1) = 0;

        if ( !GConfig->GetString(Section, Key, appStaticString1024(), 1024, (SystemIni ? TEXT("System") : TEXT("User")) ) )
        {
            if ( Throw )
                appThrowf(LocalizeError(TEXT("ConfigNotFound"), TEXT("Core")), InName);

            return false;
        }

        InName = appStaticString1024();
    }

    while ( appStrchr(InName, '.') )
    {
        TCHAR PartialName[256];
        appStrcpy(PartialName, InName);
        *appStrchr(PartialName, '.') = 0;

        if ( Create )
        {
            InPackage = CreatePackage(InPackage, PartialName);
        }
        else
        {
            UObject *fnd = FindObject<UPackage>(InPackage, PartialName, false);
            if ( !fnd )
            {
                fnd = FindObject<UObject>(InPackage, PartialName, false);
                if ( !fnd )
                    return false;
            }
            InPackage = fnd;
        }
        InName = appStrchr(InName, '.') + 1;
    }

    return true;
    unguard;
}


void UObject::SafeLoadError( DWORD LoadFlags, const TCHAR* Error, const TCHAR* Fmt, ... )
{
	guard(UObject::SafeLoadError); //CHECKME  position of va_start

    TCHAR TempStr[4096];
	va_list __varargs;

	va_start(__varargs, Fmt);
	vsprintf(TempStr, Fmt, __varargs);
	
	if ( !(LoadFlags & LOAD_Quiet) )
		GLog->Logf(NAME_Warning, TempStr);
		
	if ( LoadFlags & LOAD_Throw )
		appThrowf(TEXT("%s"), Error);
		
	if ( LoadFlags & LOAD_NoFail )
		GError->Logf(TEXT("%s"), TempStr);
		
	if ( !(LoadFlags & LOAD_NoWarn) )
		GWarn->Logf(TEXT("%s"), TempStr);

    unguard;
}


void UObject::PurgeGarbage()
{
	guard(UObject::PurgeGarbage);

    INT CountPurged = 0;
	INT CountBefore = 0;
	
	if ( GNoGC )
	{
		GLog->Logf(NAME_Log, TEXT("Not purging garbage"));
		return;
	}

	
	GLog->Logf(NAME_Log, TEXT("Purging garbage"));

    for (INT i = 0; i < GObjObjects.Num(); ++i )
	{
      UObject *obj = GObjObjects(i);
      if ( obj )
      {
        ++CountBefore;
		if ( obj->ObjectFlags & RF_Unreachable )
          {
            if ( !(obj->ObjectFlags & RF_Native) || GExitPurge )
            {
              debugfSlow(NAME_DevGarbage, TEXT("Garbage collected object %i: %s"), i, obj->GetFullName() );
              obj->ConditionalDestroy();
              ++CountPurged;
            }
          }
      }
    }
	
    for ( INT i = 0; i < GObjObjects.Num(); ++i )
    {
      UObject *obj = GObjObjects(i);
      if ( obj )
      {
        if ( obj->ObjectFlags & RF_Unreachable )
        {
          if ( !(obj->ObjectFlags & RF_Native) )
          {
            FName DeleteName = obj->Name;
			delete obj;
          }
        }
      }
    }
	
    for ( INT i = 0; i < FName::GetMaxNames(); ++i )
    {
      FNameEntry* name = FName::GetEntry(i);
      if ( name && (name->Flags & (RF_Native | RF_Unreachable)) == RF_Unreachable )
      {
        debugfSlow(NAME_DevGarbage, TEXT("Garbage collected name %i: %s"), i, name->Name);
        FName::DeleteEntry(i);
      }
    }
	
    GLog->Logf(TEXT("Garbage: objects: %i->%i; refs: %i"), CountBefore, CountBefore - CountPurged, GGarbageRefCount);

    unguard;
}


void UObject::CacheDrivers( UBOOL ForceRefresh )
{
	guard(UObject::ForceRefresh);
	
	if ( !ForceRefresh && appStricmp(GObjCachedLanguage, GetLanguage()) == 0 )
		return;
	
    appStrcpy(GObjCachedLanguage, GetLanguage());
	
	GObjPreferences.Empty();
	GObjDrivers.Empty();
	
	for(INT i = 0; i < GSys->Paths.Num(); i++)
	{
		TCHAR tmp[256];
		appSprintf(tmp, TEXT("%s%s"), appBaseDir(), *GSys->Paths(i));
		TCHAR *dir = appStrstr(tmp, TEXT("*."));
		if (dir)
		{
			appSprintf(dir, TEXT("*.int"));
			TArray<FString> files = GFileManager->FindFiles(tmp, 1, 0);
			
			for(INT j = 0; j < files.Num(); ++j)
			{
				appSprintf(tmp, TEXT("%s%s"), appBaseDir(), *files(j));
				
				TCHAR *ext = &tmp[ appStrlen(tmp) - 4 ];
				appSprintf(ext, TEXT(".%s"), GetLanguage());
				
				TCHAR conf[0x8000];
				UBOOL get = GConfig->GetSection(TEXT("Public"), conf, 0x7FFF, tmp);
				
				if (!get)
				{
					appSprintf(ext, TEXT(".int"));
					get = GConfig->GetSection(TEXT("Public"), conf, 0x7FFF, tmp);
				}
				
				if (get)
				{
					TCHAR *pc = conf;
					TCHAR *next = pc;
					while (*next)
					{
						next = &pc[ appStrlen(pc) + 1 ];
						TCHAR *eq = appStrstr(pc, TEXT("="));
						
						if ( eq )
						  {
							*eq = 0;
							eq += 1;
							if ( *eq == '(' )
							{
								*eq = 0;
								eq += 1;
							}

					
							if ( *eq && eq[appStrlen(eq) - 1] == ')' )
							  eq[ appStrlen(eq) - 1 ] = 0;
					
							if ( !appStricmp(pc, "Object") )
							{
								FRegistryObjectInfo *reg = new(GObjDrivers) FRegistryObjectInfo;

							  Parse(eq, "Name=", reg->Object);
							  Parse(eq, "Class=", reg->Class);
							  Parse(eq, "MetaClass=", reg->MetaClass);
							  Parse(eq, "Description=", reg->Description);
							  Parse(eq, "Autodetect=", reg->Autodetect);
							}
							else if ( !appStricmp(pc, "Preferences") )
							  {
								FPreferencesInfo *pref = new(GObjPreferences) FPreferencesInfo;
						
								Parse(eq, "Caption=", pref->Caption);
								Parse(eq, "Parent=", pref->ParentCaption);
								Parse(eq, "Class=", pref->Class);
								Parse(eq, "Category=", pref->Category);
								ParseUBOOL(eq, "Immediate=", pref->Immediate);
								pref->Category.SetFlags(RF_Public);
							  }                    
						  }
				  
					}
				}
			}
			
			files.Empty();
		}
	}
	
	TArray<FString> files = GFileManager->FindFiles(TEXT("Core.*"), 1, 0);
	
	for(INT i = 0; i < files.Num(); ++i)
	{
		FString tmp1;
		FString tmp2(".");
		
		FString &file = files(i);
		INT dot = file.InStr(*tmp2, true);
		
		UBOOL has = false;
		
		if (dot >= 0)
		{
			tmp1 = file.Mid(dot + tmp2.Len());
			has = true;
		}
		else
		{
			has = false;
		}
		
		tmp1 = FString(".") + tmp1;
		
		if ( !appStricmp(*tmp1, TEXT(".dll")) || !appStricmp(*tmp1, TEXT(".u")) || !appStricmp(*tmp1, TEXT(".ilk")) )
		{
			FRegistryObjectInfo *freg = new(GObjDrivers) FRegistryObjectInfo;
			freg->Object = appStrstr(*files(i), TEXT(".")) + 1;
			freg->Class = TEXT("Class");
			freg->MetaClass = ULanguage::StaticClass()->GetPathName();
		}
		
		
		
	}
	
	unguard;
}







