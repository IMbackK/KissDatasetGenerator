#include <set>
#include <filesystem>
#include <eisgenerator/model.h>
#include <eisgenerator/log.h>
#include <eisgenerator/translators.h>
#include <thread>
#include <list>
#include <mutex>
#include <cassert>
#include <memory>
#include <algorithm>

#include "log.h"
#include "options.h"
#include "filterdata.h"
#include "tokenize.h"
#include "randomgen.h"
#include "eisgendata.h"

static bool checkDir(const std::filesystem::path& outDir)
{
	if(!std::filesystem::is_directory(outDir))
	{
		if(!std::filesystem::create_directory(outDir))
		{
			std::cerr<<outDir<<" dose not exist and can not be created\n";
			return false;
		}
	}
	return true;
}

struct Spectrum
{
	Example ex;
	std::string model;
	size_t indexInModel;
};

static bool save(const Spectrum& spectrum, const std::filesystem::path& outDir)
{
	std::string filename("generated");
	filename.push_back('_');
	filename.append(spectrum.model);
	filename.push_back('_');
	filename.append(std::to_string(spectrum.indexInModel));
	filename.append(".csv");
	eis::EisSpectra saveSpectra(spectrum.ex.data, spectrum.model,
								std::to_string(spectrum.ex.label) + ", " + std::to_string(spectrum.indexInModel));
	bool ret = eis::saveToDisk(saveSpectra, outDir/filename);
	if(!ret)
		Log(Log::ERROR)<<"Could not save "<<outDir/filename<<" to disk\n";
	return ret;
}

void threadFunc(EisGeneratorDataset dataset, size_t begin, size_t end, int testPercent,
				std::vector<size_t>* classCounts, std::vector<size_t>* testCounts, std::mutex* countsMutex,
				const std::filesystem::path outDir)
{
	countsMutex->lock();
	Log(Log::INFO)<<"Thread doing "<<begin<<" to "<<end-1;
	countsMutex->unlock();
	for(size_t i = begin; i < end; ++i)
	{
		Spectrum spectrum;
		spectrum.ex = dataset.get(i);
		spectrum.model = dataset.modelStringForClass(spectrum.ex.label);
		spectrum.indexInModel = classCounts->at(spectrum.ex.label);

		bool test = false;

		{
			std::scoped_lock lock(*countsMutex);
			++classCounts->at(spectrum.ex.label);
			if(testPercent > 0 &&
				(rd::rand(100) < testPercent ||
				testCounts->at(spectrum.ex.label)/static_cast<double>(classCounts->at(spectrum.ex.label))*100.0 < testPercent/2 ||
				testCounts->at(spectrum.ex.label) == 0))
			{
				++testCounts->at(spectrum.ex.label);
				test = true;
			}
		}

		if(test)
			save(spectrum, outDir/"test");
		else
			save(spectrum, outDir/"train");
	}
}

int main(int argc, char** argv)
{
	Log::level = Log::INFO;
	eis::Log::level = eis::Log::ERROR;
	Config config;
	argp_parse(&argp, argc, argv, 0, 0, &config);

	if(config.datasetPath.empty())
	{
		Log(Log::ERROR)<<"A path to a dataset (option -d) must be provided";
		return 1;
	}

	bool ret = checkDir(config.outDir);
	if(!ret)
		return 3;
	ret = checkDir(config.outDir/"train");
	if(!ret)
		return 3;
	if(config.testPercent > 0)
	{
		ret = checkDir(config.outDir/"test");
		if(!ret)
			return 3;
	}

	EisGeneratorDataset dataset(config.datasetPath, config.desiredSize, 100, 0, true, false);
	Log(Log::INFO)<<"Dataset size: "<<dataset.size();

	std::mutex countsMutex;
	std::vector<size_t> classCounts(dataset.classesCount(), 0);
	std::vector<size_t> testCounts(dataset.classesCount(), 0);

	std::vector<std::thread> threads;
	size_t threadCount = std::min(std::thread::hardware_concurrency(), 8U);
	size_t countPerThread = dataset.size()/threadCount;
	size_t i = 0;
	for(; i < threadCount-1; ++i)
		threads.push_back(std::thread(threadFunc, dataset, i*countPerThread, (i+1)*countPerThread,
									  config.testPercent, &classCounts, &testCounts, &countsMutex, config.outDir));
	threads.push_back(std::thread(threadFunc, dataset, i*countPerThread, dataset.size(),
									  config.testPercent, &classCounts, &testCounts, &countsMutex, config.outDir));

	for(std::thread& thread : threads)
		thread.join();

	return 0;
}
