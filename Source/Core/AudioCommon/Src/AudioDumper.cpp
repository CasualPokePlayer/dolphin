#include <string>
#include <sstream>

#include "Common.h"
#include "AudioDumper.h"
#include "FileUtil.h"

AudioDumper::AudioDumper(std::string _basename)
{
	currentrate = 0;
	fileopen = false;
	fileindex = 0;
	basename = _basename;
}

AudioDumper::~AudioDumper()
{
	if (fileopen)
	{
		wfr.Stop();
		fileopen = false;
	}
}

bool AudioDumper::CheckEm(int srate)
{
	if (!fileopen || srate != currentrate || wfr.GetAudioSize() > 2 * 1000 * 1000 * 1000)
	{
		if (fileopen)
		{
			wfr.Stop();
			fileopen = false;
		}
		std::stringstream fnames;
		fnames << File::GetUserPath(D_DUMPAUDIO_IDX) << basename << fileindex << ".wav";
		std::string fname = fnames.str();
		if (!File::CreateFullPath(fname) || !wfr.Start(fname.c_str (), srate))
			// huh?
			return false;

		fileopen = true;
		currentrate = srate;
		fileindex++;
	}
	return true;

}

void AudioDumper::DumpSamplesBE(const short* buff, int nsamp, int srate)
{
	if (!CheckEm(srate))
		return;

	wfr.AddStereoSamplesBE(buff, nsamp);
}

void AudioDumper::DumpSamples(const short* buff, int nsamp, int srate)
{
	if (!CheckEm(srate))
		return;

	wfr.AddStereoSamples(buff, nsamp);
}