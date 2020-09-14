/*******************************************************************************
 * Copyright (c) 1991, 2016 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "modronapicore.hpp"
#include "VerboseWriterFileLoggingSynchronous.hpp"

#include "EnvironmentBase.hpp"
#include "GCExtensionsBase.hpp"
#include "VerboseManager.hpp"

#include <string.h>
#include "GCExtensions.hpp"
#include "Heap.hpp"
#include "HeapRegionManager.hpp"
#include "ObjectAccessBarrier.hpp"

MM_VerboseWriterFileLoggingSynchronous::MM_VerboseWriterFileLoggingSynchronous(MM_EnvironmentBase *env, MM_VerboseManager *manager)
	:MM_VerboseWriterFileLogging(env, manager, VERBOSE_WRITER_FILE_LOGGING_SYNCHRONOUS)
	,_logFileDescriptor(-1)
{
	/* No implementation */
}

/**
 * Create a new MM_VerboseWriterFileLoggingSynchronous instance.
 * @return Pointer to the new MM_VerboseWriterFileLoggingSynchronous.
 */
MM_VerboseWriterFileLoggingSynchronous *
MM_VerboseWriterFileLoggingSynchronous::newInstance(MM_EnvironmentBase *env, MM_VerboseManager *manager, char *filename, uintptr_t numFiles, uintptr_t numCycles)
{
	MM_GCExtensionsBase *extensions = MM_GCExtensionsBase::getExtensions(env->getOmrVM());
	
	MM_VerboseWriterFileLoggingSynchronous *agent = (MM_VerboseWriterFileLoggingSynchronous *)extensions->getForge()->allocate(sizeof(MM_VerboseWriterFileLoggingSynchronous), OMR::GC::AllocationCategory::DIAGNOSTIC, OMR_GET_CALLSITE());
	if(agent) {
		new(agent) MM_VerboseWriterFileLoggingSynchronous(env, manager);
		if(!agent->initialize(env, filename, numFiles, numCycles)) {
			agent->kill(env);
			agent = NULL;
		}
	}
	return agent;
}

/**
 * Initializes the MM_VerboseWriterFileLoggingSynchronous instance.
 * @return true on success, false otherwise
 */
bool
MM_VerboseWriterFileLoggingSynchronous::initialize(MM_EnvironmentBase *env, const char *filename, uintptr_t numFiles, uintptr_t numCycles)
{
	return MM_VerboseWriterFileLogging::initialize(env, filename, numFiles, numCycles);
}

/**
 * Tear down the structures managed by the MM_VerboseWriterFileLoggingSynchronous.
 * Tears down the verbose buffer.
 */
void
MM_VerboseWriterFileLoggingSynchronous::tearDown(MM_EnvironmentBase *env)
{
	MM_VerboseWriterFileLogging::tearDown(env);
}

/**
 * Opens the file to log output to and prints the header.
 * @return true on sucess, false otherwise
 */
bool
MM_VerboseWriterFileLoggingSynchronous::openFile(MM_EnvironmentBase *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());	
	MM_GCExtensionsBase* extensions = env->getExtensions();
	const char* version = omrgc_get_version(env->getOmrVM());
	
	char *filenameToOpen = expandFilename(env, _currentFile);
	if (NULL == filenameToOpen) {
		return false;
	}

	omrtty_printf("!@: openFile start %s\n",filenameToOpen);
	
	_logFileDescriptor = omrfile_open(filenameToOpen, EsOpenRead | EsOpenWrite | EsOpenCreate | EsOpenTruncate, 0666);
	if(-1 == _logFileDescriptor) {
		char *cursor = filenameToOpen;
		/**
		 * This may have failed due to directories in the path not being available.
		 * Try to create these directories and attempt to open again before failing.
		 */
		while ( (cursor = strchr(++cursor, DIR_SEPARATOR)) != NULL ) {
			*cursor = '\0';
			omrfile_mkdir(filenameToOpen);
			*cursor = DIR_SEPARATOR;
		}

		/* Try again */
		_logFileDescriptor = omrfile_open(filenameToOpen, EsOpenRead | EsOpenWrite | EsOpenCreate | EsOpenTruncate, 0666);
		if (-1 == _logFileDescriptor) {
			_manager->handleFileOpenError(env, filenameToOpen);
			extensions->getForge()->free(filenameToOpen);
			return false;
		}
	}

	extensions->getForge()->free(filenameToOpen);
	
	omrfile_printf(_logFileDescriptor, getHeader(env), version);
	const char* temp="!@: MM_VerboseWriterFileLoggingSynchronous::openFile\n\n";
	omrfile_printf(_logFileDescriptor, temp, version);

	MM_GCExtensions *extensionsExt = MM_GCExtensions::getExtensions(env);
	UDATA beatMicro = 0;
	UDATA timeWindowMicro = 0;
	UDATA targetUtilizationPercentage = 0;
	UDATA gcInitialTrigger = 0;
	UDATA headRoom = 0;
#if defined(J9VM_GC_REALTIME)
	beatMicro = extensions->beatMicro;
	timeWindowMicro = extensions->timeWindowMicro;
	targetUtilizationPercentage = extensions->targetUtilizationPercentage;
	gcInitialTrigger = extensions->gcInitialTrigger;
	headRoom = extensions->headRoom;
#endif /* J9VM_GC_REALTIME */

	UDATA numaNodes = extensions->_numaManager.getAffinityLeaderCount();

	UDATA regionSize = extensionsExt->getHeap()->getHeapRegionManager()->getRegionSize();
	UDATA regionCount = extensionsExt->getHeap()->getHeapRegionManager()->getTableRegionCount();

	UDATA arrayletLeafSize = 0;
	arrayletLeafSize = env->getOmrVM()->_arrayletLeafSize;

	const char* temp2="!@: Before Trigger\n\n";
	omrfile_printf(_logFileDescriptor, temp2, version);
	// !@!@ Trigger From OMR
	TRIGGER_J9HOOK_MM_OMR_INITIALIZED_NOLOCK(
		// Same
		extensions->omrHookInterface,

		// Not sure
		env->getOmrVMThread(),

		// j9time_hires_clock(),
		omrtime_hires_clock(),

		// j9gc_get_gcmodestring(vm),
		extensions->gcModeString,

		0, /* unused */

		// j9gc_get_maximum_heap_size(vm),
		extensions->memoryMax,

		// j9gc_get_initial_heap_size(vm),
		extensions->initialMemorySize,

		// j9sysinfo_get_physical_memory(),
		omrsysinfo_get_physical_memory(),

		// j9sysinfo_get_number_CPUs_by_type(J9PORT_CPU_ONLINE),
		// Should it be `OMRPORT_CPU_ONLINE`?
		// omrsysinfo_get_number_CPUs_by_type(OMRPORT_CPU_ONLINE),
		omrsysinfo_get_number_CPUs_by_type(OMRPORT_CPU_ONLINE),

		extensions->gcThreadCount,

		// j9sysinfo_get_CPU_architecture(),
		omrsysinfo_get_CPU_architecture(),

		// j9sysinfo_get_OS_type(),
		omrsysinfo_get_OS_type(),

		// j9sysinfo_get_OS_version(),
		omrsysinfo_get_OS_version(),

		// extensions->accessBarrier->compressedPointersShift(),
		extensionsExt->accessBarrier->compressedPointersShift(),
		// 0,

		// Same
		beatMicro,
		timeWindowMicro,		
		targetUtilizationPercentage,
		gcInitialTrigger,
		headRoom,
		extensions->heap->getPageSize(),		
		getPageTypeString(extensions->heap->getPageFlags()),
		extensions->requestedPageSize,
		getPageTypeString(extensions->requestedPageFlags),
		numaNodes,
		regionSize,
		regionCount,
		arrayletLeafSize);

		const char* temp3="!@: After Trigger\n\n";
		omrfile_printf(_logFileDescriptor, temp3, version);
		omrtty_printf("!@: openFile end %s\n",filenameToOpen);
	
	return true;
}

/**
 * Prints the footer and closes the file being logged to.
 */
void
MM_VerboseWriterFileLoggingSynchronous::closeFile(MM_EnvironmentBase *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	
	if(-1 != _logFileDescriptor) {
		omrfile_write_text(_logFileDescriptor, getFooter(env), strlen(getFooter(env)));
		omrfile_write_text(_logFileDescriptor, "\n", strlen("\n"));
		omrfile_close(_logFileDescriptor);
		_logFileDescriptor = -1;
	}
}

void
MM_VerboseWriterFileLoggingSynchronous::outputString(MM_EnvironmentBase *env, const char* string)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());

	if(-1 == _logFileDescriptor) {
		/* we open the file at the end of the cycle so can't have a final empty file at the end of a run */
		openFile(env);
	}

	if(-1 != _logFileDescriptor){
		omrfile_write_text(_logFileDescriptor, string, strlen(string));
	} else {
		omrfile_write_text(OMRPORT_TTY_ERR, string, strlen(string));
	}
}
