#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <cstring>
#include <fstream>
#include <csignal>
#include <algorithm>

#include "../bmslab.h"

enum class AllocMode {
	BMSLAB,
	MALLOC,
};

static int g_threadCount = 1;
static int g_runSeconds = 10;
static int g_benchMode = 1; // B=1,2,3
static AllocMode g_allocMode = AllocMode::MALLOC;

static int g_objSize = 128;
static int g_maxPageCount = 256;
static int g_chunkSize = 1000;
static int g_phaseInterval = 5;

static bmslab *g_slab = NULL;
static std::atomic<bool> g_stopFlag {false};

static std::atomic<long long> g_allocCount{0};
static std::atomic<long long> g_freeCount{0};

// (B=3) alloc/free pattern
struct LoadPhase {
	int startSec;
	int endSec;
	int allocRate;
	int chunkSize;
};
static std::vector<LoadPhase> g_loadPhases;

// file stream for stat logs
static std::ofstream g_throughputLog;
static std::ofstream g_memoryLog;
static std::ofstream g_bmslabLog;
static std::ofstream g_finalResult;

static auto g_benchStartTime = std::chrono::steady_clock::now();

// VmRss (KB) from /proc/self/status
long long getCurrentRSSkB() {
	std::ifstream ifs("/proc/self/status");
	
	if (!ifs) {
		return -1;
	}

	std::string line;
	while (std::getline(ifs, line)) {
		if (line.rfind("VmRSS:", 0) == 0) {
			auto pos = line.find_last_of(" \t");

			if (pos == std::string::npos) {
				return -1;
			}

			std::string kbStr = line.substr(pos + 1);
			return std::atoll(kbStr.c_str());
		}
	}

	return -1;
}

// Stat gathering thread
void metricsThreadFunc() {
	using namespace std::chrono;
	auto prevTime = steady_clock::now();
	long long prevAllocCount = 0;
	long long prevFreeCount = 0;

	while (!g_stopFlag.load()) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		auto curTime = steady_clock::now();
		double elapsed = duration_cast<seconds>(curTime - prevTime).count();
		double sinceStartSec
			= duration_cast<seconds>(curTime - g_benchStartTime).count();
		prevTime = curTime;

		// alloc/free
		long long curAlloc = g_allocCount.load();
		long long curFree = g_freeCount.load();
		long long deltaAlloc = (curAlloc - prevAllocCount);
		long long deltaFree = (curFree - prevFreeCount);
		prevAllocCount = curAlloc;
		prevFreeCount = curFree;

		double allocTPS
			= (elapsed > 0.0) ? ((double)deltaAlloc / elapsed) : 0.0;
		double freeTPS
			= (elapsed > 0.0) ? ((double)deltaFree / elapsed) : 0.0;

		// RSS
		long long rssKB = getCurrentRSSkB();

		// bmslab
		int page_count = 0;
		int slot_count = 0;
		if (g_allocMode == AllocMode::BMSLAB && g_slab) {
			page_count = get_bmslab_phys_page_count(g_slab);
			slot_count = get_bmslab_allocated_slots(g_slab);
		}

		// 1) throughput.csv -> "timeSec, allocTPS, freeTPS"
		g_throughputLog << sinceStartSec << "," << allocTPS << "."
			<< freeTPS << "\n";

		// 2) memory.csv -> "timeSec, rssKB"
		g_memoryLog << sinceStartSec << "," << rssKB << "\n";

		// 3) bmslab.csv
		if (g_allocMode == AllocMode::BMSLAB) {
			g_bmslabLog << sinceStartSec << "," << page_count << ","
				<< slot_count << "\n";
		}

		// flush
		g_throughputLog.flush();
		g_memoryLog.flush();
		if (g_allocMode == AllocMode::BMSLAB) {
			g_bmslabLog.flush();
		}
	}
}

// B=1
void workerB1(int id) {
	auto endTime
		= std::chrono::steady_clock::now() + std::chrono::seconds(g_runSeconds);

	while (std::chrono::steady_clock::now() < endTime) {
		// alloc
		void *ptr = NULL;
		if (g_allocMode == AllocMode::BMSLAB) {
			ptr = bmslab_alloc(g_slab);
		} else {
			ptr = malloc(g_objSize);
		}

		if (ptr) {
			g_allocCount.fetch_add(1);

			// free
			if (g_allocMode == AllocMode::BMSLAB) {
				bmslab_free(g_slab, ptr);
			} else {
				free(ptr);
			}
			g_freeCount.fetch_add(1);
		}
	}
}

// B=2
void workerB2(int id) {
	auto endTime
		= std::chrono::steady_clock::now() + std::chrono::seconds(g_runSeconds);
	std::vector<void *> localPtrs;
	localPtrs.reserve(g_chunkSize);

	while (std::chrono::steady_clock::now() < endTime) {
		// alloc
		localPtrs.clear();
		for (int i = 0; i < g_chunkSize; i++) {
			void *ptr = NULL;
			if (g_allocMode == AllocMode::BMSLAB) {
				ptr = bmslab_alloc(g_slab);
			} else {
				ptr = malloc(g_objSize);
			}

			if (ptr) {
				localPtrs.push_back(ptr);
				g_allocCount.fetch_add(1);
			}
		}

		// free
		for (auto &ptr : localPtrs) {
			if (g_allocMode == AllocMode::BMSLAB) {
				bmslab_free(g_slab, ptr);
			} else {
				free(ptr);
			}
			g_freeCount.fetch_add(1);
		}
	}
}

// B=3
void workerB3(int id) {
	auto startTime = std::chrono::steady_clock::now();
	auto endTime = startTime + std::chrono::seconds(g_runSeconds);

	int cycleLen = 0;
	if (!g_loadPhases.empty()) {
		cycleLen = g_loadPhases.back().endSec;
	}

	while (true) {
		auto nowT = std::chrono::steady_clock::now();
		if (nowT >= endTime) {
			break;
		}

		int elapsedSec
			= (int)std::chrono::duration_cast<std::chrono::seconds>
					(nowT - startTime).count();

		int modSec = elapsedSec;
		if (cycleLen > 0) {
			modSec = elapsedSec % cycleLen;
		}

		int allocRate = 1000;
		int chunkSz = g_chunkSize;
		for (auto &ph : g_loadPhases) {
			if (modSec >= ph.startSec && modSec < ph.endSec) {
				allocRate = ph.allocRate;
				chunkSz = ph.chunkSize;
				break;
			}
		}

		int repeats = (chunkSz > 0) ? (allocRate / chunkSz) : 1;
		if (repeats < 1) {
			repeats = 1;
		}

		auto loopEnd = nowT + std::chrono::seconds(1);
		std::vector<void *> localPtrs;
		localPtrs.reserve(chunkSz);
		for (int r = 0; r < repeats; r++) {
			// alloc
			localPtrs.clear();
			for (int i = 0; i < chunkSz; i++) {
				void *ptr = NULL;
				if (g_allocMode == AllocMode::BMSLAB) {
					ptr = bmslab_alloc(g_slab);
				} else {
					ptr = malloc(g_objSize);
				}

				if (ptr) {
					g_allocCount.fetch_add(1);
					localPtrs.push_back(ptr);
				}
			}

			// free
			for (auto &ptr : localPtrs) {
				if (g_allocMode == AllocMode::BMSLAB) {
					bmslab_free(g_slab, ptr);
				} else {
					free(ptr);
				}
				g_freeCount.fetch_add(1);
			}

			if (std::chrono::steady_clock::now() >= loopEnd) {
				break;
			}
		}
	}
}

int main(int argc, char *argv[]) {
	// Arguments:
	// 1) threadCount
	// 2) runSeconds
	// 3) benchMode=1|2|3
	// 4) allocMode=malloc|bmslab
	// 5) objSize
	// 6) maxPageCount
	// 7) chunkSize
	// 8) phaseInterval
	if (argc < 9) {
		std::cerr << "Usage: " << argv[0]
			<< " <threadCount> <runSeconds> <benchMode=1|2|3>"
			<< " <allocMode=malloc|bmslab> <objSize> <maxPageCount>"
			<< " <chunkSize> <phaseInterval>\n";
		return 1;
	}

	g_threadCount = std::stoi(argv[1]);
	g_runSeconds = std::stoi(argv[2]);
	g_benchMode = std::stoi(argv[3]);
	std::string modeStr = argv[4];

	g_objSize = std::stoi(argv[5]);
	g_maxPageCount = std::stoi(argv[6]);
	g_chunkSize = std::stoi(argv[7]);
	g_phaseInterval = std::stoi(argv[8]);

	if (modeStr == "bmslab") {
		g_allocMode = AllocMode::BMSLAB;
	} else {
		g_allocMode = AllocMode::MALLOC;
	}

	g_throughputLog.open("throughput.csv");
	g_memoryLog.open("memory.csv");
	g_bmslabLog.open("bmslab.csv");
	g_finalResult.open("final_result.csv");

	g_throughputLog << "TimeSec,AllocTPS,FreeTPS\n";
	g_memoryLog << "TimeSec,RSS_kB\n";
	if (g_allocMode == AllocMode::BMSLAB) {
		g_bmslabLog << "TimeSec,PhysPageCount,AllocatedSlots\n";
	}

	if (g_allocMode == AllocMode::BMSLAB) {
		g_slab = bmslab_init(g_objSize, g_maxPageCount);
		if (!g_slab) {
			std::cerr << "Failed to init bmslab\n";
			return 1;
		}
		std::cerr << "bmslab_init OK. objSize=" << g_objSize
			<< ", maxPageCount=" << g_maxPageCount << std::endl;
	}

	if (g_benchMode == 3) {
		g_loadPhases.clear();

		g_loadPhases.push_back({0, g_phaseInterval,
								2000, g_chunkSize});
		g_loadPhases.push_back({g_phaseInterval, 2*g_phaseInterval, 
								20000, g_chunkSize});
		g_loadPhases.push_back({2*g_phaseInterval, 3*g_phaseInterval, 
								3000, g_chunkSize});
		g_loadPhases.push_back({3*g_phaseInterval, 4*g_phaseInterval, 
								15000, g_chunkSize});
	}

	std::thread metricThread(metricsThreadFunc);

	std::vector<std::thread> workers;
	workers.reserve(g_threadCount);
	for (int i = 0; i < g_threadCount; i++) {
		if (g_benchMode == 1) {
			workers.emplace_back(workerB1, i);
		} else if (g_benchMode == 2) {
			workers.emplace_back(workerB2, i);
		} else {
			workers.emplace_back(workerB3, i);
		}
	}

	std::this_thread::sleep_for(std::chrono::seconds(g_runSeconds));
	g_stopFlag.store(true);

	for (auto &th : workers) {
		th.join();
	}
	metricThread.join();

	// Final result
	long long totalAllocs = g_allocCount.load();
	long long totalFrees = g_freeCount.load();

	double avgAllocTPS = (double)totalAllocs / g_runSeconds;
	double avgFreeTPS = (double)totalFrees / g_runSeconds;

	g_finalResult << "Threads: " << g_threadCount << "\n";
	g_finalResult << "Duration: " << g_runSeconds << "\n";
	g_finalResult << "BenchMode: " << g_benchMode << "\n";
	g_finalResult << "AllocMode: " << modeStr << "\n";
	g_finalResult << "ObjSize: " << g_objSize << "\n";
	g_finalResult << "MaxPageCount: " << g_maxPageCount << "\n";
	g_finalResult << "ChunkSize: " << g_chunkSize << "\n";
	g_finalResult << "PhaseInterval: " << g_phaseInterval << "\n";
	g_finalResult << "TotalAllocs: " << totalAllocs << "\n";
	g_finalResult << "TotalFrees: " << totalFrees << "\n";
	g_finalResult << "AvgAllocTPS: " << avgAllocTPS << "\n";
	g_finalResult << "AvgFreeTPS: " << avgFreeTPS << "\n";

	// Close files
	g_throughputLog.close();
	g_memoryLog.close();
	g_bmslabLog.close();
	g_finalResult.close();

	if (g_slab) {
		bmslab_destroy(g_slab);
		g_slab = NULL;
	}

	return 0;
}
