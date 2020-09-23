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
#include "VerboseHandlerOutput.hpp"

#include <string.h>
#include "GCExtensions.hpp"
#include "Heap.hpp"
#include "HeapRegionManager.hpp"
#include "ObjectAccessBarrier.hpp"

class MM_VerboseHandlerOutput;

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
	_manager->handleFileOpenSuccess(env, filenameToOpen);
	extensions->getForge()->free(filenameToOpen);
	// omrfile_write_text
	omrfile_printf(_logFileDescriptor, getHeader(env), version);
	omrfile_printf(_logFileDescriptor, getHeader(env));
	const char* temp="!@: MM_VerboseWriterFileLoggingSynchronous::openFile\n\n";
	omrfile_printf(_logFileDescriptor, temp);
	
	// omrfile_printf(_logFileDescriptor, getInitial(env));

	// writer->formatAndOutput(env, 0, "!@: new INIT Start\n");	
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

	omrfile_printf(_logFileDescriptor, "!@: new INIT Start\n");
	char tagTemplate[200];

	_manager->setInitializedTime(omrtime_hires_clock());
	// VerboseHandlerOutput::getTagTemplate(tagTemplate, sizeof(tagTemplate), _manager->getIdAndIncrement(), omrtime_current_time_millis());
	MM_VerboseHandlerOutput *_verboseHandlerOutput = MM_VerboseHandlerOutput::newInstance(env, _manager);
	_verboseHandlerOutput->getTagTemplate(tagTemplate, sizeof(tagTemplate), _manager->getIdAndIncrement(), omrtime_current_time_millis());
	omrfile_printf(_logFileDescriptor, "<initialized %s>\n", tagTemplate);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"gcPolicy\" value=\"%s\" />\n", extensions->gcModeString);
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	if (extensions->isConcurrentScavengerEnabled()) {
		omrfile_printf(_logFileDescriptor, "\t<attribute name=\"concurrentScavenger\" value=\"%s\" />", extensions->gcModeString,
#if defined(S390) || defined(J9ZOS390)
				extensions->concurrentScavengerHWSupport ?
				"enabled, with H/W assistance" :
				"enabled, without H/W assistance");
#else /* defined(S390) || defined(J9ZOS390) */
				"enabled");
#endif /* defined(S390) || defined(J9ZOS390) */
	}
#endif /* OMR_GC_CONCURRENT_SCAVENGER */

	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"maxHeapSize\" value=\"0x%zx\" />\n", extensions->memoryMax);
	// writer->formatAndOutput(env, 1, "<attribute name=\"maxHeapSize\" value=\"0x%zx\" />", event->maxHeapSize);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"initialHeapSize\" value=\"0x%zx\" />\n", extensions->initialMemorySize);
	// writer->formatAndOutput(env, 1, "<attribute name=\"initialHeapSize\" value=\"0x%zx\" />", event->initialHeapSize);

#if defined(OMR_GC_COMPRESSED_POINTERS)
	if (env->compressObjectReferences()) {
		// writer->formatAndOutput(env, 1, "<attribute name=\"compressedRefs\" value=\"true\" />");
		omrfile_printf(_logFileDescriptor, "\t<attribute name=\"compressedRefs\" value=\"true\" />\n");
		// writer->formatAndOutput(env, 1, "<attribute name=\"compressedRefsDisplacement\" value=\"0x%zx\" />", 0);
		omrfile_printf(_logFileDescriptor, "\t<attribute name=\"compressedRefsDisplacement\" value=\"0x%zx\" />\n", 0);
		// writer->formatAndOutput(env, 1, "<attribute name=\"compressedRefsShift\" value=\"0x%zx\" />", event->compressedPointersShift);
		omrfile_printf(_logFileDescriptor, "\t<attribute name=\"compressedRefsShift\" value=\"0x%zx\" />\n", extensionsExt->accessBarrier->compressedPointersShift());
	} else
#endif /* defined(OMR_GC_COMPRESSED_POINTERS) */
	{
		// writer->formatAndOutput(env, 1, "<attribute name=\"compressedRefs\" value=\"false\" />");
		omrfile_printf(_logFileDescriptor, "\t<attribute name=\"compressedRefs\" value=\"false\" />\n");
	}
	// writer->formatAndOutput(env, 1, "<attribute name=\"pageSize\" value=\"0x%zx\" />", event->heapPageSize);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"pageSize\" value=\"0x%zx\" />\n", extensions->heap->getPageSize());
	// writer->formatAndOutput(env, 1, "<attribute name=\"pageType\" value=\"%s\" />", event->heapPageType);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"pageType\" value=\"%s\" />\n", getPageTypeString(extensions->heap->getPageFlags()));
	// writer->formatAndOutput(env, 1, "<attribute name=\"requestedPageSize\" value=\"0x%zx\" />", event->heapRequestedPageSize);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"requestedPageSize\" value=\"0x%zx\" />\n", extensions->requestedPageSize);
	// writer->formatAndOutput(env, 1, "<attribute name=\"requestedPageType\" value=\"%s\" />", event->heapRequestedPageType);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"requestedPageType\" value=\"%s\" />\n", getPageTypeString(extensions->requestedPageFlags));
	// writer->formatAndOutput(env, 1, "<attribute name=\"gcthreads\" value=\"%zu\" />", event->gcThreads);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"gcthreads\" value=\"%zu\" />\n", extensions->gcThreadCount);
	if (gc_policy_gencon == extensions->configurationOptions._gcPolicy) {
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
		if (extensions->isConcurrentScavengerEnabled()) {
			// writer->formatAndOutput(env, 1, "<attribute name=\"gcthreads Concurrent Scavenger\" value=\"%zu\" />", _extensions->concurrentScavengerBackgroundThreads);
			omrfile_printf(_logFileDescriptor, "\t<attribute name=\"gcthreads Concurrent Scavenger\" value=\"%zu\" />\n", extensions->concurrentScavengerBackgroundThreads);
		}
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
	if (extensions->isConcurrentMarkEnabled()) {
			// writer->formatAndOutput(env, 1, "<attribute name=\"gcthreads Concurrent Mark\" value=\"%zu\" />", _extensions->concurrentBackground);
			omrfile_printf(_logFileDescriptor, "\t<attribute name=\"gcthreads Concurrent Mark\" value=\"%zu\" />\n", extensions->concurrentBackground);
		}
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */
	}

	// writer->formatAndOutput(env, 1, "<attribute name=\"packetListSplit\" value=\"%zu\" />", _extensions->packetListSplit);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"packetListSplit\" value=\"%zu\" />\n", extensions->packetListSplit);
#if defined(OMR_GC_MODRON_SCAVENGER)
	// writer->formatAndOutput(env, 1, "<attribute name=\"cacheListSplit\" value=\"%zu\" />", _extensions->cacheListSplit);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"cacheListSplit\" value=\"%zu\" />\n", extensions->cacheListSplit);
#endif /* OMR_GC_MODRON_SCAVENGER */
	// writer->formatAndOutput(env, 1, "<attribute name=\"splitFreeListSplitAmount\" value=\"%zu\" />", _extensions->splitFreeListSplitAmount);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"splitFreeListSplitAmount\" value=\"%zu\" />\n", extensions->splitFreeListSplitAmount);
	// writer->formatAndOutput(env, 1, "<attribute name=\"numaNodes\" value=\"%zu\" />", event->numaNodes);
	omrfile_printf(_logFileDescriptor, "\t<attribute name=\"numaNodes\" value=\"%zu\" />\n", numaNodes);

	// writer->formatAndOutput(env, 1, "<system>");
	omrfile_printf(_logFileDescriptor, "\t<system>\n");
	// writer->formatAndOutput(env, 2, "<attribute name=\"physicalMemory\" value=\"%llu\" />", event->physicalMemory);
	omrfile_printf(_logFileDescriptor, "\t\t<attribute name=\"physicalMemory\" value=\"%llu\" />\n", omrsysinfo_get_physical_memory());
	// writer->formatAndOutput(env, 2, "<attribute name=\"numCPUs\" value=\"%zu\" />", event->numCPUs);
	omrfile_printf(_logFileDescriptor, "\t\t<attribute name=\"numCPUs\" value=\"%zu\" />\n", omrsysinfo_get_number_CPUs_by_type(OMRPORT_CPU_ONLINE));
	// writer->formatAndOutput(env, 2, "<attribute name=\"architecture\" value=\"%s\" />", event->architecture);
	omrfile_printf(_logFileDescriptor, "\t\t<attribute name=\"architecture\" value=\"%s\" />\n", omrsysinfo_get_CPU_architecture());
	// writer->formatAndOutput(env, 2, "<attribute name=\"os\" value=\"%s\" />", event->os);
	omrfile_printf(_logFileDescriptor, "\t\t<attribute name=\"os\" value=\"%s\" />\n", omrsysinfo_get_OS_type());
	// writer->formatAndOutput(env, 2, "<attribute name=\"osVersion\" value=\"%s\" />", event->osVersion);
	omrfile_printf(_logFileDescriptor, "\t\t<attribute name=\"osVersion\" value=\"%s\" />\n", omrsysinfo_get_OS_version());
	// writer->formatAndOutput(env, 1, "</system>");
	omrfile_printf(_logFileDescriptor, "\t</system>\n");
	omrfile_printf(_logFileDescriptor, "\t<vmargs>\n");
	omrfile_printf(_logFileDescriptor, "\t<vmargs>\n");
	omrfile_printf(_logFileDescriptor, "\t</vmargs>\n");

	omrfile_printf(_logFileDescriptor, "\n!@: new INIT End\n");

	// JavaVMInitArgs* vmArgs = env->getOmrVMThread()->vmArgsArray->actualVMArgs;
	

	


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
