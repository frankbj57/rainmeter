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
	LONGLONG oldValue = 0LL;
	LONGLONG newValue = 0LL;
	LONG generation = 0;
};

// Use in std::find algorithm for Rawstring vectors
bool operator==(const RawString &left, const RawString &right)
{
	return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

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
		auto it = std::find(measure->excludes.begin(), measure->excludes.end(), name);

		if (it != measure->excludes.end())
				return false;		// Exclude

		return true;	// Include
	}
	else
	{
		auto it = std::find(measure->includes.begin(), measure->includes.end(), name);

		if (it != measure->includes.end())
			return true;	// Include
	}

	return false;  // Exclude
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

	return (double) newValue;
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
Template function to obtain counter data for the standard types
Specialize, if string and other special formats are needed
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

/*
This updates the process status
*/
void UpdateProcesses()
{
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
				

				LONGLONG newData;
				if (GetCounterData(pObjInst.get(), L"% Processor Time", newData))
				{
					processId_type id;
					if (GetCounterData(pObjInst.get(), L"ID Process", id))
					{
						// Total_ also has process id == 0
						// Make a special case for this, so we don't have to
						// find the name for all ids.

						if (id == 0)
						{
							// We only need to get and check the name id == 0
							if (pObjInst->GetObjectInstanceName(name, 256))
							{
								if (_wcsicmp(name, L"_Total") == 0)
								{
									// Don't add _Total to the list
									// And to confuse things, it has process ID 0
									continue;
								}
							}
						}

						// Update new or old processvalues in g_Processes
						// using a map speeds up lookup
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
							if (pObjInst->GetObjectInstanceName(name, 256))
							{
								if (_wcsicmp(name, L"_Total") == 0)
								{
									// Don't add _Total to the list
									continue;
								}

								ProcessValues newValues;

								newValues.name = name;
								newValues.oldValue = 0LL;
								newValues.newValue = newData;
								newValues.generation = Generation;

								g_Processes.emplace(id, newValues);

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
