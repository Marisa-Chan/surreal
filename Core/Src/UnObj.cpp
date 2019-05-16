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

    if (InIndex == INDEX_NONE)
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
    guard(UObject::MakeUniqueObjectName);

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

    Index = INDEX_NONE;
    _LinkerIndex = INDEX_NONE;
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

    Index = INDEX_NONE;
    _LinkerIndex = INDEX_NONE;
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

    if ( Index != INDEX_NONE && GObjInitialized && !GIsCriticalError )
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

    SetLinker(NULL, INDEX_NONE);

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
                    tmp.Value = INDEX_NONE;

                Ar << tmp;

                if ( tmp.Value == INDEX_NONE )
                    StateFrame->Code = NULL;
                else
                {
                    if ( tmp.Value < 0 || tmp.Value >= StateFrame->Node->Script.Num() )
                        GError->Logf(TEXT("%s: Offset mismatch: %i %i"), GetFullName(), tmp.Value, StateFrame->Node->Script.Num());
                    if ( tmp.Value == INDEX_NONE )
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
        for ( UState *i = StateFrame->StateNode; i; i = i->GetSuperState() )
        {
            if ( i->LabelTableOffset != INDEX_NONE )
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

    _LinkerIndex = INDEX_NONE;

    if ( !Outer )
        GError->Logf( TEXT("Autoregistered object %s is unpackaged"), GetFullName() );

    if ( Name == NAME_None )
        GError->Logf( TEXT("Autoregistered object %s has invalid name"), GetFullName() );

    if ( StaticFindObject(NULL, Outer, *Name ) )
        GError->Logf( TEXT("Autoregistered object %s already exists"), GetFullName() );

    AddObject(INDEX_NONE);

    unguard;
}


void UObject::LanguageChange()
{
    guard(UObject::LanguageChange);

    LoadLocalized();

    unguard;
}





void UObject::AddToRoot()
{
    guard(UObject::AddToRoot);

    GObjRoot( GObjRoot.Add() ) = this;

    unguard;
}


void UObject::RemoveFromRoot()
{
    guard(UObject::RemoveFromRoot);

    GObjRoot.RemoveItem( this );

    unguard;
}


const TCHAR* UObject::GetFullName( TCHAR* Str ) const
{
    guard(UObject::GetFullName);

    if ( !Str )
        Str = appStaticString1024();

    if ( this )
    {
        appSprintf(Str, TEXT("%s "), *Name);
        GetPathName( NULL, &Str[ appStrlen(Str) ] );
    }
    else
    {
        appStrcpy(Str, TEXT("None"));
    }
    return Str;

    unguard;
}


const TCHAR* UObject::GetPathName( UObject* StopOuter, TCHAR* Str ) const
{
    guard(UObject::GetPathName);

    if ( !Str )
        Str = appStaticString1024();

    if ( this == StopOuter )
    {
        appSprintf(Str, TEXT("None"));
    }
    else
    {
        Str[0] = 0;

        if ( Outer != StopOuter )
        {
            Outer->GetPathName(StopOuter, Str);
            appStrcat(Str, TEXT("."));
        }

        appStrcat(Str, *Name);
    }
    return Str;

    unguard;
}


UBOOL UObject::IsValid()
{
    guard(UObject::IsValid);

    if ( !this )
    {
        GLog->Logf(NAME_Warning, TEXT("NULL object"));
        return false;
    }

    if ( Index < 0 || Index >= GObjObjects.Num() )
    {
        GLog->Logf(NAME_Warning, TEXT("Invalid object index %i"), Index);
        GLog->Logf(NAME_Warning, TEXT("This is: %s"), GetFullName());
        return false;
    }

    if ( ! GObjObjects(Index) )
    {
        GLog->Logf(NAME_Warning, TEXT("Empty slot"));
        GLog->Logf(NAME_Warning, TEXT("This is: %s"), GetFullName());
        return false;
    }

    if ( GObjObjects(Index) != this )
    {
        GLog->Logf(NAME_Warning, TEXT("Other object in slot"));
        GLog->Logf(NAME_Warning, TEXT("This is: %s"), GetFullName());
        GLog->Logf(NAME_Warning, TEXT("Other is: %s"), GObjObjects(Index)->GetFullName());
        return false;
    }

    return true;

    unguard;
}


void UObject::ConditionalRegister()
{
    guard(UObject::ConditionalRegister);

    if ( Index == INDEX_NONE )
    {
        INT i;
        for (i = 0; i < GObjRegistrants.Num() ; ++i )
        {
            if ( GObjRegistrants(i) == this )
                break;
        }

        check(i != GObjRegistrants.Num());

        Register();
    }

    unguard;
}


UBOOL UObject::ConditionalDestroy()
{
    guard(UObject::ConditionalDestroy);

    if ( Index == INDEX_NONE )
        return false;

    if ( ObjectFlags & RF_Destroyed )
        return false;

    ObjectFlags &= ~(RF_Destroyed | RF_DebugDestroy);
    ObjectFlags |= RF_Destroyed;

    Destroy();

    if ( !(ObjectFlags & RF_DebugDestroy) )
        GError->Logf(TEXT("%s failed to route Destroy"), GetFullName());

    return true;

    unguard;
}


void UObject::ConditionalPostLoad()
{
    guard(UObject::ConditionalPostLoad);

    if ( ObjectFlags & RF_NeedPostLoad )
    {
        ObjectFlags &= ~(RF_NeedPostLoad | RF_DebugPostLoad);

        PostLoad();

        if ( !(ObjectFlags & RF_DebugPostLoad) )
            GError->Logf(TEXT("%s failed to route PostLoad"), GetFullName());
    }

    unguard;
}


void UObject::ConditionalShutdownAfterError()
{
    guard(UObject::ConditionalShutdownAfterError);

    if ( !(ObjectFlags & RF_ErrorShutdown) )
    {
        ObjectFlags |= RF_ErrorShutdown;
        ShutdownAfterError();
    }

    unguard;
}


UBOOL UObject::IsProbing( FName ProbeName )
{
    guard(UObject::IsProbing);

    if ( ProbeName.GetIndex() >= NAME_PROBEMIN && ProbeName.GetIndex() < NAME_PROBEMAX )
    {
        if ( StateFrame )
        {
            if ( !(  StateFrame->ProbeMask & ((QWORD)1 << (ProbeName.GetIndex() - NAME_PROBEMIN) )  ) )
                return false;
        }
    }
    return true;

    unguard;
}


void UObject::Rename( const TCHAR* InName )
{
    guard(UObject::Rename);

    FName NewName;
    if ( InName )
        NewName = FName(InName);
    else
        NewName = MakeUniqueObjectName(Outer, Class);


    if ( Outer )
        UnhashObject(Outer->Index);
    else
        UnhashObject(0);

    GNull->Logf( TEXT("Renaming %s to %s"), *Name, *NewName);
    Name = NewName;

    HashObject();

    unguard;
}


UField* UObject::FindObjectField( FName InName, UBOOL Global )
{
    guard(UObject::FindObjectField);

    if ( StateFrame && StateFrame->StateNode && !Global && StateFrame->StateNode->VfHash[ InName.GetIndex() ] )
    {
        for( UField* i = StateFrame->StateNode->VfHash[ InName.GetIndex() ]; i; i = i->HashNext )
        {
            if ( i->Name == InName )
                return i;
        }
    }

    for( UField* i = Class->VfHash[ InName.GetIndex() ]; i; i = i->HashNext )
    {
        if ( i->Name == InName )
            return i;
    }

    return NULL;

    unguard;
}


UFunction* UObject::FindFunction( FName InName, UBOOL Global )
{
    guard(UObject::FindFunction);

    return Cast<UFunction>(FindObjectField(InName, Global));

    unguard;
}


UFunction* UObject::FindFunctionChecked( FName InName, UBOOL Global )
{
    guard(UObject::FindFunctionChecked);

    if ( !GIsScriptable )
        return NULL;

    UFunction* fnc = Cast<UFunction>(FindObjectField(InName, Global));

    if ( !fnc )
        GError->Logf(TEXT("Failed to find function %s in %s"), *InName, GetFullName());

    return fnc;

    unguard;
}


UState* UObject::FindState( FName InName )
{
    guard(UObject::FindState);

    return Cast<UState>(FindObjectField(InName, true));

    unguard;
}


void UObject::SaveConfig( DWORD Flags, const TCHAR* InFilename )
{
    guard(UObject::SaveConfig);

    TCHAR TempKey[256];
    appMemset(TempKey, 0, sizeof(TempKey)); //CHECKIT added this because thinking it's error - without init

    UBOOL PerObject = false;

    if ( Class->ClassFlags & CLASS_PerObjectConfig && Index != INDEX_NONE )
        PerObject = true;

    const TCHAR *Filename = InFilename;

    if ( !Filename )
    {
        if ( PerObject && Outer != GObjTransientPkg )
            Filename = *(Outer->Name);
        else
            Filename = *(Class->ClassConfigName);
    }

    for(TFieldIterator<UProperty> It(Class); It; ++It )
    {
        if ( (It->PropertyFlags & Flags) == Flags )
        {
            TCHAR Value[1024];
            //Value[0] = '\0';
            appMemset(Value, 0, sizeof(Value));

            const TCHAR *Key = *(It->Name);
            const TCHAR *Section;

            if (PerObject)
                Section = *Name;
            else
            {
                if ( It->PropertyFlags & CPF_GlobalConfig )
                    Section = It->GetOwnerClass()->GetPathName();
                else
                {
                    Section = Class->GetPathName();
                }
            }

            UArrayProperty *Array = Cast<UArrayProperty>(*It);
            UMapProperty *Map = Cast<UMapProperty>(*It);

            if (Array)
            {
                TMultiMap<FString,FString> *Sec = GConfig->GetSectionPrivate( Section, true, false, Filename);

                check(Sec);

                Sec->Remove(Key);

                FArray *Ptr = (FArray *)&((BYTE *)this)[Array->Offset];

                for(INT i = 0; i < Ptr->Num(); ++i)
                {
                    TCHAR Buffer[1024];
                    appMemset(Buffer, 0, sizeof(Buffer));

                    BYTE *pval = &((BYTE *)Ptr->GetData())[i * Array->Inner->ElementSize]; //CHECKIT
                    Array->Inner->ExportTextItem(Buffer, pval, pval, 0);
                    Sec->Add(Key, Buffer);
                }
            }
            else if (Map)
            {
                TMultiMap<FString,FString> *Sec = GConfig->GetSectionPrivate( Section, true, false, Filename);

                check(Sec);

                Sec->Remove(Key);
            }
            else
            {
                for(INT i = 0; i < It->ArrayDim; ++i)
                {
                    if ( i != 1 )
                    {
                        appSprintf(TempKey, TEXT("%s[%i]"), *(It->Name), i);
                        Key = TempKey;
                    }

                    It->ExportText(i, Value, (BYTE *)this, (BYTE *)this, 0);

                    GConfig->SetString(Section, Key, Value, Filename);
                }
            }
        }
    }

    UClass *BaseClass = Class;
    for( UClass *i = BaseClass->GetSuperClass(); i && (i->ClassFlags & CLASS_Config); i = i->GetSuperClass() )
        BaseClass = i;

    if (BaseClass)
    {
        for (TObjectIterator<UClass> It; It; ++It)
        {
            if ( It->IsChildOf(BaseClass) )
                It->GetDefaultObject()->LoadConfig(true);
        }
    }

    unguard;
}


void UObject::LoadConfig( UBOOL Propagate, UClass* InClass, const TCHAR* InFilename )
{
    guard(UObject::LoadConfig);

    char TempKey[256];
    appMemset(TempKey, 0, sizeof(TempKey)); //CHECKIT added this because thinking it's error - without init

    if ( !InClass )
        InClass = Class;

    if ( InClass->ClassFlags & CLASS_Config )
    {
        if (Propagate && InClass->GetSuperClass())
            LoadConfig(Propagate, InClass->GetSuperClass(), InFilename);

        UBOOL PerObject = false;
        if ( (InClass->ClassFlags & CLASS_PerObjectConfig) && Index != INDEX_NONE )
            PerObject = true;


        const TCHAR *Filename = InFilename;
        if ( !Filename )
        {
            if ( PerObject && Outer != GObjTransientPkg )
                Filename = *(Outer->Name);
            else
                Filename = *(InClass->ClassConfigName);
        }

        for(TFieldIterator<UProperty> It(InClass); It; ++It )
        {
            if ( It->PropertyFlags & CPF_Config )
            {
                TCHAR Value[1024];
                appMemset(Value, 0, sizeof(Value));

                const TCHAR *Section;

                if (PerObject)
                    Section = *Name;
                else
                {
                    if ( It->PropertyFlags & CPF_GlobalConfig )
                        Section = It->GetOwnerClass()->GetPathName();
                    else
                    {
                        Section = InClass->GetPathName();
                    }
                }

                const TCHAR *Key;

                if (It->ArrayDim == 1)
                    Key = *(It->Name);
                else
                    Key = TempKey;

                UArrayProperty *Array = Cast<UArrayProperty>(*It);
                UMapProperty *Map = Cast<UMapProperty>(*It);

                if (Array)
                {
                    TMultiMap<FString,FString> *Sec = GConfig->GetSectionPrivate( Section, false, true, Filename);
                    if (Sec)
                    {
                        TArray<FString> List;
                        Sec->MultiFind(Key, List);

                        FArray *Ptr = (FArray *)&((BYTE *)this)[Array->Offset];

                        Array->DestroyValue(Ptr);

                        INT idx = Ptr->AddZeroed(Array->Inner->ElementSize, List.Num());

                        for(INT i = List.Num() - 1, j = 0;  i >= 0;  --i, ++j)
                            Array->Inner->ImportText( *(List(i)), (BYTE *)Ptr->GetData() + Array->Inner->ElementSize * j, 0);
                    }
                }
                else if (Map)
                {
                    TMultiMap<FString,FString> *Sec = GConfig->GetSectionPrivate( Section, false, true, Filename);
                    if (Sec)
                    {
                        TArray<FString> List;
                        Sec->MultiFind(Key, List);
                    }
                }
                else
                {
                    for(INT i = 0; i < It->ArrayDim; ++i)
                    {
                        if ( i != 1 )
                        {
                            appSprintf(TempKey, TEXT("%s[%i]"), *(It->Name), i);
                            Key = TempKey;
                        }

                        if ( GConfig->GetString(Section, Key, Value, 1024, Filename) )
                            It->ImportText(Value, (BYTE *)this + It->Offset + i * It->ElementSize, 0);
                    }
                }

            }
        }
    }

    unguard;
}


void UObject::LoadLocalized( UBOOL Propagate, UClass* InClass )
{
    guard(UObject::LoadLocalized);

    TCHAR TempKey[256];

    if ( !InClass )
        InClass = Class;

    if ( InClass->ClassFlags & CLASS_Localized && !GIsEditor )
    {
        if ( Propagate && InClass->GetSuperClass() )
            LoadLocalized(Propagate, InClass->GetSuperClass());

        const TCHAR *PackageName;
        const TCHAR *Section;

        if ( Index == INDEX_NONE )
            PackageName = *(InClass->Outer->Name);
        else
            PackageName = *(Outer->Name);

        if ( Index == INDEX_NONE )
            Section = *(InClass->Name);
        else
            Section = *Name;

        for(TFieldIterator<UProperty> It(InClass); It; ++It )
        {
            if ( (It->PropertyFlags & CPF_Localized) )
            {
                for ( INT i = 0; i < It->ArrayDim; ++i )
                {
                    const TCHAR *Key = *(It->Name);

                    if ( It->ArrayDim != 1 )
                    {
                        appSprintf(TempKey, TEXT("%s[%i]"), *(It->Name), i);
                        Key = TempKey;
                    }

                    const TCHAR *loc = Localize(Section, Key, PackageName, NULL, true);
                    if ( loc[0] )
                        It->ImportText(loc, (BYTE *)this + It->Offset + i * It->ElementSize, 0);
                }
            }
        }
    }

    unguard;
}


void UObject::InitClassDefaultObject( UClass* InClass )
{
    guard(UObject::InitClassDefaultObject);

#warning Vtable Erase!
    appMemset(this, 0, sizeof(UObject)); //CHECKIT hackky

    Class = InClass;
    Index = INDEX_NONE;

#warning Vtable copy! Check it for your compiller
    *((void **)this) = *((void **)InClass); //CHECKIT hackky copy vtable NOT PORTABLE

    InitProperties((BYTE *)this, InClass->GetPropertiesSize(), InClass->GetSuperClass(), NULL, 0);

    unguard;
}


void UObject::ProcessInternal( FFrame& TheStack, void*const Result )
{
    guard(UObject::ProcessInternal);

    static INT Recurse = 0;

    UFunction *Func = (UFunction *)TheStack.Node;

    DWORD Sing = Func->FunctionFlags & FUNC_Singular;

    if ( !ProcessRemoteFunction(Func, TheStack.Locals, NULL) )
    {
        if ( IsProbing( TheStack.Node->Name ) )
        {
            if ( !(ObjectFlags & Sing) )
            {
                ObjectFlags |= Sing;

                BYTE Buffer[1024];

                appMemset(Buffer, 0, 0xC); //CHECKIT why so small? or it's mistake and needed whole

                if ( ++Recurse > 250 )
                    TheStack.Logf(NAME_Critical, "Infinite script recursion (%i calls) detected", 250);

                while ( *TheStack.Code != EX_Return )
                    TheStack.Step(TheStack.Object, Buffer);

                TheStack.Code++; //Skip return

                TheStack.Step(TheStack.Object, Result);

                ObjectFlags &= ~Sing;

                --Recurse;
            }
        }
    }


    unguard;
}


void UObject::ParseParms( const TCHAR* Parms )
{
    guard(UObject::ParseParms);

    if ( Parms )
    {
        for(TFieldIterator<UProperty> It(Class); It; ++It )
        {
            if (It->Outer != UObject::StaticClass())
            {
                FString Value;
                FString tmp( *(It->Name) );
                tmp += TEXT("=");
                if ( Parse(Parms, *tmp, Value) )
                    It->ImportText(*Value, (BYTE *)this + It->Offset, PPF_Localized);
            }
        }
    }

    unguard;
}
