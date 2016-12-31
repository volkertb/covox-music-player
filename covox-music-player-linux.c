#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/io.h>
#include <sys/types.h>
#include <sndfile.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <termios.h>
#include <stdbool.h>


#define ERROR_CODE_WRONG_ARG 1
#define ERROR_CODE_CANNOT_OPEN_FILE 2
#define ERROR_CODE_PARALLEL_ADDRESS 3

#define FILENAME_WAV_CONVERT "/tmp/covox-wav-convert.wav"

//Store and force overwrite converted wav file in tmp directory as libsndfile can only process wav files
#define FORMAT_COMMAND_FFMPEG "ffmpeg -y -i %s /tmp/covox-wav-convert.wav"

#define CODE_SPACEBAR 32
#define CODE_ESCAPE 27

struct termios initial_settings, new_settings;

bool pausePlayback = false;
bool endPlayback = false;


static const char * format_duration_str (double seconds){
	static char str [128] ;
	int hrs, min ;
	double sec ;

	memset (str, 0, sizeof (str)) ;

	hrs = (int) (seconds / 3600.0) ;
	min = (int) ((seconds - (hrs * 3600.0)) / 60.0) ;
	sec = seconds - (hrs * 3600.0) - (min * 60.0) ;

	snprintf (str, sizeof (str) - 1, "%02d:%02d:%04.1f", hrs, min, sec) ;

	return str ;
}


static const char * generate_duration_str (SF_INFO *sfinfo){
	double seconds ;

	if (sfinfo->samplerate < 1)
	return NULL ;

	if (sfinfo->frames / sfinfo->samplerate > 0x7FFFFFFF)
	return "unknown" ;

	seconds = (1.0 * sfinfo->frames) / sfinfo->samplerate ;


	return format_duration_str (seconds) ;
}

//From: http://stackoverflow.com/questions/5309471/getting-file-extension-in-c
const char * get_filename_ext(const char *filename) {
	const char *dot = strrchr(filename, '.');
	if(!dot || dot == filename) return "";
	return dot + 1;
}

uint8_t mapShortTo8bit(short input){

	double slope = 1.0 * (UINT8_MAX - 0) / (SHRT_MAX - SHRT_MIN);
	uint8_t output = 0 + slope * (input -  SHRT_MIN);

	// printf("Convert %d to %" PRIu8 "\n",input, output);

	return output;
}

long long getCurrentNanoseconds(){
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);
	long long specTime = (spec.tv_sec * 1E9) + spec.tv_nsec;
	return specTime;
}

int parallelPortBaseAddress;

short * dataBuffer;
int totalCount;


long long nanosecondsPerFrame;

long long startSpecTime;

long long currentSpecTime;
long long timeSinceStart;
int frameNumber;
int previousFrameNumber;
int channels;

long framesSkippedCumulativeUIThread = 0;
long framesSkippedCumulativePlaybackThread = 0;

void *playbackFunction(void *inputPtr){
	while(1){

		if(endPlayback){
			break;
		}

		if(pausePlayback){
			usleep(10000);
			continue;
		}



		currentSpecTime = getCurrentNanoseconds();
		timeSinceStart = currentSpecTime - startSpecTime;


		frameNumber = timeSinceStart / nanosecondsPerFrame;

		if(frameNumber >= totalCount){
			break;
		}

		//Only accumulate the skipping if the difference is greater than the usual 1 increment
		if((frameNumber - previousFrameNumber) > 1){
				framesSkippedCumulativePlaybackThread += frameNumber - previousFrameNumber - 1;
		}

		previousFrameNumber = frameNumber;

		//Average values to merge all channels to mono
		int sum = 0;

		for (int m = 0 ; m < 1 ; m++) {
			sum += dataBuffer[frameNumber * channels + m];
		}

		short value = sum / channels;


		uint8_t smallValue = mapShortTo8bit(value);
		outb(smallValue, parallelPortBaseAddress);
	}

}



int main(int argc, char *argv[]){


	if(argc < 3){
		printf("Insufficient arguments: Require music file and first parallel port address like this ./linux-covox-player file.mp3 0x378\n");
		return ERROR_CODE_WRONG_ARG;
	}

	char * filename = argv[1];
	char * parallelPortAddressStr = argv[2];
	const char * fileExtension = get_filename_ext(filename);

	//If file does not have wav extension, call FFMPEG to convert it to wav before proceeding
	if(strcmp(fileExtension, "wav") != 0){
		printf("File is not wav, converting to wav using FFMPEG\n");

		char conversionCommand[50];

		//Generate conversion command
		snprintf(conversionCommand, 500, FORMAT_COMMAND_FFMPEG, filename);
		system(conversionCommand);

		filename = FILENAME_WAV_CONVERT;

		printf("Conversion to WAV completed\n");
	}



	printf("Attempting to open parallel port at %s\n", parallelPortAddressStr);


	parallelPortBaseAddress = (int)strtol(parallelPortAddressStr, NULL, 0);

	if(parallelPortBaseAddress == 0L //Unable to convert given address to hex number
		|| ioperm(parallelPortBaseAddress, 8, 1) == -1)	{ //Set permissions to access port

			printf("Invalid parallel port base address.\n");
			return ERROR_CODE_PARALLEL_ADDRESS;
	}


	printf("Attempting to play file %s to port at %s\n", filename, parallelPortAddressStr);


	//Open file using lsndfile API
	SF_INFO sfinfo;
	memset (&sfinfo, 0, sizeof (sfinfo)) ;

	SNDFILE* soundFile = sf_open(filename, SFM_READ, &sfinfo);

	if(soundFile == NULL){

		int errorCode = sf_error(soundFile) ;

		switch(errorCode){
			case SF_ERR_NO_ERROR:
			printf("No error huh?\n");
			break;
			case SF_ERR_UNRECOGNISED_FORMAT:
			printf("Unrecognised file format\n");
			break;
			case SF_ERR_SYSTEM:
			printf("System error, probably a missing file\n");
			break;
			case SF_ERR_MALFORMED_FILE:
			printf("Malformed File\n");
			break;
			case SF_ERR_UNSUPPORTED_ENCODING:
			printf("Unsupported encoding\n");
			break;
			default:
			printf("Unknown error code from libsnd library\n");
		}

		return ERROR_CODE_CANNOT_OPEN_FILE;
	}


	//Source https://gist.github.com/whyrusleeping/3983293
	tcgetattr(0,&initial_settings);

	//Disable delay on getchar
	new_settings = initial_settings;
	new_settings.c_lflag &= ~ICANON;
	new_settings.c_lflag &= ~ECHO;
	new_settings.c_lflag &= ~ISIG;
	new_settings.c_cc[VMIN] = 0;
	new_settings.c_cc[VTIME] = 0;

	tcsetattr(0, TCSANOW, &new_settings);



	printf("\nFile details:\n");

	int sampleRate = sfinfo.samplerate;

	printf ("Sample Rate : %d\n", sampleRate) ;

	long frames = sfinfo.frames;

	if (frames == SF_COUNT_MAX){
		printf ("Frames      : unknown\n") ;
	} else {
		printf ("Frames      : %ld\n", frames) ;
	}

	channels = sfinfo.channels;

	printf ("Channels    : %d\n", channels) ;
	printf ("Format      : 0x%08X\n", sfinfo.format) ;
	printf ("Sections    : %d\n", sfinfo.sections) ;
	printf ("Seekable    : %s\n", (sfinfo.seekable ? "TRUE" : "FALSE")) ;
	printf ("Duration    : %s\n", generate_duration_str (&sfinfo)) ;

	int totalItems = frames * channels;

	dataBuffer = (short *) malloc(totalItems * sizeof(short));
	totalCount = sf_readf_short (soundFile, dataBuffer, frames);
	sf_close(soundFile);

	printf("Total Frames Read from file: %d\n\n", totalCount);

	printf("Press spacebar to pause, Escape to exit\n\n");


	//Calculate how many nanoseconds to play each frame
	nanosecondsPerFrame = 1E9 / sampleRate;

	startSpecTime = getCurrentNanoseconds();
	frameNumber = 0;
	previousFrameNumber = 0;

	//setbuf(stdout, NULL);

	pthread_t playBackThread;
	pthread_create(&playBackThread, NULL, playbackFunction, NULL);


  while(1){
		usleep(100000);

		if(!pausePlayback){

			double secondsPlayed = 1.0 * frameNumber / sampleRate;

			const char * currentPlayTime = format_duration_str(secondsPlayed);

			int framesSkipped = 0;

			if(framesSkippedCumulativePlaybackThread != framesSkippedCumulativeUIThread){

				int diff = framesSkippedCumulativePlaybackThread - framesSkippedCumulativeUIThread;

				framesSkippedCumulativeUIThread = framesSkippedCumulativePlaybackThread;

				framesSkipped = diff;

			}

			printf("\rPosition: %s, framesSkipped: %03d", currentPlayTime, framesSkipped);

			if(framesSkipped > 0){
				printf("\n");
			}
		}

		int readChar = getchar();

		if(readChar == CODE_SPACEBAR){
			pausePlayback = !pausePlayback;
			printf("\nPaused. Press spacebar to resume.");
		} else if(readChar == CODE_ESCAPE){
			endPlayback = true;
			break;
		}

		fflush(stdout);


	}


	pthread_join(playBackThread, NULL);

	free(dataBuffer);

	outb(0, parallelPortBaseAddress);

	//Restore previous terminal settings
	tcsetattr(0, TCSANOW, &initial_settings);

	printf("\n");

	//Take away permissions to access port
	if (ioperm(parallelPortBaseAddress, 8, 0)) {
		printf("Error closing parallel port\n");
		return ERROR_CODE_PARALLEL_ADDRESS;
	}

	return 0;
	}
