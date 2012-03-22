#ifndef _AUDIODUMPER_H
#define _AUDIODUMPER_H

#include "WaveFile.h"
#include <string>

struct AudioDumper
{
private:
	WaveFileWriter wfr;
	int currentrate;
	bool fileopen;
	int fileindex;
	std::string basename;
	bool CheckEm(int srate);
public:
	AudioDumper(std::string _basename);
	~AudioDumper();

	void DumpSamples(const short* buff, int nsamp, int srate);
	void DumpSamplesBE(const short* buff, int nsamp, int srate);
};

#endif
