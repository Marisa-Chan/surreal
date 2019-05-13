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
                appThrowf(LocalizeError("ConfigNotFound", TEXT("Core")), InName);

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
	guard(UObject::CacheDrivers);
	
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


UObject::UObject()
{
	guard(UObject::UObject);
	unguard;
}


UObject::UObject( const UObject& Src )
{
	guard(UObject::UObject);
	
	checkSlow(&Src);

    if ( Src.Class != Class )
        GError->Logf(TEXT("Attempt to copy-construct %s from %s"), GetFullName(), Src.GetFullName());
	
	unguard;
}


UObject::UObject( ENativeConstructor, UClass* InClass, const TCHAR* InName, const TCHAR* InPackageName, DWORD InFlags )
{
	guard(UObject::UObject);
	
	Index = -1;
    _LinkerIndex = -1;
    Name = FName(NAME_None);
    ObjectFlags = InFlags | RF_Native;
    HashNext = NULL;
    StateFrame = NULL;
    _Linker = NULL;
    Outer = NULL;
    Class = InClass;
	
	checkSlow(!GObjNoRegister);
	
    Name = FName((EName)(NAME_INDEX)InName); //CHECKME WTF
    Outer = (UObject *)InPackageName;  //CHECKME WTF
    _LinkerIndex = (INT)GAutoRegister;  //CHECKME WTF
    GAutoRegister = this;
	
    if ( GObjInitialized && Class == UObject::StaticClass() )
        Register();
		
	unguard;
}


UObject::UObject( EStaticConstructor, const TCHAR* InName, const TCHAR* InPackageName, DWORD InFlags )
{
	guard(UObject::UObject);
	
	Index = -1;
    _LinkerIndex = -1;
    Name = FName(NAME_None);
    HashNext = NULL;
	
    ObjectFlags = InFlags | RF_Native;
    Outer = (UObject *)InPackageName;  //CHECKME WTF
    Name = FName((EName)(NAME_INDEX)InName); //CHECKME WTF

    StateFrame = NULL;
    _Linker = NULL;

    if ( !GObjInitialized )
    {
        _LinkerIndex = (INT)GAutoRegister;  //CHECKME WTF
        GAutoRegister = this;
    }
			
	unguard;
}


UObject::UObject( EInPlaceConstructor, UClass* InClass, UObject* InOuter, FName InName, DWORD InFlags )
{
	guard(UObject::UObject);
	StaticAllocateObject(InClass, InOuter, InName, InFlags, NULL, GError, this );
	unguard;
}


UObject& UObject::operator=( const UObject& Src)
{
	guard(UObject::operator=);
	checkSlow(&Src);
	
    if ( Src.Class != Class )
        GError->Logf( TEXT("Attempt to assign %s from %s"), GetFullName(), Src.GetFullName());
	
	return *this;
	unguard;
}


void UObject::StaticConstructor()
{
	guard(UObject::StaticConstructor);
	unguard;
}


UObject::~UObject()
{
	guardSlow(UObject::~UObject);

    if ( Index != -1 && GObjInitialized && !GIsCriticalError )
    {
        checkSlow(IsValid);
		
        ConditionalDestroy();
        UnhashObject(_LinkerIndex);

        GObjObjects(Index) = NULL;
		
		GObjAvailable( GObjAvailable.Add() ) = Index;
    }

    if ( StateFrame )
    {
        ::operator delete(StateFrame);
        StateFrame = NULL;
    }
	
	unguardSlow;
}


DWORD STDCALL UObject::QueryInterface( const FGuid& RefIID, void** InterfacePtr )
{
	guard(UObject::QueryInterface);
	*InterfacePtr = NULL;
	return 0;
	unguard;
}


DWORD STDCALL AddRef()
{
	guard(UObject::AddRef);
	return 0;
	unguard;
}


DWORD STDCALL Release()
{
	guard(UObject::Release);
	return 0;
	unguard;
}


void UObject::ProcessEvent( UFunction* Function, void* Params, void* Result )
{
	guard(UObject::ProcessEvent);

    if ( GIsScriptable && IsProbing(Function->Name) && !IsPendingKill() && !Function->iNative &&
	     ( !(Function->FunctionFlags & FUNC_Native) || !ProcessRemoteFunction(Function, Params, NULL) ) )
                {
                    if ( ++GScriptEntryTag == 1 )
                    {
                        GScriptCycles -= appCycles();
                    }
					
					BYTE tmp[Function->PropertiesSize];
					FFrame FF(this, Function, 0, tmp);
					
					appMemcpy(FF.Locals, Params, Function->ParmsSize);
					appMemset(&FF.Locals[Function->ParmsSize], 0, Function->PropertiesSize - Function->ParmsSize);
					
					(this->*(Function->Func))(FF, &FF.Locals[Function->ReturnValueOffset]);
                   
                    appMemcpy(Params, FF.Locals, Function->ParmsSize);
					
                    for (UProperty *prop = Function->ConstructorLink; prop; prop = prop->ConstructorLinkNext )
                    {
                        if ( prop->Offset >= Function->ParmsSize )
                            prop->DestroyValue(&FF.Locals[prop->Offset]);
                    }
					
                    if ( --GScriptEntryTag == 0 )
                    {
                        GScriptCycles += appCycles() - 34;
                    }
                }
	unguard;
}


void UObject::ProcessState( FLOAT DeltaSeconds )
{
	guard(UObject::ProcessState);
	unguard;
}


UBOOL UObject::ProcessRemoteFunction( UFunction* Function, void* Parms, FFrame* Stack )
{
	guard(UObject::ProcessRemoteFunction);
	return false;
	unguard;
}


void UObject::Modify()
{
	guard(UObject::Modify);
	if ( GUndo )
    {
        if ( ObjectFlags & RF_Transactional )
            GUndo->SaveObject(this);
    }
	unguard;
}


void UObject::PostLoad()
{
	guard(UObject::PostLoad);
	ObjectFlags |= RF_DebugPostLoad;
	unguard;
}


void UObject::Destroy()
{
	guard(UObject::Destroy);

    ObjectFlags |= RF_DebugDestroy;
	
    ExitProperties((BYTE* )this, Class);
	
    if ( GObjInitialized && !GIsCriticalError )
        GNull->Logf(NAME_DevKill, TEXT("Destroying %s"), GetFullName());

    SetLinker(NULL, -1);

    if ( Outer )
        _LinkerIndex = Outer->Index;
    else
        _LinkerIndex = 0;

	unguard;
}


void UObject::Serialize( FArchive& Ar )
{
	guard(UObject::Serialize);
	
    ObjectFlags |= RF_DebugSerialize;
		
    if ( Class != UClass::StaticClass() )
        Ar.Preload(Class);
	
    if ( (!Ar.IsLoading() && !Ar.IsSaving()) || Ar.IsTrans() )
        Ar << Name << Outer << Class;

    if ( !Ar.IsLoading() && !Ar.IsSaving() )
        Ar << (UObject *&)_Linker;

    if ( !Ar.IsTrans() )
    {
        if ( ObjectFlags & RF_HasStack )
        {
            if ( !StateFrame )
                StateFrame = new(TEXT("ObjectStateFrame")) FStateFrame(this);

            Ar << StateFrame->Node << StateFrame->StateNode;
            Ar.Serialize(&StateFrame->ProbeMask, sizeof(StateFrame->ProbeMask)); //8
			Ar.Serialize(&StateFrame->LatentAction, sizeof(StateFrame->LatentAction)); //4

            if ( StateFrame->Node )
            {
                Ar.Preload(StateFrame->Node);
				
                if ( Ar.IsSaving() )
					checkSlow( StateFrame->Code >= &StateFrame->Node->Script(0) && StateFrame->Code < &StateFrame->Node->Script(StateFrame->Node->Script.Num()) );
                
				//CheckME and below
				
				FCompactIndex tmp;
				if (StateFrame->Code)
					tmp.Value = StateFrame->Code - &StateFrame->Node->Script(0);
				else
					tmp.Value = -1;
					
                Ar << tmp;
				
                if ( tmp.Value == -1 )
                    StateFrame->Code = NULL;
				else
				{
					if ( tmp.Value < 0 || tmp.Value >= StateFrame->Node->Script.Num() )
						GError->Logf(TEXT("%s: Offset mismatch: %i %i"), GetFullName(), tmp.Value, StateFrame->Node->Script.Num());
					if ( tmp.Value == -1 )
						StateFrame->Code = NULL;
					else
						StateFrame->Code = &StateFrame->Node->Script(tmp.Value);
				}
            }
            else
            {
                StateFrame->Code = NULL;
            }
        }
        else if ( StateFrame )
        {
            ::operator delete(StateFrame);
            StateFrame = NULL;
        }
    }

    if ( Class != UClass::StaticClass() )
    {
        if ( (Ar.IsLoading() || Ar.IsSaving()) && !Ar.IsTrans() )
            Class->SerializeTaggedProperties(Ar, (BYTE *)this, Class);
        else
            Class->SerializeBin(Ar, (BYTE *)this);
    }
	
    INT cnt = Class->GetPropertiesSize();
    Ar.CountBytes(cnt, cnt);
	
	unguard;
}


EGotoState UObject::GotoState( FName State )
{
	guard(UObject::GotoState);

    if ( !StateFrame )
        return GOTOSTATE_NotFound;

    StateFrame->LatentAction = 0;
	
	FName OldStateName = NAME_None;
    if ( StateFrame->StateNode == Class )
        OldStateName = NAME_None;
    else
        OldStateName = StateFrame->StateNode->Name;
	
	UState *StateNode = NULL;
		
    FName NewState = State;
    if ( State == NAME_Auto )
    {
		for( TFieldIterator<UState> It(Class); It; ++It )
			if (It->StateFlags & STATE_Auto)
			{
				StateNode = *It;
				break;
			}
    }
    else
    {
        StateNode = FindState(State);
    }
	
    if ( StateNode )
    {
        if ( State == NAME_Auto )
            NewState = StateNode->Name;
    }
    else
    {
        StateNode = Class;
        NewState = NAME_None;
    }
	
    if ( OldStateName != NAME_None && NewState != OldStateName && IsProbing(NAME_EndState) && !(ObjectFlags & RF_InEndState) )
    {
        ObjectFlags &= ~RF_StateChanged;
		ObjectFlags |= RF_InEndState;

        ProcessEvent( FindFunctionChecked(NAME_EndState), NULL );

        ObjectFlags &= RF_InEndState;
		
        if ( ObjectFlags & RF_StateChanged )
            return GOTOSTATE_Preempted;
    }
	
    StateFrame->Node = StateNode;
    StateFrame->StateNode = StateNode;
    StateFrame->Code = 0;
    StateFrame->ProbeMask = StateNode->IgnoreMask & (StateNode->ProbeMask | Class->ProbeMask);
	
    if ( NewState == NAME_None )
        return GOTOSTATE_NotFound;
		
    if ( NewState != OldStateName && IsProbing(NAME_BeginState) )
        {
            ObjectFlags &= RF_StateChanged;
			
            ProcessEvent( FindFunctionChecked(NAME_BeginState), NULL );

            if ( ObjectFlags & RF_StateChanged )
                return GOTOSTATE_Preempted;
        }

    ObjectFlags |= RF_StateChanged;
	
    return GOTOSTATE_Success;
	
	unguard;
}


INT UObject::GotoLabel( FName Label )
{
	guard(UObject::GotoLabel);

	if (!StateFrame)
		return 0;
	
	StateFrame->LatentAction = 0;
	
    if ( Label != NAME_None )
	{            
		for ( UState *i = StateFrame->StateNode; i; i = (UState *)i->SuperField )
        {
            if ( i->LabelTableOffset != -1 )
            {
                for (DWORD *j = (DWORD *)&i->Script( i->LabelTableOffset ); j[0]; j += 2 )
                {
                    if ( Label.GetIndex() == j[0])
                    {
                        StateFrame->Node = i;
                        StateFrame->Code = &i->Script( j[1] );
                        return 1;
                    }
                }
            }
        }
	}
	
    StateFrame->Code = 0;
	return 0;
		
	unguard;
}


void UObject::InitExecution()
{
	guard(UObject::InitExecution);
	
	checkSlow(GetClass() != NULL);
	
	if ( StateFrame )
		::operator delete(StateFrame);
	
	StateFrame = new(TEXT("ObjectStateFrame")) FStateFrame(this);
	
	ObjectFlags |= RF_HasStack;		
	
	unguard;
}


void UObject::ShutdownAfterError()
{
	guard(UObject::ShutdownAfterError);
	unguard;
}


void UObject::PostEditChange()
{
	guard(UObject::PostEditChange);
	
	Modify();
	
	unguard;
}


void UObject::CallFunction( FFrame& Stack, RESULT_DECL, UFunction* Function )
{
	guard(UObject::CallFunction);

	UBOOL SkipIt = false;
	
    if ( Function->iNative )
    {
        (this->*(Function->Func))(Stack, Result);
        return;
    }

    if ( Function->FunctionFlags & FUNC_Native )
    {
		BYTE tmpParams[0x400];
        if ( !ProcessRemoteFunction(Function, tmpParams, &Stack) )
        {
            (this->*(Function->Func))(Stack, Result);
            return;
        }
        SkipIt = true;
    }
	
	BYTE tmp[Function->PropertiesSize];
	appMemset(tmp, 0, Function->PropertiesSize);
	FFrame NewStack(this, Function, 0, tmp);
	
    UProperty *Property = (UProperty *)Function->Children;
	
    struct FOutParmRec
	{
		UProperty *Property;
		BYTE *PropAddr;
	} Outs[16];

	FOutParmRec *i;
    for (i = &Outs[0]; *Stack.Code != EX_EndFunctionParms; Property = (UProperty *)Property->Next )
    {
        GPropAddr = NULL;
		Stack.Step( Stack.Object, &NewStack.Locals[Property->Offset] );
		
        if ( (Property->PropertyFlags & CPF_OutParm) && GPropAddr )
        {
            i->PropAddr = GPropAddr;
            i->Property = Property;
            ++i;
        }
    }
	
    ++Stack.Code;
	
    if ( SkipIt == 0 )
        ProcessInternal(NewStack, Result);
		
    for ( FOutParmRec *j = i - 1; j >= &Outs[0]; --j )
        j->Property->CopyCompleteValue( j->PropAddr, &NewStack.Locals[ j->Property->Offset ] );
		
    for ( UProperty *k = Function->ConstructorLink; k; k = k->ConstructorLinkNext )
        k->DestroyValue( &NewStack.Locals[ k->Offset ] );
	
	unguard;
}


UBOOL UObject::ScriptConsoleExec( const TCHAR* Cmd, FOutputDevice& Ar, UObject* Executor )
{
	guard(UObject::ScriptConsoleExec);
	
	if ( !GIsScriptable )
        return false;
	
	TCHAR MsgStr[64];
	if ( !ParseToken(Cmd, MsgStr, 64, true) )
        return false;
	
	FName InName(MsgStr, FNAME_Find);
	if (InName == NAME_None)
		return false;
	
	UFunction *Func = FindFunction(InName);
    if ( !Func || !(Func->FunctionFlags & FUNC_Exec) )
        return false;
	
	BYTE tmp[Func->ParmsSize];
	appMemset(tmp, 0, Func->ParmsSize);
	
	UBOOL Failed = false;
	INT Count = 0;
	for(TFieldIterator<UProperty> It(Func); It && (It->PropertyFlags & (CPF_ReturnParm | CPF_Parm) == CPF_Parm); ++It, ++Count )
	{
		if (!Count && Executor)
		{
			UObjectProperty *objprop = Cast<UObjectProperty>(*It);
			if (objprop)
			{
				if (Executor->IsA(objprop->PropertyClass))
				{
					*(UObject**)(&tmp[objprop->Offset]) = Executor;
					continue;
				}
			}
		}
		
		ParseNext(&Cmd);
		
		Cmd = It->ImportText(Cmd, &tmp[It->Offset], 1);
		
        if ( !Cmd )
        {
            if ( !(It->PropertyFlags & CPF_OptionalParm) )
            {
                Ar.Logf( LocalizeError("BadProperty", TEXT("Core")), FName(InName), FName(It->Name) );
				Failed = true;
            }
            break;
        }
	}
	
	if (!Failed)
		ProcessEvent(Func, tmp);
	
	for(TFieldIterator<UProperty> It(Func); It && (It->PropertyFlags & (CPF_ReturnParm | CPF_Parm) == CPF_Parm); ++It, ++Count )
	{
		It->DestroyValue( &tmp[ It->Offset ] );
	}
	
	return true;
	unguard;
}



void UObject::Register()
{
	guard(UObject::Register);
	
	checkSlow(GObjInitialized);
	
    Outer = CreatePackage(NULL, (const TCHAR*)Outer);
	Name = FName(*((const TCHAR**)&Name)) ;

    _LinkerIndex = -1;
    
    if ( !Outer )
        GError->Logf( TEXT("Autoregistered object %s is unpackaged"), GetFullName() );

    if ( Name == NAME_None )
        GError->Logf( TEXT("Autoregistered object %s has invalid name"), GetFullName() );

    if ( StaticFindObject(NULL, Outer, *Name ) )
        GError->Logf( TEXT("Autoregistered object %s already exists"), GetFullName() );

    AddObject(-1);
	
	unguard;
}


void UObject::LanguageChange()
{
	guard(UObject::LanguageChange);
	
	LoadLocalized();
	
	unguard;
}