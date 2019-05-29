
class FArchiveFindCulprit : public FArchive
{
public:
	FArchiveFindCulprit(UObject* InFind)
	:	Find			( InFind )
	,	Count			( 0 )
	{}
	
	FArchive& operator<<( UObject*& Obj )
	{
		guard(FArchiveFindCulprit<<UObject);
		if (Obj == Find)
			++Count;
		return *this;
		unguard;
	};
	
	INT GetCount() const
	{
		return Count;
	}

protected:
	UObject*		Find;
	INT				Count;
};






struct FTraceRouteRecord
{
	INT				Depth;
	UObject *		Referencer;
	
	FTraceRouteRecord()
	:	Depth		( 0 )
	,	Referencer	( NULL )
	{}
	
	FTraceRouteRecord(INT InDepth, UObject *InReferencer)
	:	Depth		( InDepth )
	,	Referencer	( InReferencer )
	{}
	
	FTraceRouteRecord& operator=( const FTraceRouteRecord& Other )
	{
		guardSlow(FTraceRouteRecord::operator=);
		Depth     = Other.Depth;
		Referencer = Other.Referencer;
		return *this;
		unguardSlow;
	}
};


class FArchiveTraceRoute : public FArchive
{
public:
	FArchiveTraceRoute(TMap<UObject*, FTraceRouteRecord> &InRoutes)
	:	Routes			( InRoutes )
	,	Depth			( 0 )
	,	Prev			( NULL )
	{
		for (TObjectIterator<UObject> It; It; ++It)
			It->SetFlags(RF_TagExp);
		
		UObject::SerializeRootSet(*this, RF_Native, 0);
		
		for (TObjectIterator<UObject> It; It; ++It)
			It->ClearFlags(RF_TagExp);
	}
	
	FArchive& operator<<( UObject*& Obj )
	{
		guard(FArchiveTraceRoute<<UObject);
		
		if ( Obj )
		{
			FTraceRouteRecord *Rec = Routes.Find(Obj);
			if (!Rec || Rec->Depth > Depth)
				Routes.Set(Obj, FTraceRouteRecord(Depth, Prev));
		}

		if ( Obj && (Obj->GetFlags() & RF_TagExp) )
		{
			Obj->ClearFlags(RF_TagExp);
			UObject* Tmp = Prev;
			++Depth;
			Obj->Serialize(*this);
			Prev = Tmp;
			--Depth;
		}

		return *this;
		unguard;
	};
	
	static TArray<UObject*> FindShortestRootPath(UObject* Obj)
	{
		TMap<UObject*, FTraceRouteRecord> Routes;
		FArchiveTraceRoute Rt(Routes);
		
		TArray<UObject*> Result;
		
		if(Routes.Find(Obj))
		{
			Result( Result.Add() ) = Obj;
			UObject* ItObj = Obj;
			for(FTraceRouteRecord *Node = Routes.Find(ItObj); Node->Depth != 0; Node = Routes.Find(ItObj))
			{
				ItObj = Node->Referencer;
				Result.Insert(0);
				Result(0) = ItObj;
			}
		}
		return Result;
	};

protected:
	TMap<UObject*, FTraceRouteRecord> &Routes;
	INT				Depth;
	UObject*		Prev;
};






class FArchiveShowReferences : public FArchive
{
public:
	FArchiveShowReferences(FOutputDevice& InAr, UObject* inOuter, UObject* inSource, TArray<UObject*>& inExclude)
	:	Ar				( InAr )
	,	Parent			( inOuter )
	,	Obj				( inSource )
	,	Exclude			( inExclude )
	,	DidRef			( false )
	{
	}
	
	FArchive& operator<<( UObject*& Obj )
	{
		guard(FArchiveShowReferences<<UObject);
		if ( !Obj )
			return *this;

		if ( Obj->GetOuter() == Parent )
			return *this;
			
		for (INT i = 0; i < Exclude.Num(); ++i )
		{
			if (Exclude(i) == Obj->GetOuter())
				return *this;
		}
		
		if ( !DidRef )
			Ar.Logf(TEXT("   %s references:"), Obj->GetFullName());

		Ar.Logf(TEXT("      %s"), Obj->GetFullName());
		DidRef = true;
				
		return *this;
		unguard;
	};
	
protected:
	UBOOL			DidRef;
	FOutputDevice&	Ar;
	UObject*		Parent;
	UObject*		Obj;
	TArray<UObject*>&	Exclude;
};



class FArchiveCountMem : public FArchive
{
public:
	FArchiveCountMem()
	:	Num				( 0 )
	,	Max				( 0 )
	{
	}
	
	SIZE_T GetNum()
	{
		return Num;
	}
	
	SIZE_T GetMax()
	{
		return Max;
	}
	
	void CountBytes( SIZE_T InNum, SIZE_T InMax )
	{
		Num += InNum;
		Max += InMax;
	}
	
protected:
	SIZE_T			Num;
	SIZE_T			Max;
};



class FArchiveSaveTagImports : public FArchive
{
public:
	FArchiveSaveTagImports(DWORD InContextFlags, ULinkerSave* InLinker)
	:	ContextFlags		( InContextFlags )
	,	Linker				( InLinker )
	{
		ArIsPersistent = 1;
        ArIsSaving = 1;
	}
	
	FArchive& operator<<( UObject*& Object )
	{
		guard(FArchiveSaveTagImports<<UObject);

		if ( !Object )
			return *this;
		
		if ( Object->IsPendingKill() )
			return *this;
		
		if ( Object->GetFlags() & RF_Transient && !(Object->GetFlags() & RF_Public) )
			return *this;
		
		++Linker->ObjectIndices(Object->GetIndex());
	
		if ( Object->GetFlags() & RF_TagExp )
			return *this;
		
		Object->SetFlags(RF_TagImp);

		if ( !(Object->GetFlags() & RF_NotForEdit) )
			Object->SetFlags(RF_LoadForEdit);

		if ( !(Object->GetFlags() & RF_NotForClient) )
			Object->SetFlags(RF_LoadForClient);

		if ( !(Object->GetFlags() & RF_NotForServer) )
			Object->SetFlags(RF_LoadForServer);

		UObject* Outer = Object->GetOuter();
		if ( Outer )
			(*this) << Outer;
		
		return *this;
		unguard;
	};
	
	FArchive& operator<<( FName& N )
	{
		guard(FArchiveSaveTagImports<<FName);
		
		N.SetFlags(ContextFlags | RF_TagExp);
		++Linker->NameIndices(N.GetIndex());
		
		return *this;
		unguard;
	};
	
protected:
	DWORD			ContextFlags;
	ULinkerSave*	Linker;
};



class FArchiveSaveTagExports : public FArchive
{
public:
	FArchiveSaveTagExports(UObject* InParent)
	:	Parent				( InParent )
	{
		ArForEdit = 1;
		ArIsTrans = 1;
	}
	
	FArchive& operator<<( UObject*& Object )
	{
		guard(FArchiveSaveTagExports<<UObject);
	
		if ( !Object )
			return *this;
		
		if ( Object->IsPendingKill() )
			debugf(NAME_Warning, TEXT("trying to archive deleted object: %s"), Object->GetName());
	
		if ( !Object || Object->IsPendingKill() )
			return *this;
	
		if ( !Object->IsIn(Parent) )
			return *this;
	
		if ( Object->GetFlags() & (RF_Transient | RF_TagExp) )
			return *this;
		
		Object->SetFlags(RF_TagExp);
	
		if ( !(Object->GetFlags() & RF_NotForEdit) )
			Object->SetFlags(RF_LoadForEdit);

		if ( !(Object->GetFlags() & RF_NotForClient) )
			Object->SetFlags(RF_LoadForClient);

		if ( !(Object->GetFlags() & RF_NotForServer) )
			Object->SetFlags(RF_LoadForServer);
	
		UClass *Cls = Object->GetClass();
		UObject *Outer = Object->GetOuter();
		(*this) << Cls << Outer;
	
		Object->Serialize( *this );
		return *this;
		unguard;
	};
		
protected:
	UObject*		Parent;
};





