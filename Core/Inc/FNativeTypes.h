/*=============================================================================
	FNativeTypes.h: Unreal typedefs for native OS abstraction classes.

	Revision history:
		* Created by Stéphan Kochen
=============================================================================*/

#if WIN32
	#include "FFileManagerWindows.h"
	typedef FFileManagerWindows FFileManagerNative;
	#include "FMallocWindows.h"
	typedef FMallocAnsi FMallocNative;
#elif __LINUX_X86__
	#include "FFileManagerLinux.h"
	typedef FFileManagerLinux FFileManagerNative;
	#include "FMallocAnsi.h"
	typedef FMallocAnsi FMallocNative;
#else
	#include "FFileManagerAnsi.h"
	typedef FFileManagerAnsi FFileManagerNative;
	#include "FMallocAnsi.h"
	typedef FMallocAnsi FMallocNative;
#endif