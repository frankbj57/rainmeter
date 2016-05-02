/* Copyright (C) 2004 Rainmeter Project Developers
 *
 * This Source Code Form is subject to the terms of the GNU General Public
 * License; either version 2 of the License, or (at your option) any later
 * version. If a copy of the GPL was not distributed with this file, You can
 * obtain one at <https://www.gnu.org/licenses/gpl-2.0.html>. */

#include <windows.h>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>

#include "../PluginPerfMon/Titledb.h"
#include "../PluginPerfMon/PerfSnap.h"
#include "../PluginPerfMon/PerfObj.h"
#include "../PluginPerfMon/PerfCntr.h"
#include "../PluginPerfMon/ObjList.h"
#include "../PluginPerfMon/ObjInst.h"
#include "../API/RainmeterAPI.h"
#include "../../Common/RawString.h"

struct MeasureData
{
	std::vector<RawString> includes;
	std::vector<RawString> excludes;
	RawString includesCache;
	RawString excludesCache;
	int topProcess;
	RawString topProcessName;
	LONGLONG topProcessValue;

	MeasureData() :
		topProcess(-1),
		topProcessValue()
	{
	}
};

typedef int processId_type;

struct ProcessValues
{
	RawString name;
	processId_type ProcessId = 0;
	LONGLONG oldValue = 0LL;
	LONGLONG newValue = 0LL;
	LONG generation = 0;
};

static CPerfTitleDatabase g_CounterTitles(PERF_TITLE_COUNTER);
std::map<processId_type, ProcessValues> g_Processes;

static LONG Generation = 0;

void UpdateProcesses();

void SplitName(WCHAR* names, std::vector<RawString>& splittedNames)
{
	WCHAR* context = nullptr;
	WCHAR* token = wcstok(names, L";", &context);
	while (token != nullptr)
	{
		splittedNames.push_back(token);
		token = wcstok(nullptr, L";", &context);
	}
}

PLUGIN_EXPORT void Initialize(void** data, void* rm)
{
	MeasureData* measure = new MeasureData;
	*data = measure;
}

PLUGIN_EXPORT void Reload(void* data, void* rm, double* maxValue)
{
	MeasureData* measure = (MeasureData*)data;
	bool changed = false;

	LPCWSTR value = RmReadString(rm, L"CPUInclude", L"");
	if (_wcsicmp(value, measure->includesCache.c_str()) != 0)
	{
		measure->includesCache = value;
		measure->includes.clear();

		WCHAR* buffer = _wcsdup(value);
		SplitName(buffer, measure->includes);
		delete buffer;
		changed = true;
	}

	value = RmReadString(rm, L"CPUExclude", L"");
	if (_wcsicmp(value, measure->excludesCache.c_str()) != 0)
	{
		measure->excludesCache = value;
		measure->excludes.clear();

		WCHAR* buffer = _wcsdup(value);
		SplitName(buffer, measure->excludes);
		delete buffer;
		changed = true;
	}

	int topProcess = RmReadInt(rm, L"TopProcess", 0);
	if (topProcess != measure->topProcess)
	{
		measure->topProcess = topProcess;
		changed = true;
	}

	if (changed)
	{
		*maxValue = 10000000;	// The values are 100 * 100000
	}
}

bool CheckProcess(MeasureData* measure, const WCHAR* name)
{
	if (measure->includes.empty())
	{
		for (size_t i = 0; i < measure->excludes.size(); ++i)
		{
			if (_wcsicmp(measure->excludes[i].c_str(), name) == 0)
			{
				return false;		// Exclude
			}
		}
		return true;	// Include
	}
	else
	{
		for (size_t i = 0; i < measure->includes.size(); ++i)
		{
			if (_wcsicmp(measure->includes[i].c_str(), name) == 0)
			{
				return true;	// Include
			}
		}
	}
	return false;
}

ULONGLONG _GetTickCount64()
{
	typedef ULONGLONG (WINAPI * FPGETTICKCOUNT64)();
	static FPGETTICKCOUNT64 c_GetTickCount64 = (FPGETTICKCOUNT64)GetProcAddress(GetModuleHandle(L"kernel32"), "GetTickCount64");

	if (c_GetTickCount64)
	{
		return c_GetTickCount64();
	}
	else
	{
		static ULONGLONG lastTicks = 0;
		ULONGLONG ticks = GetTickCount();
		while (ticks < lastTicks) ticks += 0x100000000;
		lastTicks = ticks;
		return ticks;
	}
}

PLUGIN_EXPORT double Update(void* data)
{
	MeasureData* measure = (MeasureData*)data;
	static ULONGLONG oldTime = 0;

	// Only update twice per second
	ULONGLONG time = _GetTickCount64();
	if (oldTime == 0 || time - oldTime > 500)
	{
		UpdateProcesses();
		oldTime = time;
	}

	LONGLONG newValue = 0;

	for (auto it = g_Processes.begin(); it != g_Processes.end(); ++it)
	{
		// Check process include/exclude
		if (CheckProcess(measure, it->second.name.c_str()))
		{
			if (it->second.oldValue != 0)
			{
				LONGLONG value = it->second.newValue - it->second.oldValue;

				if (measure->topProcess == 0)
				{
					// Add all values together
					newValue += value;
				}
				else
				{
					// Find the top process
					if (newValue < value)
					{
						newValue = value;
						measure->topProcessName = it->second.name;
						measure->topProcessValue = newValue;
					}
				}
			}
		}
	}

	return (double)newValue;
}

PLUGIN_EXPORT LPCWSTR GetString(void* data)
{
	MeasureData* measure = (MeasureData*)data;

	if (measure->topProcess == 2)
	{
		return measure->topProcessName.c_str();
	}

	return nullptr;
}

PLUGIN_EXPORT void Finalize(void* data)
{
	MeasureData* measure = (MeasureData*)data;
	delete measure;

	CPerfSnapshot::CleanUp();
}

/*
  This updates the process status
*/
template <typename counterType>
bool GetCounterData(const CPerfObjectInstance * pInstance, PCTSTR counterName, counterType & counterValue)
{
	std::unique_ptr<CPerfCounter> pPerfCntr(pInstance->GetCounterByName(counterName));
	if (pPerfCntr != nullptr)
	{
		if (pPerfCntr->GetSize() == sizeof counterType)
		{
			BYTE data[sizeof counterType];

			pPerfCntr->GetData(data, sizeof counterType, nullptr);

			counterValue = *((counterType *)data);

			return true;
		}
	}
	
	return false;
}

void UpdateProcesses()
{
	BYTE data[256];
	WCHAR name[256];

	CPerfSnapshot snapshot(&g_CounterTitles);
	CPerfObjectList objList(&snapshot, &g_CounterTitles);

	if (snapshot.TakeSnapshot(L"Process"))
	{
		std::unique_ptr<CPerfObject> pPerfObj(objList.GetPerfObject(L"Process"));

		if (pPerfObj)
		{
			// Update generation
			Generation++;

			for (std::unique_ptr<CPerfObjectInstance> pObjInst(pPerfObj->GetFirstObjectInstance());
				pObjInst != nullptr;
				pObjInst.reset(pPerfObj->GetNextObjectInstance()))
			{
				if (pObjInst->GetObjectInstanceName(name, 256))
				{
					if (_wcsicmp(name, L"_Total") == 0)
					{
						continue;
					}

					ULONGLONG procTime;
					GetCounterData(pObjInst.get(), L"% Processor Time", procTime);

					std::unique_ptr<CPerfCounter> pPerfCntr(pObjInst->GetCounterByName(L"% Processor Time"));
					if (pPerfCntr != nullptr)
					{
						pPerfCntr->GetData(data, 256, nullptr);

						if (pPerfCntr->GetSize() == 8)
						{
							ULONGLONG newData = *(ULONGLONG*)data;

							std::unique_ptr<CPerfCounter> pProcessIdData(pObjInst->GetCounterByName(L"ID Process"));
							if (pProcessIdData != nullptr)
							{
								if (pProcessIdData->GetSize() == sizeof(processId_type))
								{
									pProcessIdData->GetData(data, 256, nullptr);
									processId_type id = *(processId_type*)data;

									// Update new or old processvalues in g_Processes
									// [id] will create a new entry, if one doesn't exist
									auto it = g_Processes.find(id);
									if (it != g_Processes.end())
									{
										// Already exists, only update necessary fields
										it->second.oldValue = it->second.newValue;
										it->second.newValue = newData;
										it->second.generation = Generation;
									}
									else
									{
										ProcessValues newValues;

										newValues.name = name;
										newValues.newValue = newData;
										newValues.generation = Generation;
										newValues.ProcessId = id;

										g_Processes.emplace(id, newValues);
									}
								}
							}
						}
					}
				}
			}

			// Clean up dead processes
			for (auto it = g_Processes.begin(); it != g_Processes.end();)
			{
				if (it->second.generation != Generation)
				{
					it = g_Processes.erase(it);
				}
				else
				{
					it++;
				}
			}
		}
	}
}
