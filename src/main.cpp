#include <set>
#include <filesystem>
#include <eisgenerator/model.h>
#include <eisgenerator/log.h>
#include <thread>
#include <list>
#include <mutex>
#include <cassert>
#include <memory>

#include "log.h"
#include "options.h"
#include "filterdata.h"
#include "tokenize.h"

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

struct FileInfo
{
	std::string model;
	std::filesystem::path path;
	size_t indexInModel;
	int classId;
};

struct Spectrum
{
	FileInfo info;
	std::vector<eis::DataPoint> data;
	double meanNorm;
};

template <typename type>
struct ThreadList
{
	std::list<type> list;
	std::mutex mutex;
};

static std::string dropSeriesResistance(const std::string& model)
{
	std::vector<std::string> tokens = tokenize(model, '-', '(', ')');
	std::string out;
	for(const std::string& token : tokens)
	{
		if(token == "r" || token == "R")
			continue;
		out.append(token);
		out.push_back('-');
	}
	out.pop_back();
	return out;
}

static std::vector<FileInfo> getListOfFiles(const std::filesystem::path dir, std::string extension)
{
	std::vector<FileInfo> out;
	if(!std::filesystem::is_directory(dir))
	{
		Log(Log::ERROR)<<dir<<" must be a valid directory";
		return out;
	}

	for(const std::filesystem::directory_entry& dirent : std::filesystem::directory_iterator{dir})
	{
		if(dirent.path().extension() != ".csv")
			continue;
		if(dirent.is_regular_file())
		{
			std::vector<std::string> tokens = tokenize(dirent.path().stem(), '_');
			if(tokens.size() != 3)
			{
				Log(Log::WARN)<<"file "<<dirent.path()<<" dosent correspond to the required prefix_circut_modelIndex.extension sheme, skipping";
				continue;
			}
			FileInfo info;
			info.model = dropSeriesResistance(tokens[1]);
			info.path = dirent.path();
			Log(Log::DEBUG)<<"Found "<<info.path;
			out.push_back(info);
		}
	}
	return out;
}

static std::vector<std::vector<eis::DataPoint>>::iterator
checkForDuplicateModel(std::vector<std::vector<eis::DataPoint>>& modelSpectra, const std::vector<eis::DataPoint> spectrum)
{
	for(size_t i = 0; i < modelSpectra.size(); ++i)
	{
		const std::vector<eis::DataPoint>& testSpectrum = modelSpectra[i];
		fvalue distance = eis::eisDistance(testSpectrum, spectrum);
		if(distance < 0.001)
			return modelSpectra.begin()+i;
	}
	return modelSpectra.end();
}

static std::vector<std::pair<std::string, size_t>> discoverModels(std::vector<FileInfo>& files)
{
	std::vector<std::pair<std::string, size_t>> models;
	std::vector<std::vector<eis::DataPoint>> modelSpectra;

	for(FileInfo& file : files)
	{
		auto search =
			std::find_if(models.begin(), models.end(),
				[&file](const std::pair<std::string, size_t>& a) {return a.first == file.model;});
		if(search == models.end())
		{
			eis::Model model(file.model, 100, false);
			eis::Range omegaRange(0.1, 1e6, 100, true);
			std::vector<eis::DataPoint> spectrum = model.executeSweep(omegaRange);
			std::vector<std::vector<eis::DataPoint>>::iterator iter;
			if((iter = checkForDuplicateModel(modelSpectra, spectrum)) != modelSpectra.end())
			{
				Log(Log::ERROR)<<file.model<<" and "<<models[iter-modelSpectra.begin()].first
					<<" are different strings but are mathematically equivalent reprisentations of the same model"
					<<" currenlty this case is not handled aborting";
				exit(1);
			}
			else
			{
				models.push_back({file.model, 1});
				modelSpectra.push_back(spectrum);
				file.indexInModel = 0;
				file.classId = models.size()-1;
			}
		}
		else
		{
			file.classId = search-models.begin();
			file.indexInModel = ++search->second;
		}
	}
	return models;
}

static bool save(const FileInfo& info, const std::vector<eis::DataPoint>& data, const std::filesystem::path& outDir)
{
	bool ret =saveToDisk(data, outDir/info.path.filename(), info.model + ", " + std::to_string(info.classId) + ", " + std::to_string(info.indexInModel));
	if(!ret)
		Log(Log::ERROR)<<"Could not save "<<outDir/info.path.filename()<<" to disk\n";
	return ret;
}

static std::vector<eis::DataPoint> load(const FileInfo& info)
{
	std::vector<eis::DataPoint> data = eis::loadFromDisk(info.path).first;
	filterData(data, 100);
	return data;
}

static double getMeanNorm(const std::vector<eis::DataPoint>& data)
{
	double mean = 0;
	for(const eis::DataPoint point : data)
		mean += std::sqrt(std::pow(point.im.real(), 2) + std::pow(point.im.imag(), 2));
	mean = mean/data.size();
	return mean;
}

static void printModels(const std::vector<std::pair<std::string, size_t>>& models)
{
	Log(Log::INFO)<<"Models:";
	for(const std::pair<std::string, size_t>& model : models)
		Log(Log::INFO)<<model.first<<": "<<model.second;
}

static void dropFilesWithDropModels(const std::vector<std::string>& dropModels, std::vector<FileInfo>& files)
{
	for(size_t i = 0; i < files.size(); ++i)
	{
		FileInfo& file = files[i];
		auto search =
			std::find_if(dropModels.begin(), dropModels.end(),
				[&file](const std::string& a) {return a == file.model;});
		if(search != dropModels.end())
		{
			files.erase(files.begin()+i);
			--i;
		}
	}
}

static std::vector<std::string> detemineDropModels(const std::vector<std::pair<std::string, size_t>>& models)
{
	std::vector<std::string> dropModels;
	size_t totalCount = 0;
	for(const std::pair<std::string, size_t>& model : models)
		totalCount += model.second;

	size_t expectedCount = totalCount/models.size();
	for(const std::pair<std::string, size_t>& model : models)
	{
		double frac = static_cast<double>(model.second)/expectedCount;
		if(frac < 0.1)
		{
			Log(Log::INFO)<<"Will drop "<<model.first<<" as this model has only "
				<<static_cast<double>(model.second)/totalCount*100<<"\% of examles";
			dropModels.push_back(model.first);
		}
	}
	return dropModels;
}

static std::vector<std::list<std::unique_ptr<Spectrum>>> splitByModel(const std::list<std::unique_ptr<Spectrum>>& list)
{
	std::vector<size_t> classIds;
	std::vector<std::list<std::unique_ptr<Spectrum>>> datasets;

	for(const std::unique_ptr<Spectrum>& spectrum : list)
	{
		auto search = std::find(classIds.begin(), classIds.end(), spectrum->info.classId);
		if(search != classIds.end())
		{
			classIds.push_back(spectrum->info.classId);
			datasets.push_back(std::list<std::unique_ptr<Spectrum>>());
			//datasets.back().push_back(std::move(spectrum));
		}
		else
		{
			datasets[search-classIds.begin()].push_back(std::move(spectrum));
		}
	}
	return datasets;
}

void ammendWithGeneratedSpectra(std::list<std::unique_ptr<Spectrum>>* list, size_t targetSize)
{
	Log(Log::INFO, false)<<list->front()->info.model;
	if(list->size() > targetSize)
	{
		Log(Log::INFO)<<" already has sufficant examples";
		return;
	}

	size_t needed = targetSize-list->size();
	Log(Log::INFO)<<" will be ammended with "<<needed<<" generated examples";

	eis::Model model(list->front()->info.model, 10, true);
	eis::Range omegaRange(1, 1e6, true);
	const double step = 0.35;
	std::vector<size_t> indicies = model.getRecommendedParamIndices(omegaRange, step, true);
	if(indicies.empty())
	{
		Log(Log::WARN)<<list->front()->info.model<<": can not generate examples for this model";
		return;
	}
	for(size_t i = 2; i < 32 && indicies.size() < needed; i*=2)
	{
		Log(Log::INFO)<<list->front()->info.model<<": found indicies for only "<<indicies.size()<<" spectra trying again with a step of "<<step/i;
		indicies = model.getRecommendedParamIndices(omegaRange, step/i, true);
	}

	if(indicies.size() < needed)
		Log(Log::WARN)<<list->front()->info.model<<": can not generate sufficant examples for this model, will only add "<<indicies.size();

	size_t stride = indicies.size()/needed;
	if(stride == 0)
		stride = 1;

	for(size_t i = 0; i < indicies.size(); i+=stride)
	{
		std::unique_ptr<Spectrum> spectrum(new Spectrum);
		spectrum->data = model.executeSweep(omegaRange, i);
		spectrum->meanNorm = getMeanNorm(spectrum->data);
		spectrum->info = list->back()->info;
		++spectrum->info.indexInModel;
		list->push_back(std::move(spectrum));
	}
}


static void loadThreadFunc(std::vector<FileInfo>::iterator begin, std::vector<FileInfo>::iterator end, ThreadList<std::unique_ptr<Spectrum>>* list)
{
	do
	{
		std::vector<eis::DataPoint> data = load(*begin);
		if(data.empty())
		{
			Log(Log::ERROR)<<"Could not load "<<begin->path;
			continue;
		}
		std::unique_ptr<Spectrum> spectrum(new Spectrum);
		spectrum->info = *begin;
		spectrum->data = data;
		spectrum->meanNorm = getMeanNorm(data);
		list->mutex.lock();
		list->list.push_back(std::move(spectrum));
		list->mutex.unlock();
	} while(++begin != end);
}

void ammendThreadFunc(std::list<std::unique_ptr<Spectrum>>* dataset, size_t totalCount, size_t totalModels)
{
	size_t expectedCount = totalCount/totalModels;
	ammendWithGeneratedSpectra(dataset, expectedCount);
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

	std::vector<FileInfo> files = getListOfFiles(config.datasetPath, ".csv");
	if(files.empty())
	{
		Log(Log::ERROR)<<"no files found";
		return 2;
	}

	std::vector<std::pair<std::string, size_t>> models = discoverModels(files);
	printModels(models);
	Log(Log::INFO)<<"Total Count: "<<files.size();

	std::vector<std::string> dropModels = detemineDropModels(models);
	dropFilesWithDropModels(dropModels, files);
	Log(Log::INFO)<<"Count after dropping: "<<files.size();

	bool ret = checkDir(config.outDir);
	if(!ret)
		return 3;

	ThreadList<std::unique_ptr<Spectrum>> list;
	std::vector<std::thread> threads;
	size_t filesPerThread = files.size()/std::thread::hardware_concurrency();
	for(size_t i = 0; i < std::thread::hardware_concurrency()-1; ++i)
		threads.push_back(std::thread(loadThreadFunc, files.begin()+i*filesPerThread, files.begin()+(i+1)*filesPerThread, &list));
	threads.push_back(std::thread(loadThreadFunc, files.begin()+(std::thread::hardware_concurrency()-1)*filesPerThread, files.end(), &list));
	for(std::thread& thread : threads)
		thread.join();
	threads.clear();
	Log(Log::INFO)<<"Loaded Count: "<<list.list.size();

	std::vector<std::list<std::unique_ptr<Spectrum>>> datasets = splitByModel(list.list);
	for(size_t i = 0; i < datasets.size(); ++i)
	{
		threads.push_back(std::thread(ammendThreadFunc, &datasets[i], list.list.size(), datasets.size()));
	}

	return 0;
}
