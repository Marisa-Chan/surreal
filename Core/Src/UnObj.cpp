#include "Core.h"
#include "UnLinker.h"
#include "UnArUtil.h"

//Local
static UBOOL GNoGC = false;
static UBOOL GCheckConflicts = 0;
static UBOOL GExitPurge = 0;
static INT GGarbageRefCount = 0;
static DOUBLE GTempDouble = 0.0;
static INT GNativeDuplicate = 0;
static ULinkerSave *GTempSave = NULL;


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
TCHAR				UObject::GLanguage[64] = TEXT("int");





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
    UObject **ppobj = &GObjHash[ GetObjectHash(Name, OuterIndex) ];

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

        if ( !appStricmp(*tmp1, DLLEXT) || !appStricmp(*tmp1, TEXT(".u")) || !appStricmp(*tmp1, TEXT(".ilk")) )
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

        Cmd = It->ImportText(Cmd, &tmp[It->Offset], PPF_Localized);

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
        if ( PerObject && Outer != UObject::GetTransientPackage() )
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
            if ( PerObject && Outer != UObject::GetTransientPackage() )
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



UObject* UObject::StaticFindObject( UClass* ObjectClass, UObject* InObjectPackage, const TCHAR* InName, UBOOL ExactClass )
{
    guard(UObject::StaticFindObject);

    UObject *ObjectPackage;
    if ( InObjectPackage == (UObject *)-1 )
        ObjectPackage = NULL;
    else
        ObjectPackage = InObjectPackage;

    if ( !ResolveName(ObjectPackage, InName, false, false) )
        return NULL;

    FName ObjectName(InName, FNAME_Find);

    if ( ObjectName == NAME_None )
        return NULL;

    for ( UObject *i = GObjHash[ GetObjectHash(ObjectName, GetTypeHash(ObjectPackage)) ]; i; i = i->HashNext )
    {
        if ( i->Name == ObjectName && i->Outer == ObjectPackage )
        {
            if ( !ObjectClass )
                return i;
            else
            {
                if ( ExactClass )
                {
                    if ( i->Class == ObjectClass )
                        return i;
                }
                else
                {
                    if ( i->IsA(ObjectClass) )
                        return i;
                }
            }

        }
    }

    if ( InObjectPackage == (UObject *)-1 )
    {
        for (TObjectIterator<UObject> It; It; ++It)
        {
            if ( It->Name == ObjectName )
            {
                if ( !ObjectClass )
                    return *It;
                else
                {
                    if ( ExactClass )
                    {
                        if ( It->Class == ObjectClass )
                            return *It;
                    }
                    else
                    {
                        if ( It->IsA(ObjectClass) )
                            return *It;
                    }
                }
            }
        }
    }

    return NULL;
    unguard;
}


UObject* UObject::StaticFindObjectChecked( UClass* InClass, UObject* InOuter, const TCHAR* InName, UBOOL ExactClass )
{
    guard(UObject::StaticLoadObject);

    UObject *Result = StaticFindObject(InClass, InOuter, InName, ExactClass);

    if ( !Result )
    {
        const TCHAR *rsn;

        if ( InOuter == (UObject *)-1 )
        {
            rsn = TEXT("Any");
        }
        else if ( InOuter )
        {
            rsn = *(InOuter->Name);
        }
        else
        {
            rsn = TEXT("None");
        }

        appErrorf( LocalizeError("ObjectNotFound", TEXT("Core")), *(InClass->Name), rsn, InName );
    }

    return Result;

    unguard;
}


UObject* UObject::StaticLoadObject( UClass* ObjectClass, UObject* InOuter, const TCHAR* InName, const TCHAR* Filename, DWORD LoadFlags, UPackageMap* Sandbox )
{
    guard(UObject::StaticLoadObject);

    check( ObjectClass );
    check( InName );

    ULinkerLoad *Loader = NULL;
    UObject* Result = NULL;

    BeginLoad();

    ResolveName(InOuter, InName, true, true);

    while ( InOuter && InOuter->Outer )
        InOuter = InOuter->GetOuter();

    if ( !(LoadFlags & LOAD_DisallowFiles) )
        Loader = GetPackageLinker(InOuter, Filename, LoadFlags | (LOAD_AllowDll | LOAD_Throw), Sandbox, NULL);

    if ( Loader )
        Result = Loader->Create(ObjectClass, InName, LoadFlags, false);

    if ( !Result )
    {
        Result = StaticFindObject(ObjectClass, InOuter, InName);

        if ( !Result )
            appThrowf(LocalizeError("ObjectNotFound", TEXT("Core")), *(ObjectClass->Name), (InOuter ? InOuter->GetPathName() : TEXT("None")), *InName);
    }
    UObject::EndLoad();
    return Result;

    unguard;
}


UClass* UObject::StaticLoadClass( UClass* BaseClass, UObject* InOuter, const TCHAR* Name, const TCHAR* Filename, DWORD LoadFlags, UPackageMap* Sandbox )
{
    guard(UObject::StaticLoadClass);

    check(BaseClass);

    UClass *Class = LoadObject<UClass>( InOuter, Name, Filename, LoadFlags | LOAD_Throw, Sandbox );

    if (Class && !Class->IsChildOf(BaseClass))
        appThrowf(LocalizeError("LoadClassMismatch", TEXT("Core")), Class->GetFullName(), BaseClass->GetFullName());

    return Class;

    unguard;
}


UObject* UObject::StaticAllocateObject( UClass* InClass, UObject* InOuter, FName InName, DWORD InSetFlags, UObject* Template, FOutputDevice* Error, UObject* Ptr )
{
    guard(UObject::StaticAllocateObject);

    check(Error);

    if ( !InClass )
    {
        Error->Logf(TEXT("Empty class for object %s"), *InName);
        return NULL;
    }

    check( !InClass || InClass->ClassWithin );
    check( !InClass || InClass->ClassConstructor );

    if ( InClass->Index == INDEX_NONE && GObjRegisterCount == 0 )
    {
        Error->Logf(TEXT("Unregistered class for %s"), *InName);
        return NULL;
    }

    if ( InClass->ClassFlags & CLASS_Abstract )
    {
        Error->Logf(LocalizeError("Abstract", TEXT("Core")), *InName, *(InClass->Name) );
        return NULL;
    }

    if ( InOuter )
    {
        if ( !InOuter->IsA( InClass->ClassWithin ) )
        {
            if ( InClass->ClassWithin )
            {
                Error->Logf(LocalizeError("NotWithin", TEXT("Core")), *(InClass->Name), *InName, *(InOuter->Class->Name), *(InClass->ClassWithin->Name) );
                return NULL;
            }
        }
    }
    else if ( InClass != UPackage::StaticClass() )
    {
        Error->Logf(LocalizeError("NotPackaged", TEXT("Core")), *(InClass->Name), *InName);
        return NULL;
    }

    if ( InName == NAME_None )
        InName = MakeUniqueObjectName(InOuter, InClass);

    if ( GCheckConflicts && InName != NAME_None )
    {
        for (UObject *i = GObjHash[ GetObjectHash(InName, GetTypeHash(InOuter)) ]; i; i = i->HashNext )
        {
            if ( i->Name == InName && i->Outer == InOuter && i->Class != InClass )
                GLog->Logf(NAME_Log, TEXT("CONFLICT: %s - %s"), i->GetFullName(), *(InClass->Name));
        }
    }

    UObject *Obj = StaticFindObject( InClass, InOuter, *InName, false);
    UClass *Cls = Cast<UClass>(Obj);
    INT Index = INDEX_NONE;
    UClass *ClassWithin = NULL;
    DWORD ClassFlags = 0;
    void(*ClassConstructor)(void*) = NULL;

    if ( Obj )
    {
        check( !Ptr || Ptr==Obj );

        debugfSlow(NAME_DevReplace, TEXT("Replacing %s"), *(Obj->Name) );

        if ( Obj->Class != InClass )
            appErrorf( LocalizeError("NoReplace", TEXT("Core")), Obj->GetFullName(), *(InClass->Name) );

        InSetFlags |= Obj->ObjectFlags & (RF_Marked|RF_Native);

        Index = Obj->Index;

        if ( Cls )
        {
            ClassFlags = Cls->ClassFlags & CLASS_Abstract;
            ClassWithin = Cls->ClassWithin;
            ClassConstructor = Cls->ClassConstructor;
        }

        Obj->~UObject();

        check( GObjAvailable.Num() && GObjAvailable.Last()==Index );

        GObjAvailable.Pop();
    }
    else
    {
        Obj = Ptr;

        if ( !Obj )
            Obj = (UObject *)::operator new(InClass->GetPropertiesSize(), *InName); //CHECKIT
    }

    if ( InClass->ClassFlags & CLASS_Transient )
        InSetFlags |= RF_Transient;

    Obj->Index = INDEX_NONE;
    Obj->_LinkerIndex = INDEX_NONE;
    Obj->HashNext = NULL;
    Obj->StateFrame = NULL;
    Obj->_Linker = NULL;
    Obj->Outer = InOuter;
    Obj->Name = InName;
    Obj->ObjectFlags = InSetFlags;
    Obj->Class = InClass;

    InitProperties((BYTE *)Obj, InClass->GetPropertiesSize(), InClass, (BYTE *)Template, InClass->GetPropertiesSize());

    Obj->AddObject(Index);

    check( !Obj->IsValid() );

    if ( InClass->ClassFlags & CLASS_PerObjectConfig )
    {
        Obj->LoadConfig();
        Obj->LoadLocalized();
    }

    if ( Cls )
    {
        Cls->ClassWithin = ClassWithin;
        Cls->ClassFlags |= ClassFlags;
        Cls->ClassConstructor = ClassConstructor;
    }

    return Obj;

    unguard;
}


UObject* UObject::StaticConstructObject( UClass* Class, UObject* InOuter, FName Name, DWORD SetFlags, UObject* Template, FOutputDevice* Error )
{
    guard(UObject::StaticConstructObject);

    check(Error);
    UObject *Result = StaticAllocateObject(Class, InOuter, Name, SetFlags, Template, Error, NULL);

    if (Result)
        Class->ClassConstructor(Result);

    return Result;
    unguard;
}


void UObject::StaticInit()
{
    guard(UObject::StaticInit);

    GObjNoRegister = true;
    GCheckConflicts = ParseParam(appCmdLine(), TEXT("CONFLICTS"));
    GNoGC = ParseParam(appCmdLine(), TEXT("NOGC"));

    for (INT i = 0; i < 4096; ++i )
        GObjHash[i] = NULL;

    GObjInitialized = true;
    ProcessRegistrants();

    GObjTransientPkg = new(NULL, TEXT("Transient"), 0) UPackage();

    GObjTransientPkg->AddToRoot();

    GObjPackageRemap = new TMultiMap<FName,FName>;

    GObjPackageRemap->Add(TEXT("UnrealI"), TEXT("UnrealShare"));

    GLog->Logf(NAME_Init, TEXT("Object subsystem initialized"));

    unguard;
}


void UObject::StaticExit()
{
    guard(UObject::StaticExit);

    check(GObjLoaded.Num() == 0);
    check(GObjRegistrants.Num() == 0);
    check(!GAutoRegister);

    GObjTransientPkg->RemoveFromRoot();

    for (TObjectIterator<UObject> It; It; ++It)
        It->ObjectFlags |= RF_TagGarbage | RF_Unreachable;

    for (INT i = 0; i < FName::GetMaxNames(); ++i )
    {
        FNameEntry* N = FName::GetEntry(i);
        if ( N )
            N->Flags |= RF_Unreachable;
    }

    GExitPurge = true;

    PurgeGarbage();

    GObjObjects.Empty();
    GObjLoaded.Empty();
    GObjObjects.Empty();
    GObjAvailable.Empty();
    GObjLoaders.Empty();
    GObjRoot.Empty();
    GObjRegistrants.Empty();
    GObjPreferences.Empty();
    GObjDrivers.Empty();

    if ( GObjPackageRemap )
    {
        GObjPackageRemap->Empty();
        delete GObjPackageRemap;
    }

    GObjInitialized = false;

    GLog->Logf(NAME_Exit, TEXT("Object subsystem successfully closed."));

    unguard;
}



static void ShowClasses(UClass *Class, FOutputDevice &Ar, INT Indent)
{
    Ar.Logf(TEXT("%s%s"), appSpc(Indent), Class->GetName());

    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->GetSuperClass() == Class)
            ShowClasses(*It, Ar, Indent + 2);
    }
}


UBOOL UObject::StaticExec( const TCHAR* Cmd, FOutputDevice& Ar )
{
    guard(UObject::StaticExec);

    if ( ParseCommand(&Cmd, TEXT("MEM")) )
    {
        GMalloc->DumpAllocs();
    }
    else if ( ParseCommand(&Cmd, TEXT("DUMPNATIVES")) )
    {
        for (INT i = 0; i < 4096; ++i )
        {
            if ( GNatives[i] == &UObject::execUndefined )
                GLog->Logf(TEXT("Native index %i is available"), i);
        }
    }
    else if ( ParseCommand(&Cmd, TEXT("GET")) )
    {
        TCHAR ClassName[256];
        TCHAR PropertyName[256];

        if ( !ParseToken(Cmd, ClassName, 256, true) )
        {
            Ar.Logf(NAME_ExecWarning, TEXT("Unrecognized class %s"), ClassName);
            return true;
        }

        UClass *Cls = FindObject<UClass>((UObject *)-1, ClassName, false);
        if (!Cls)
        {
            Ar.Logf(NAME_ExecWarning, TEXT("Unrecognized class %s"), ClassName);
            return true;
        }

        if ( !ParseToken(Cmd, PropertyName, 256, true) )
        {
            Ar.Logf(NAME_ExecWarning, TEXT("Unrecognized property %s"), PropertyName);
            return true;
        }

        UProperty *Prop = FindField<UProperty>(Cls, PropertyName);
        if (!Prop)
        {
            Ar.Logf(NAME_ExecWarning, TEXT("Unrecognized property %s"), PropertyName);
            return true;
        }

        TCHAR Buff[256];
        appMemset(Buff, 0, sizeof(Buff));

        if (Cls->Defaults.Num())
            Prop->ExportText(0, Buff, (BYTE *)Cls->Defaults.GetData(), (BYTE *)Cls->Defaults.GetData(), PPF_Localized);

        Ar.Log(Buff);
    }
    else if ( ParseCommand(&Cmd, TEXT("SET")) )
    {
        TCHAR ClassName[256];
        TCHAR PropertyName[256];

        if ( !ParseToken(Cmd, ClassName, 256, true) )
        {
            Ar.Logf(NAME_ExecWarning, TEXT("Unrecognized class %s"), ClassName);
            return true;
        }

        UClass *Cls = FindObject<UClass>((UObject *)-1, ClassName, false);
        if (!Cls)
        {
            Ar.Logf(NAME_ExecWarning, TEXT("Unrecognized class %s"), ClassName);
            return true;
        }

        if ( !ParseToken(Cmd, PropertyName, 256, true) )
        {
            Ar.Logf(NAME_ExecWarning, TEXT("Unrecognized property %s"), PropertyName);
            return true;
        }

        UProperty *Prop = FindField<UProperty>(Cls, PropertyName);
        if (!Prop)
        {
            Ar.Logf(NAME_ExecWarning, TEXT("Unrecognized property %s"), PropertyName);
            return true;
        }

        while(Cmd[0] == ' ')
            ++Cmd;

        GlobalSetProperty(Cmd, Cls, Prop, Prop->Offset, true);
    }
    else if ( !ParseCommand(&Cmd, TEXT("OBJ")) )
    {
        if ( !ParseCommand(&Cmd, TEXT("GTIME")) )
            return false;

        GLog->Logf(TEXT("GTime = %f"), GTempTime);
    }
    else if ( ParseCommand(&Cmd, TEXT("GARBAGE")) )
    {
        UBOOL tmp = GNoGC;
        GNoGC = false;

        CollectGarbage((GIsEditor ? RF_Standalone : 0) | RF_Native);

        GNoGC = tmp;
    }
    else if ( ParseCommand(&Cmd, TEXT("MARK")) )
    {
        GLog->Logf(TEXT("Marking objects"));

        for (TObjectIterator<UObject> It; It; ++It)
            It->ObjectFlags |= RF_Marked;
    }
    else if ( ParseCommand(&Cmd, TEXT("MARKCHECK")) )
    {
        GLog->Logf(TEXT("Unmarked objects:"));

        for (TObjectIterator<UObject> It; It; ++It)
        {
            if ( !(It->ObjectFlags & RF_Marked) )
                GLog->Logf(TEXT("%s"), It->GetFullName());
        }
    }
    else if ( ParseCommand(&Cmd, TEXT("REFS")) )
    {
        UClass *ObjClass;
        UObject *Object;

        if ( !ParseObject<UClass>(Cmd, TEXT("CLASS="), ObjClass, (UObject *)-1)
                || !ParseObject(Cmd, TEXT("NAME="), ObjClass, Object, (UObject *)-1) )
        {
            return true;
        }

        Ar.Logf(TEXT(""));
        Ar.Logf(TEXT("Referencers of %s:"), Object->GetFullName());

        for (TObjectIterator<UObject> It; It; ++It)
        {
            FArchiveFindCulprit Culprit(Object);

            It->Serialize(Culprit);

            if (Culprit.GetCount())
                Ar.Logf(TEXT("   %s"), It->GetFullName());
        }

        Ar.Logf(TEXT(""));
        Ar.Logf(TEXT("Shortest reachability from root to %s:"), Object->GetFullName());

        TArray<UObject*> Route = FArchiveTraceRoute::FindShortestRootPath(Object);
        for(INT i = 0; i < Route.Num(); ++i)
        {
            if ( i == 0 )
            {
                if ( Route(i)->GetFlags() & RF_Native )
                    Ar.Logf(TEXT("   %s%s"), Route(i)->GetFullName(), TEXT(" (native)"));
                else
                    Ar.Logf(TEXT("   %s%s"), Route(i)->GetFullName(), TEXT(" (root)"));
            }
            else
            {
                Ar.Logf(TEXT("   %s%s"), Route(i)->GetFullName(), TEXT(""));
            }
        }

        if ( !Route.Num() )
            Ar.Logf(TEXT("   (Object is not currently rooted)"));
    }
    else if ( ParseCommand(&Cmd, TEXT("HASH")) )
    {
        FName::DisplayHash(Ar);

        /*INT Objs = 0;
        for (TObjectIterator<UObject> It; It; ++It)
        {
        	++Objs;
        }

        INT Hsh = 0;
        for (INT i = 0; i < 4096; ++i)
        {
        	INT Chain = 0;
        	UObject *H = GObjHash[i];
        	while (H)
        	{
        		H = H->HashNext;
        		Chain++;
        	}

        	if (Chain)
        		++Hsh;
        }*/
    }
    else if ( ParseCommand(&Cmd, TEXT("CLASSES")) )
    {
        ShowClasses(UObject::StaticClass(), Ar, 0);
    }
    else if ( ParseCommand(&Cmd, TEXT("DEPENDENCIES")) )
    {
        UPackage *Pkg;
        if ( ParseObject<UPackage>(Cmd, TEXT("PACKAGE="), Pkg, NULL) )
        {
            TArray<UObject*> Exclude;
            for (INT i = 0; i < 16; ++i )
            {
                TCHAR Buf[16];
                appSprintf(Buf, TEXT("EXCLUDE%i="), i);

                FName N;
                if ( Parse(Cmd, Buf, N) )
                    Exclude( Exclude.Add() ) = UObject::CreatePackage(NULL, *N);
            }

            Ar.Logf(TEXT("Dependencies of %s:"), Pkg->GetPathName());

            for (TObjectIterator<UObject> It; It; ++It)
            {
                if (It->GetOuter() == Pkg)
                {
                    FArchiveShowReferences ShowRef(Ar, Pkg, *It, Exclude);

                    It->Serialize(ShowRef);
                }
            }
        }
    }
    else if ( ParseCommand(&Cmd, TEXT("LIST")) )
    {
        Ar.Log(TEXT("Objects:"));
        Ar.Log(TEXT(""));

        UClass *CheckType = NULL;
        UPackage *CheckPackage = NULL;
        UPackage *InsidePackage = NULL;

        ParseObject<UClass>(Cmd, TEXT("CLASS="), CheckType, (UObject *)-1);
        ParseObject<UPackage>(Cmd, TEXT("PACKAGE="), CheckPackage, NULL);
        ParseObject<UPackage>(Cmd, TEXT("INSIDE="), InsidePackage, NULL);

        struct FItem
        {
            UClass*		Class;
            INT			Count;
            SIZE_T		Num;
            SIZE_T		Max;

            FItem()
                : Class		( NULL )
                , Count		( 0 )
                , Num		( 0 )
                , Max		( 0 )
            {}

            static QSORT_RETURN CDECL QCompare( const void* _a, const void* _b )
            {
                const FItem *a = (const FItem *)_a;
                const FItem *b = (const FItem *)_b;

                return b->Max - a->Max;
            }
        };

        struct FSubItem
        {
            UObject*	Object;
            SIZE_T		Num;
            SIZE_T		Max;

            FSubItem()
                : Object	( NULL )
                , Num		( 0 )
                , Max		( 0 )
            {}

            static QSORT_RETURN CDECL QCompare( const void* _a, const void* _b )
            {
                const FSubItem *a = (const FSubItem *)_a;
                const FSubItem *b = (const FSubItem *)_b;

                INT Res = b->Max - a->Max;
                if (Res)
                    return Res;

                Res = appStrcmp(a->Object->GetClass()->GetName(), b->Object->GetClass()->GetName());
                if (Res)
                    return Res;

                Res = appAtoi(a->Object->GetName() + appStrlen(a->Object->GetClass()->GetName()))
                      - appAtoi(b->Object->GetName() + appStrlen(b->Object->GetClass()->GetName()));
                return Res;
            }
        };

        TArray<FItem> List;
        TArray<FSubItem> Objects;
        FItem Total;

        for (TObjectIterator<UObject> It; It; ++It)
        {
            if ( (CheckType && It->IsA(CheckType)) ||
                    (CheckPackage && It->GetOuter() == CheckPackage) ||
                    (InsidePackage && It->IsIn(InsidePackage)) )
            {
                FArchiveCountMem Count;
                It->Serialize(Count);

                FItem *Found = NULL;
                for (INT i = 0; i < List.Num(); ++i)
                {
                    if (List(i).Class == It->GetClass())
                    {
                        Found = &List(i);
                        break;
                    }
                }

                if (!Found)
                {
                    Found = new(List) FItem();
                    if ( Found )
                        Found->Class = It->GetClass();
                }

                //if ( CheckType || CheckPackage || InsidePackage )
                {
                    FSubItem * Itm = new(Objects) FSubItem();
                    if ( Itm )
                    {
                        Itm->Object = *It;
                        Itm->Num = Count.GetNum();
                        Itm->Max = Count.GetMax();
                    }
                }

                ++Found->Count;
                Found->Num += Count.GetNum();
                Found->Max = Count.GetMax();

                ++Total.Count;
                Total.Num += Count.GetNum();
                Total.Max += Count.GetMax();
            }
        }

        INT DeletedObjects = 0;

        if (Objects.Num())
        {
            appQsort(Objects.GetData(), Objects.Num(), sizeof(FSubItem), FSubItem::QCompare);

            Ar.Log(TEXT(" "));
            Ar.Logf(TEXT("%-78s %10s %10s"), TEXT("Object"), TEXT("NumBytes"), TEXT("MaxBytes"));
            UBOOL Verbose = ParseCommand(&Cmd, TEXT("VERBOSE"));

            for (INT i = 0; i < Objects.Num(); ++i )
            {
                if ( Verbose || !Objects(i).Object->IsPendingKill() )
                {
                    if ( !Objects(i).Object->IsPendingKill() )
                        Ar.Logf(TEXT("%-78s %10i %10i %s"), Objects(i).Object->GetFullName(), Objects(i).Num, Objects(i).Max, TEXT(" "));
                    else
                        Ar.Logf(TEXT("%-78s %10i %10i %s"), Objects(i).Object->GetFullName(), Objects(i).Num, Objects(i).Max, TEXT("X"));
                }

                if ( Objects(i).Object->IsPendingKill() )
                    ++DeletedObjects;
            }
            Ar.Log(TEXT(" "));
        }

        if ( List.Num() )
        {
            appQsort(List.GetData(), List.Num(), sizeof(FItem), FItem::QCompare);

            Ar.Logf(TEXT("%-30s %6s %10s %10s"), TEXT("Class"), TEXT("Count"), TEXT("NumKBytes"), TEXT("MaxKBytes"));

            for (INT i = 0; i < List.Num(); ++i )
                Ar.Logf(TEXT("%-30s %6i %10i %10i"), List(i).Class->GetName(), List(i).Count, List(i).Num / 1024, List(i).Max /1024);

            Ar.Log(TEXT(" "));
        }

        Ar.Logf(TEXT("%i Objects (%.3fM / %.3fM)"), Total.Count, Total.Num / 1048576.0, Total.Max / 1048576.0);
        Ar.Logf(TEXT("%i Deleted Objects"), DeletedObjects);
    }
    else if ( ParseCommand(&Cmd, TEXT("VFHASH")) )
    {
        Ar.Logf(TEXT("Class VfHashes:"));

        for (TObjectIterator<UState> It; It; ++It)
        {
            Ar.Logf(TEXT("%s:"), It->GetName());
            for (INT i = 0; i < 256; ++i )
            {
                INT Cnt = 0;
                UField *Hsh = It->VfHash[i];
                while ( Hsh )
                {
                    Hsh = Hsh->HashNext;
                    ++Cnt;
                }
                Ar.Logf(TEXT("   %i: %i"), i, Cnt);
            }
        }
    }
    else if ( ParseCommand(&Cmd, TEXT("LINKERS")) )
    {
        Ar.Logf(TEXT("Linkers:"));

        for(INT i = 0; i < GObjLoaders.Num(); ++i)
        {
            ULinkerLoad* Loder = CastChecked<ULinkerLoad>( GObjLoaders(i) );

            INT NameSz = 0;
            for (INT j = 0; j < Loder->NameMap.Num(); ++j)
                NameSz += sizeof(TCHAR) * appStrlen( *(Loder->NameMap(j)) ) + 14; //sizeof???

            Ar.Logf(TEXT("%s (%s): Names=%i (%iK/%iK) Imports=%i (%iK) Exports=%i (%iK) Gen=%i Lazy=%i"),
                    *(Loder->Filename),
                    Loder->LinkerRoot->GetFullName(),
                    Loder->NameMap.Num(),
                    Loder->NameMap.Num() * sizeof(FName)  / 1024, //CHECKIT
                    NameSz / 1024,
                    Loder->ImportMap.Num(),
                    Loder->ImportMap.Num() * sizeof(FObjectImport) / 1024,
                    Loder->ExportMap.Num(),
                    Loder->ExportMap.Num() * sizeof(FObjectExport) / 1024,
                    Loder->Summary.Generations.Num(),
                    Loder->LazyLoaders.Num());
        }
    }
    else
        return false;

    return true;
    unguard;
}


void UObject::StaticTick()
{
    guard(UObject::StaticTick);

    check(GObjBeginLoadCount == 0);

    if ( GNativeDuplicate )
        appErrorf(TEXT("Duplicate native registered: %i"), GNativeDuplicate);

    unguard;
}


UObject* UObject::LoadPackage( UObject* InOuter, const TCHAR* InFilename, DWORD LoadFlags )
{
    guard(UObject::LoadPackage);
    BeginLoad();

    if ( !InFilename )
        InFilename = InOuter->GetName();

    ULinkerLoad *Linker = GetPackageLinker(InOuter, InFilename, LoadFlags | LOAD_Throw, NULL, NULL);

    if ( !(LoadFlags & LOAD_Verify) )
        Linker->LoadAllObjects();

    EndLoad();

    return Linker->LinkerRoot;
    unguard;
}



static QSORT_RETURN CDECL LinkerNameSort( const void* _a, const void* _b )
{
    FName *a = (FName *)_a;
    FName *b = (FName *)_b;

    return GTempSave->MapName(b) - GTempSave->MapName(a);
}

static QSORT_RETURN CDECL LinkerImportSort( const void* _a, const void* _b )
{
    const FObjectImport *a = (const FObjectImport *)_a;
    const FObjectImport *b = (const FObjectImport *)_b;

    return GTempSave->MapObject(b->XObject) - GTempSave->MapObject(a->XObject);
}

static QSORT_RETURN CDECL LinkerExportSort( const void* _a, const void* _b )
{
    const FObjectExport *a = (const FObjectExport *)_a;
    const FObjectExport *b = (const FObjectExport *)_b;

    return GTempSave->MapObject(b->_Object) - GTempSave->MapObject(a->_Object);
}

UBOOL UObject::SavePackage( UObject* InOuter, UObject* Base, DWORD TopLevelFlags, const TCHAR* Filename, FOutputDevice* Error, ULinkerLoad* Conform )
{
    guard(UObject::SavePackage);

    DWORD TStart = appCycles();

    check(InOuter);
    check(Filename);

    UPackage *P = Cast<UPackage>(InOuter);
    if (P)
        P->PackageFlags |= PKG_AllowDownload;

    TCHAR TempFilename[256];

    appStrcpy(TempFilename, Filename);
    for (INT i = appStrlen(TempFilename); i > 0; --i )
    {
        TCHAR C = TempFilename[ i - 1 ];
        if ( C == '\\' || C == '/' || C == ':' )
        {
            TempFilename[i] = 0;
            break;
        }
    }

    appStrcat(TempFilename, TEXT("Save.tmp"));

    GWarn->StatusUpdatef(0, 0, LocalizeProgress("Saving", TEXT("Core")), Filename);

    for (TObjectIterator<UObject> It; It; ++It)
        It->ClearFlags(RF_LoadContextFlags | RF_TagImp | RF_TagExp);

    for (INT i = 0; i < FName::GetMaxNames(); ++i)
        FName::GetEntry(i)->Flags &= ~(RF_LoadContextFlags | RF_TagImp | RF_TagExp);

    FArchiveSaveTagExports Exp(InOuter);

    for (TObjectIterator<UObject> It; It; ++It)
    {
        if ( (It->GetFlags() & TopLevelFlags) && It->IsIn(InOuter) )
        {
            UObject* Tmp = *It;
            Exp << Tmp;
        }
    }

    ULinkerSave* ULSave = new ULinkerSave(InOuter, TempFilename);

    for (TObjectIterator<UObject> It; It; ++It)
    {
        if ( It->GetFlags() & RF_TagExp )
        {
            FArchiveSaveTagImports Imp( It->GetFlags() & RF_LoadContextFlags, ULSave );

            It->Serialize(Imp);
            UClass *Cls = It->GetClass();
            Imp << Cls;

            if ( It->IsIn( UObject::GetTransientPackage() ))
                appErrorf(LocalizeError("TransientImport", TEXT("Core")), It->GetFullName());
        }
    }

    for (TObjectIterator<UObject> It; It; ++It)
    {
        if ( It->GetFlags() & (RF_TagExp | RF_TagImp) )
        {
            It->Name.SetFlags(RF_LoadContextFlags | RF_TagExp);

            if (It->GetOuter())
                It->GetOuter()->Name.SetFlags(RF_LoadContextFlags | RF_TagExp);

            if (It->GetFlags() & RF_TagImp)
            {
                It->GetClass()->Name.SetFlags(RF_LoadContextFlags | RF_TagExp);

                check(It->GetClass()->GetOuter());

                It->GetClass()->GetOuter()->Name.SetFlags(RF_LoadContextFlags | RF_TagExp);

                if ( !(It->GetFlags() & RF_Public) )
                    appThrowf(LocalizeError("FailedSavePrivate", TEXT("Core")), Filename, It->GetFullName());
            }
            else
            {
                debugfSlow(NAME_DevSave, TEXT("Saving %s"), It->GetFullName());
            }
        }
    }

    if (Conform)
    {
        debugf(TEXT("Conformal save, relative to: %s, Generation %i"), *(Conform->Filename), Conform->Summary.Generations.Num() + 1);

        ULSave->Summary.Guid = Conform->Summary.Guid;
        ULSave->Summary.Generations = Conform->Summary.Generations;
    }
    else
    {
        ULSave->Summary.Guid = appCreateGuid();
        ULSave->Summary.Generations.Empty(); //CHECKIT! Originally it's copy of empty array
    }

    new(ULSave->Summary.Generations) FGenerationInfo(0, 0);

    (*ULSave) << ULSave->Summary;
    ULSave->Summary.NameOffset = ULSave->Tell();

    for (INT i = 0; i < FName::GetMaxNames(); ++i)
    {
        FNameEntry *N = FName::GetEntry(i);
        if (N && (N->Flags & RF_TagExp))
            new(ULSave->NameMap) FName((EName)i);
    }

    GTempSave = ULSave;

    if (Conform)
    {
        for (INT i = 0; i < Conform->NameMap.Num(); ++i)
        {
            INT jj = INDEX_NONE;
            for(int j = 0; j < ULSave->NameMap.Num(); ++j)
            {
                if ( ULSave->NameMap(j) == Conform->NameMap(i) )
                {
                    jj = j;
                    break;
                }
            }

            if (Conform->NameMap(i) != NAME_None && jj != INDEX_NONE)
            {
                FName tmp = ULSave->NameMap(i);
                ULSave->NameMap(i) = ULSave->NameMap(jj);
                ULSave->NameMap(jj) = tmp;
            }
            else
            {
                new(ULSave->NameMap) FName(ULSave->NameMap(i));
            }
        }
    }

    appQsort( &ULSave->NameMap( Conform->NameMap.Num() ), ULSave->NameMap.Num() - Conform->NameMap.Num(), sizeof(FName), LinkerNameSort);

    ULSave->Summary.NameCount = ULSave->NameMap.Num();
    for ( INT i = 0; i < ULSave->NameMap.Num(); ++i )
    {
        (*ULSave) << *FName::GetEntry( ULSave->NameMap(i).GetIndex() );
        ULSave->NameIndices( ULSave->NameMap(i).GetIndex() ) = i;
    }

    for (TObjectIterator<UObject> It; It; ++It)
    {
        if ( It->GetFlags() & RF_TagImp )
            new(ULSave->ImportMap)FObjectImport(*It);
    }

    ULSave->Summary.ImportCount = ULSave->ImportMap.Num();

    GTempSave = ULSave;
    appQsort(ULSave->ImportMap.GetData(), ULSave->ImportMap.Num(), sizeof(FObjectImport), LinkerImportSort);

    for (TObjectIterator<UObject> It; It; ++It)
    {
        if ( It->GetFlags() & RF_TagExp )
            new(ULSave->ExportMap)FObjectExport(*It);
    }

    if (Conform)
    {
        TArray<FObjectExport> ExpArr (ULSave->ExportMap);
        ULSave->ExportMap.Empty();

        TArray<UBOOL> ExpB;
        ExpB.AddZeroed(ExpArr.Num());

        TMap<FString,INT> ExpMap;

        for ( INT i = 0; i < ExpArr.Num(); ++i )
            ExpMap.Set( ExpArr(i)._Object->GetFullName(), i );

        for ( INT i = 0; i < Conform->ExportMap.Num(); ++i )
        {
            INT *PVal = ExpMap.Find( Conform->GetExportFullName( i, ULSave->LinkerRoot->GetPathName() ) );

            if (PVal)
            {
                ULSave->ExportMap( ULSave->ExportMap.Add() ) = ExpArr(*PVal);
                check( ULSave->ExportMap.Last()._Object == ExpArr(*PVal)._Object );

                ExpB(*PVal) = true;
            }
            else
                new(ULSave->ExportMap)FObjectExport(NULL);
        }

        for ( INT i = 0; i < ExpB.Num(); ++i )
        {
            if (!ExpB(i))
                ULSave->ExportMap( ULSave->ExportMap.Add() ) = ExpArr(i);
        }

        appQsort( &ULSave->ExportMap(Conform->ExportMap.Num()), ULSave->ExportMap.Num() - Conform->ExportMap.Num(), sizeof(FObjectExport), LinkerExportSort);
    }
    else
        appQsort(ULSave->ExportMap.GetData(), ULSave->ExportMap.Num(), sizeof(FObjectExport), LinkerExportSort);

    ULSave->Summary.ExportCount = ULSave->ExportMap.Num();

    for ( INT i = 0; i < ULSave->ExportMap.Num(); ++i )
    {
        UObject *O = ULSave->ExportMap(i)._Object;
        if ( O )
            ULSave->ObjectIndices( O->GetIndex() ) = 1 + i;
    }

    for ( INT i = 0; i < ULSave->ImportMap.Num(); ++i )
    {
        UObject *O = ULSave->ImportMap(i).XObject;
        if ( O )
            ULSave->ObjectIndices( O->GetIndex() ) = -1 - i;
    }

    for ( INT i = 0; i < ULSave->ExportMap.Num(); ++i )
    {
        FObjectExport &Export = ULSave->ExportMap(i);
        if ( Export._Object )
        {
            if (Export._Object->IsA(UClass::StaticClass()) )
            {
                Export.ClassIndex = ULSave->ObjectIndices( Export._Object->GetClass()->GetIndex() );
                check(Export.ClassIndex != 0);
            }

            UStruct *S = Cast<UStruct>(Export._Object);
            if (S)
            {
                Export.SuperIndex = ULSave->ObjectIndices(S->GetIndex());
                check(Export.SuperIndex != 0);
            }

            if ( Export._Object->GetOuter() != InOuter )
            {
                check( Export._Object->GetOuter()->IsIn(InOuter) );

                Export.PackageIndex = ULSave->ObjectIndices( Export._Object->GetOuter()->GetIndex() );
                check(Export.PackageIndex != 0);
            }

            Export.SerialOffset = ULSave->Tell();
            Export._Object->Serialize( *ULSave );
            Export.SerialSize = ULSave->Tell() - Export.SerialOffset;
        }
    }

    ULSave->Summary.ImportOffset = ULSave->Tell();

    for ( INT i = 0; i < ULSave->ImportMap.Num(); ++i )
    {
        FObjectImport &Import = ULSave->ImportMap(i);
        if ( Import.XObject->GetOuter() )
        {
            check( !Import.XObject->GetOuter()->IsIn(InOuter) );

            Import.PackageIndex = ULSave->ObjectIndices(Import.XObject->GetOuter()->GetIndex());

            check( Import.PackageIndex < 0 );
        }
        (*ULSave) << Import;
    }

    ULSave->Summary.ExportOffset = ULSave->Tell();

    for ( INT i = 0; i < ULSave->ExportMap.Num(); ++i )
    {
        FObjectExport &Export = ULSave->ExportMap(i);
        (*ULSave) << Export;
    }

    GWarn->StatusUpdatef( 0, 0, TEXT("%s"), LocalizeProgress("Closing", TEXT("Core")) );

    ULSave->Summary.Generations.Last().ExportCount = ULSave->Summary.ExportCount;
    ULSave->Summary.Generations.Last().NameCount = ULSave->Summary.NameCount;

    ULSave->Seek(0);

    (*ULSave) << ULSave->Summary;

    debugf(NAME_Log, TEXT("Save=%f"), (appCycles() - TStart - 34) * GSecondsPerCycle * 1000.0);

    debugf(NAME_Log, TEXT("Moving '%s' to '%s'"), TempFilename, Filename);

    if ( !GFileManager->Move(Filename, TempFilename) )
    {
        GFileManager->Delete(TempFilename);
        GWarn->Logf(LocalizeError("SaveWarning", TEXT("Core")), Filename);
        return false;
    }

    return true;
    unguard;
}




class FArchiveTagUsed : public FArchive
{
public:
    FArchiveTagUsed()
    {
        GGarbageRefCount = 0;
        Context = NULL;

        for (TObjectIterator<UObject> It; It; ++It)
            It->SetFlags( RF_TagGarbage | RF_Unreachable );

        for (INT i = 0; i < FName::GetMaxNames(); ++i)
            FName::GetEntry(i)->Flags |= RF_Unreachable;
    }

    FArchive& operator<<( UObject*& Object )
    {
        guard(FArchiveTagUsed<<UObject);

        ++GGarbageRefCount;

        if ( Object )
        {
            if ( Object->GetFlags() & RF_EliminateObject )
            {
                Object = NULL;
            }
            else if ( Object->GetFlags() & RF_Unreachable )
            {
                Object->ClearFlags(RF_DebugSerialize | RF_Unreachable);

                if ( Object->GetFlags() & RF_TagGarbage )
                {
                    UObject* tmp = Context;
                    Context = Object;

                    Object->Serialize( *this );

                    if ( !(Object->GetFlags() & RF_DebugSerialize) )
                        GError->Logf(TEXT("%s failed to route Serialize"), Object->GetFullName());

                    Context = tmp;
                }
                else
                {
                    if ( Context )
                        debugfSlow(NAME_Log, TEXT("%s is referenced by %s"), Object->GetFullName(), Context->GetFullName());
                    else
                        debugfSlow(NAME_Log, TEXT("%s is referenced by %s"), Object->GetFullName(), NULL);
                }
            }
        }

        return *this;
        unguard;
    };

    FArchive& operator<<( FName& N )
    {
        guard(FArchiveTagUsed<<FName);

        N.ClearFlags(RF_Unreachable);

        return *this;
        unguard;
    };

protected:
    UObject*		Context;
};



void UObject::CollectGarbage( DWORD KeepFlags )
{
    guard(UObject::CollectGarbage);

    debugf(NAME_Log, TEXT("Collecting garbage"));

    FArchiveTagUsed Garbager;
    SerializeRootSet(Garbager, KeepFlags, RF_TagGarbage);
    PurgeGarbage();

    unguard;
}



void UObject::SerializeRootSet( FArchive& Ar, DWORD KeepFlags, DWORD RequiredFlags )
{
    guard(UObject::SerializeRootSet);

    Ar << GObjRoot;

    for (TObjectIterator<UObject> It; It; ++It)
    {
        if ( (It->GetFlags() & KeepFlags) && (It->GetFlags() & RequiredFlags) == RequiredFlags)
        {
            UObject *Obj = *It;
            Ar << Obj;
        }
    }

    unguard;
}



UBOOL UObject::IsReferenced( UObject*& Res, DWORD KeepFlags, UBOOL IgnoreReference )
{
    guard(UObject::IsReferenced);

    UObject* Obj = Res;

    if ( IgnoreReference )
        Res = NULL;

    FArchiveTagUsed Refs;

    Obj->ClearFlags(RF_TagGarbage);

    SerializeRootSet(Refs, KeepFlags, RF_TagGarbage);

    Res = Obj;

    return (Obj->GetFlags() & RF_Unreachable) == 0;

    unguard;
}


UBOOL UObject::AttemptDelete( UObject*& Res, DWORD KeepFlags, UBOOL IgnoreReference )
{
    guard(UObject::AttemptDelete);

    if ( (Res->GetFlags() & RF_Native) || IsReferenced(Res, KeepFlags, IgnoreReference) )
        return false;

    PurgeGarbage();
    return true;

    unguard;
}


void UObject::BeginLoad()
{
    guard(UObject::BeginLoad);

    if ( GObjBeginLoadCount++ == 0 )
    {
        check(GObjLoaded.Num() == 0);
        check(!GAutoRegister);

        for (INT i = 0; i < GObjLoaders.Num(); ++i )
        {
            check( GetLoader(i)->Success );
        }
    }

    unguard;
}


void UObject::EndLoad()
{
    guard(UObject::EndLoad);

    check(GObjBeginLoadCount > 0);

    if ( --GObjBeginLoadCount == 0 )
    {
        debugfSlow(NAME_DevLoad, TEXT("Loading objects..."));

        for (INT i = 0; i < GObjLoaded.Num(); ++i )
        {
            UObject* Obj= GObjLoaded(i);
            if ( Obj->GetFlags() & RF_NeedLoad )
            {
                check(Obj->GetLinker());
                Obj->GetLinker()->Preload(Obj);
            }
        }

        INT OriginalNum = GObjLoaded.Num();

        for (INT i = 0; i < GObjLoaded.Num(); ++i )
            GObjLoaded(i)->ConditionalPostLoad();

        check(GObjLoaded.Num() == OriginalNum);

        GObjLoaded.Empty();

        if ( GImportCount )
        {
            for (INT i = 0; i < GObjLoaders.Num(); ++i )
            {
                ULinkerLoad *Loader = GetLoader(i);

                for (INT j = 0; j < Loader->ImportMap.Num(); ++j)
                {
                    FObjectImport& Import = Loader->ImportMap(j);
                    if (Import.XObject && (Import.XObject->GetFlags() & RF_Native) )
                        Import.XObject = NULL;
                }
            }
        }

        GImportCount = 0;
    }

    unguard;
}


void UObject::InitProperties( BYTE* Data, INT DataCount, UClass* DefaultsClass, BYTE* Defaults, INT DefaultsCount )
{
    guard(UObject::InitProperties);

    check(DataCount >= sizeof(UObject));

    const INT OSZ = sizeof(UObject);
    INT Count = OSZ;

    if ( !Defaults && DefaultsClass && DefaultsClass->Defaults.Num())
    {
        DefaultsCount = DefaultsClass->Defaults.Num();
        Defaults = &DefaultsClass->Defaults(0);
    }

    if (Defaults)
    {
        appMemcpy(&Data[OSZ], &Defaults[OSZ], DefaultsCount - OSZ);
        Count = DefaultsCount;
    }

    if ( Count < DataCount )
        appMemset(&Data[Count], 0, DataCount - Count);

    if ( DefaultsClass )
    {
        for (UProperty* prop = DefaultsClass->ConstructorLink; prop; prop = prop->ConstructorLinkNext )
        {
            if ( prop->Offset < DefaultsCount )
            {
                memset(&Data[ prop->Offset ], 0, prop->ArrayDim * prop->ElementSize);
                prop->CopyCompleteValue(&Data[ prop->Offset ], &Defaults[ prop->Offset ]);
            }
        }
    }

    unguard;
}


void UObject::ExitProperties( BYTE* Data, UClass* Class )
{
    guard(UObject::ExitProperties);

    for (UProperty* prop = Class->ConstructorLink; prop; prop = prop->ConstructorLinkNext )
    {
        prop->DestroyValue(&Data[ prop->Offset ]);
    }

    unguard;
}


void UObject::ResetLoaders( UObject* InOuter, UBOOL DynamicOnly, UBOOL ForceLazyLoad )
{
    guard(UObject::ResetLoaders);

    for (INT i = GObjLoaders.Num() - 1; i >= 0; --i )
    {
        ULinkerLoad *Loader = CastChecked<ULinkerLoad>( GObjLoaders(i) );

        if ( !InOuter || Loader->LinkerRoot == InOuter )
        {
            if ( DynamicOnly )
            {
                for (INT j = 0; j < Loader->ExportMap.Num(); ++j )
                {
                    UObject* Obj = Loader->ExportMap(j)._Object;
                    if ( Obj )
                    {
                        if ( Obj->GetClass()->ClassFlags >= 0 )
                            Loader->DetachExport(j);
                    }
                }
            }
            else
            {
                if ( ForceLazyLoad )
                    Loader->DetachAllLazyLoaders(true);

                if ( Loader )
                    delete Loader;
            }
        }
    }

    unguard;
}


UPackage* UObject::CreatePackage( UObject* InOuter, const TCHAR* PkgName )
{
    guard(UObject::CreatePackage);

    ResolveName(InOuter, PkgName, true, false);

    UPackage *Obj = FindObject<UPackage>(InOuter, PkgName);
    if ( !Obj )
        Obj = new(InOuter, PkgName, RF_Public) UPackage();

    return Obj;
    unguard;
}


ULinkerLoad* UObject::GetPackageLinker( UObject* InOuter, const TCHAR* InFilename, DWORD LoadFlags, UPackageMap* Sandbox, FGuid* CompatibleGuid )
{
    guard(UObject::GetPackageLinker);

    check(GObjBeginLoadCount);

    ULinkerLoad *Result = NULL;

    if ( InOuter )
    {
        for (INT i = 0; i < GObjLoaders.Num() && !Result; ++i )
        {
            if ( GetLoader(i)->LinkerRoot == InOuter )
                Result = GetLoader(i);
        }
    }

    TCHAR NewFilename[256] = TEXT("");

    if (Result)
    {
        appStrcpy(NewFilename, TEXT(""));
    }
    else if (InFilename)
    {
        if ( !appFindPackageFile(InFilename, CompatibleGuid, NewFilename) )
            appThrowf(LocalizeError("FileNotFound", TEXT("Core")), InFilename);

        TCHAR Tmp[256];
        appStrcpy(Tmp, NewFilename);

        TCHAR *p = Tmp;

        while ( 1 )
        {
            if ( appStrstr(p, TEXT("\\")) )
                p = appStrstr(p, TEXT("\\")) + 1;
            else if ( appStrstr(p, TEXT("/")) )
                p = appStrstr(p, TEXT("/")) + 1;
            else if ( appStrstr(p, TEXT(":")) )
                p = appStrstr(p, TEXT(":")) + 1;
            else
                break;
        }

        if ( appStrstr(p, ".") )
            *appStrstr(p, ".") = 0;

        UPackage *Pkg = CreatePackage(NULL, p);
        if ( InOuter )
        {
            if ( InOuter != Pkg )
            {
                debugf(TEXT("New File, Existing Package (%s, %s)"), InOuter->GetFullName(), Pkg->GetFullName());
                ResetLoaders(InOuter, false, true);
            }
        }
        else
        {
            if ( !Pkg )
                appThrowf(LocalizeError("FilenameToPackage", TEXT("Core")), InFilename);

            InOuter = Pkg;

            for (INT i = 0; i < GObjLoaders.Num() && !Result; ++i )
            {
                if ( GetLoader(i)->LinkerRoot == Pkg )
                    Result = GetLoader(i);
            }
        }
    }
    else
    {
        if ( !InOuter )
            appThrowf(LocalizeError("PackageResolveFailed", TEXT("Core")));

        if ( !appFindPackageFile(InOuter->GetName(), CompatibleGuid, NewFilename) )
        {
            if ( (LoadFlags & LOAD_AllowDll) )
            {
                UPackage *Pkg = Cast<UPackage>(InOuter);
                if (Pkg && Pkg->DllHandle)
                    return NULL;
            }

            appThrowf(LocalizeError("PackageNotFound", TEXT("Core")), InOuter->GetName());
            return NULL;
        }
    }

    if ( !Result )
        Result = new ULinkerLoad(InOuter, NewFilename, LoadFlags);

    if ( CompatibleGuid && Result->Summary.Guid != *CompatibleGuid )
        appThrowf(LocalizeError("PackageVersion", TEXT("Core")), InOuter->GetName());

    if ( Sandbox && InOuter && !Sandbox->SupportsPackage(InOuter))
    {
        debugf(LocalizeError("Sandbox", TEXT("Core")), InOuter->GetName());
        return NULL;
    }

    return Result;

    unguard;
}



void UObject::StaticShutdownAfterError()
{
    guard(UObject::StaticShutdownAfterError);

    static UBOOL Shutdown = false;

    if ( GObjInitialized && !Shutdown )
    {
        Shutdown = true;

        debugf(NAME_Exit, TEXT("Executing UObject::StaticShutdownAfterError"));

        for (INT i = 0; i < GObjObjects.Num(); ++i )
        {
            UObject *Obj = GObjObjects(i);
            if ( Obj )
                Obj->ConditionalShutdownAfterError();
        }
    }
    unguard;
}


UObject* UObject::GetIndexedObject( INT Index )
{
    guard(UObject::GetIndexedObject);

    if (Index >= 0 && Index < GObjObjects.Num())
        return GObjObjects(Index);
    return NULL;
    unguard;
}


void UObject::GlobalSetProperty( const TCHAR* Value, UClass* Class, UProperty* Property, INT Offset, UBOOL Immediate )
{
    guard(UObject::GlobalSetProperty);
    if ( Immediate )
    {
        for (TObjectIterator<UObject> It; It; ++It)
        {
            if ( It->IsA(Class) )
            {
                Property->ImportText(Value, (BYTE *)(*It) + Offset, PPF_Localized);
                It->PostEditChange();
            }
        }
    }

    Property->ImportText(Value, &Class->Defaults(Offset), PPF_Localized);
    Class->GetDefaultObject()->SaveConfig();
    unguard;
}


void UObject::ExportProperties( FOutputDevice& Out, UClass* ObjectClass, BYTE* Object, INT Indent, UClass* DiffClass, BYTE* Diff )
{
    guard(UObject::ExportProperties);

    check(ObjectClass != NULL);

    for (TFieldIterator<UProperty> It(ObjectClass); It; ++It)
    {
        if (It->Port())
        {
            for (INT i = 0; i < It->ArrayDim; ++i)
            {
                BYTE *D = NULL;
                if ( DiffClass && DiffClass->IsChildOf( It.GetStruct() ) )
                    D = Diff;

                TCHAR Value[4096];
                if ( It->ExportText(i, Value, Object, D, LOAD_NoWarn) )
                {
                    if ( It->IsA(UObjectProperty::StaticClass()) && (It->PropertyFlags & CPF_ExportObject))
                    {
                        UObject *Obj = (UObject *)&Object[It->Offset + i * It->ElementSize];
                        if ( Obj )
                        {
                            if ( !(Obj->GetFlags() & RF_TagImp) )
                            {
                                UExporter::ExportToOutputDevice(Obj, NULL, Out, TEXT("T3D"), Indent + 1);
                                Obj->SetFlags(RF_TagImp);
                            }
                        }
                    }

                    if ( It->ArrayDim != 1 )
                        Out.Logf(TEXT("%s %s(%i)=%s\r\n"), appSpc(Indent), It->GetName(), i, Value);
                    else
                        Out.Logf(TEXT("%s %s=%s\r\n"), appSpc(Indent), It->GetName(), Value);
                }
            }
        }
    }

    unguard;
}



void UObject::ResetConfig( UClass* Class )
{
    guard(UObject::ResetConfig);

    const TCHAR *Filename = NULL;

    if (Class->ClassConfigName == NAME_System)
        Filename = TEXT("Default.ini");
    else if (Class->ClassConfigName == NAME_User)
        Filename = TEXT("DefUser.ini");
    else
        return;

    TCHAR Buffer[0x7FFF];
    if ( GConfig->GetSection(Class->GetPathName(), Buffer, 0x7FFF, Filename) )
    {
        TCHAR *ln = &Buffer[0];

        while(*ln)
        {
            INT lnSz = appStrlen(ln);
            TCHAR *E = appStrstr(ln, TEXT("="));
            if ( E )
            {
                *E = 0;
                GConfig->SetString(Class->GetPathName(), ln, E + 1, *Class->ClassConfigName);
            }

            ln += lnSz + 1;
        }
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        if ( It->IsChildOf(Class) )
            It->GetDefaultObject()->LoadConfig(true);
    }

    for (TObjectIterator<UObject> It; It; ++It)
    {
        if ( It->IsA(Class) )
        {
            It->LoadConfig(true);
            It->PostEditChange();
        }
    }

    unguard;
}



void UObject::GetRegistryObjects( TArray<FRegistryObjectInfo>& Results, UClass* Class, UClass* MetaClass, UBOOL ForceRefresh )
{
    guard(UObject::GetRegistryObjects);

    check(Class);
    check(Class != UClass::StaticClass() || MetaClass);

    CacheDrivers(ForceRefresh);

    const TCHAR *ClassName = Class->GetName();
    const TCHAR *MetaName = TEXT("");

    if (MetaClass)
        MetaName = MetaClass->GetPathName();

    for(int i = 0; i < GObjDrivers.Num(); ++i)
    {
        if ( !appStricmp( *GObjDrivers(i).Class, ClassName) && !appStricmp( *GObjDrivers(i).MetaClass, MetaName) )
            *(new(Results)FRegistryObjectInfo) = GObjDrivers(i);
    }

    unguard;
}



void UObject::GetPreferences( TArray<FPreferencesInfo>& Results, const TCHAR* Category, UBOOL ForceRefresh )
{
    guard(UObject::GetPreferences);

    CacheDrivers(ForceRefresh);
    Results.Empty();

    for(int i = 0; i < GObjPreferences.Num(); ++i)
    {
        if ( !appStricmp( *GObjPreferences(i).ParentCaption, Category) )
            *(new(Results)FPreferencesInfo) = GObjPreferences(i);
    }

    unguard;
}


UBOOL UObject::GetInitialized()
{
    guard(UObject::GetInitialized);

    return GObjInitialized;

    unguard;
}


UPackage* UObject::GetTransientPackage()
{
    guard(UObject::GetTransientPackage);

    return GObjTransientPkg;

    unguard;
}


void UObject::VerifyLinker( ULinkerLoad* Linker )
{
    guard(UObject::VerifyLinker);

    Linker->Verify();

    unguard;
}


void UObject::ProcessRegistrants()
{
    guard(UObject::ProcessRegistrants);

    if ( GObjRegisterCount++ == 0 )
    {
        while ( GAutoRegister )
        {
            GObjRegistrants( GObjRegistrants.Add() ) = GAutoRegister;
            GAutoRegister = (UObject *)GAutoRegister->_LinkerIndex;
        }

        for (INT i = 0; i < GObjRegistrants.Num(); ++i )
            GObjRegistrants(i)->ConditionalRegister();

        GObjRegistrants.Empty();

        check(!GAutoRegister);
    }
    --GObjRegisterCount;

    unguard;
}


void UObject::BindPackage( UPackage* Pkg )
{
    guard(UObject::BindPackage);

    if ( !Pkg->DllHandle && !Pkg->Outer && !Pkg->AttemptedBind )
    {
        TCHAR PathName[256];
        appSprintf(PathName, TEXT("%s%s"), appBaseDir(), Pkg->GetName());

        GObjNoRegister = false;

        Pkg->AttemptedBind = 1;
        Pkg->DllHandle = appGetDllHandle(PathName);

        GObjNoRegister = true;

        if ( Pkg->DllHandle )
        {
            debugf(NAME_Log, TEXT("Bound to %s%s"), Pkg->GetName(), DLLEXT);
            ProcessRegistrants();
        }
    }

    unguard;
}


const TCHAR* UObject::GetLanguage()
{
    guard(UObject::GetLanguage);

    return GLanguage;

    unguard;
}


void UObject::SetLanguage( const TCHAR* LanguageExt )
{
    guard(UObject::SetLanguage);

    if ( appStricmp(LanguageExt, GLanguage) )
    {
        appStrcpy(GLanguage, LanguageExt);
        appStrcpy(GNone, LocalizeGeneral(TEXT("None"), TEXT("Core")));
        appStrcpy(GTrue, LocalizeGeneral(TEXT("True"), TEXT("Core")));
        appStrcpy(GFalse, LocalizeGeneral(TEXT("False"), TEXT("Core")));
        appStrcpy(GYes, LocalizeGeneral(TEXT("Yes"), TEXT("Core")));
        appStrcpy(GNo, LocalizeGeneral(TEXT("No"), TEXT("Core")));


        for (TObjectIterator<UObject> It; It; ++It)
            It->LanguageChange();
    }

    unguard;
}


