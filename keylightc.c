/*
*  Copyright (c) 2024 Michael Marley
*/

/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/poll.h>
#include <unistd.h>

#define NSEC_PER_SEC 1000000000
#define NSEC_PER_USEC 1000

#define INPUT_DEVICE_DIRECTORY_PATH "/dev/input"
#define EVENT_DEVICE_FILENAME_PREFIX "event"

#define BRIGHTNESS_FILE_PATH "/sys/class/leds/chromeos::kbd_backlight/brightness"
#define DEFAULT_ON_SEC 10
#define DEFAULT_BRIGHTNESS 30
#define DEFAULT_FADE_DURATION_USEC 100000

// This must be set exactly to the number of elements in the array or else a segfault will occur
#define SEARCH_DEVICE_COUNT 2
static char const *const restrict search_devices[SEARCH_DEVICE_COUNT]={
	"AT Translated Set 2 keyboard",
	"PIXA3854:00 093A:0274 Touchpad",
};

static struct option const long_options[]={
	{"brightness",required_argument,0,'b'},
	{"fadeduration",required_argument,0,'f'},
	{"timeout",required_argument,0,'t'},
	{"help",no_argument,0,'h'},
	{},
};

// SD_LISTEN_FDS_START, but we don't want to link to systemd
static int brightness_fd=3;

static pthread_cond_t fader_cond;
static pthread_mutex_t fader_mutex;
static int desired_brightness=0;

static bool exit_requested=false;
static bool is_daemon;

struct fader_config{
	FILE *const brightness_file;
	int const fade_duration_nsec;
};

static void log_write(int const priority,char const *const restrict format,...){
	va_list args;
	va_start(args,format);
	if(is_daemon){
		vsyslog(priority,format,args);
	}else{
		FILE *target=stdout;
		if(priority<LOG_WARNING){
			target=stderr;
		}
		vfprintf(target,format,args);
		fprintf(target,"\n");
	}
	va_end(args);
}

static void exit_handler(int const signal_number){
	exit_requested=true;
}

static int const is_event_device(struct dirent const *const restrict directory_entry){
	return strncmp(EVENT_DEVICE_FILENAME_PREFIX,directory_entry->d_name,5)==0;
}

static bool const string_in_array(char const *const restrict string,char const *const restrict array[],int const array_length){
	for(int array_index=0;array_index<array_length;array_index++){
		if(!strcmp(string,array[array_index])){
			return true;
		}
	}
	return false;
}

static int const get_input_fds(struct pollfd *const restrict pollfds){
	struct dirent **device_name_list;
	int const device_count=scandir(INPUT_DEVICE_DIRECTORY_PATH,&device_name_list,is_event_device,alphasort);
	int const clock_type=CLOCK_MONOTONIC;
	
	int found_device_count=0;
	for(int device_index=0;device_index<device_count&&found_device_count<=SEARCH_DEVICE_COUNT;device_index++){
		char device_filename[4096];
		char device_name[256];
		
		snprintf(device_filename,sizeof(device_filename),"%s/%s",INPUT_DEVICE_DIRECTORY_PATH,device_name_list[device_index]->d_name);
		int const fd=open(device_filename,O_RDONLY);
		if(fd<0){
			continue;
		}
		ioctl(fd,EVIOCGNAME(sizeof(device_name)),device_name);
		
		if(string_in_array(device_name,search_devices,SEARCH_DEVICE_COUNT)){
			ioctl(fd,EVIOCSCLOCKID,&clock_type);
			log_write(LOG_INFO,"Using device %s:\t%s",device_filename,device_name);
			pollfds[found_device_count].fd=fd;
			pollfds[found_device_count].events=POLLIN;
			found_device_count++;
		}else{
			close(fd);
		}
		
		free(device_name_list[device_index]);
	}
	
	return found_device_count;
}

static void set_brightness(FILE *const restrict brightness_file,int const brightness){
	fprintf(brightness_file,"%d",brightness);
}

static void start_fade(int const brightness){
	pthread_mutex_lock(&fader_mutex);
	desired_brightness=brightness;
	pthread_cond_signal(&fader_cond);
	pthread_mutex_unlock(&fader_mutex);
}

static int const string_to_int(int *const restrict result,int const min,int const max,char const *const restrict string){
	errno=0;
	long const long_value=strtol(string,NULL,10);
	if(errno!=0){
		return errno;
	}else if(long_value>=min&&long_value<=max){
		*result=(int)long_value;
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}

static void timespec_add_nsec(struct timespec *const restrict timespec,long const nsec){
	timespec->tv_sec+=nsec/NSEC_PER_SEC;
	timespec->tv_nsec+=nsec%NSEC_PER_SEC;
	if(timespec->tv_nsec>=NSEC_PER_SEC){
		timespec->tv_sec++;
		timespec->tv_nsec-=NSEC_PER_SEC;
	}
}

static bool const timespec_gt(struct timespec const ts1,struct timespec const ts2){
	return (ts1.tv_sec>ts2.tv_sec)||(ts1.tv_sec==ts2.tv_sec&&ts1.tv_nsec>ts2.tv_nsec);
}

static int const usage(){
	log_write(LOG_NOTICE,"Usage: keylightc [--brightness <brightness>] [--fadeduration <fadeduration>] [--timeout <timeout>]");
	log_write(LOG_NOTICE,"keylightc - automatic keyboard backlight daemon for Framework laptops");
	log_write(LOG_NOTICE,"Options:");
	log_write(LOG_NOTICE,"  --brightness\t\tbrightness level when active (1-100) [default=%d]",DEFAULT_BRIGHTNESS);
	log_write(LOG_NOTICE,"  --fadeduration\tfade time in microseconds (1-1000000) [default=%d]",DEFAULT_FADE_DURATION_USEC);
	log_write(LOG_NOTICE,"  --timeout\t\tactivity timeout in seconds (1-%d) [default=%d]",INT_MAX,DEFAULT_ON_SEC);
	log_write(LOG_NOTICE,"  --help\t\tdisplay usage information");
	return EXIT_FAILURE;
}

void *fader(void *const restrict arg){
	pthread_mutex_lock(&fader_mutex);
	
	struct fader_config fader_config=*(struct fader_config*)arg;
	int current_brightness=0;
	int previous_desired_brightness=0;
	int fade_interval_nsec=0;
	struct timespec next_fade_step_time={};
	
	int ret=0;
	while(true){
		if(current_brightness!=desired_brightness){
			// If the current brightness is not the desired brightness…
			if(ret==ETIMEDOUT){
				// …and the next fade step is due, fade in/out
				if(current_brightness>desired_brightness){
					current_brightness--;
				}else{
					current_brightness++;
				}
				set_brightness(fader_config.brightness_file,current_brightness);
				
				// Calculate next_fade_step_time based on current next_fade_step_time and fade_interval_nsec
				timespec_add_nsec(&next_fade_step_time,fade_interval_nsec);
			}else if(previous_desired_brightness!=desired_brightness){
				// If the fade step is not due and desired_brightness has changed, initialize the fade
				clock_gettime(CLOCK_MONOTONIC,&next_fade_step_time);
				fade_interval_nsec=fader_config.fade_duration_nsec/abs(current_brightness-desired_brightness);
				previous_desired_brightness=desired_brightness;
			}
			
			// Wait until next_fade_step_time
			ret=pthread_cond_timedwait(&fader_cond,&fader_mutex,&next_fade_step_time);
		}else{
			// Otherwise, wait to be signaled
			ret=pthread_cond_wait(&fader_cond,&fader_mutex);
		}
	}
}

int main(int const argc,char *const *const argv){
	is_daemon=!isatty(STDOUT_FILENO);
	
	int configured_on_sec=DEFAULT_ON_SEC;
	int configured_brightness=DEFAULT_BRIGHTNESS;
	int configured_fade_duration_nsec=DEFAULT_FADE_DURATION_USEC*NSEC_PER_USEC;
	
	int option;
	while((option=getopt_long(argc,argv,"",long_options,NULL))!=EOF){
		switch(option){
			case -1:
			case 0:
				break;
			case 'b':
				if(string_to_int(&configured_brightness,1,100,optarg)){
					return usage();
				}
				break;
			case 'f':
				if(string_to_int(&configured_fade_duration_nsec,1,1000000,optarg)){
					return usage();
				}
				configured_fade_duration_nsec*=NSEC_PER_USEC;
				break;
			case 't':
				if(string_to_int(&configured_on_sec,1,INT_MAX,optarg)){
					return usage();
				}
				break;
			case 'h':
			default:
				return usage();
		}
	}
	
	FILE *brightness_file;
	if(!fcntl(brightness_fd,F_GETFD)){
		// If systemd opened the brightness file for us, use that
		brightness_file=fdopen(brightness_fd,"w");
	}else{
		// Otherwise try to open it ourselves
		brightness_file=fopen(BRIGHTNESS_FILE_PATH,"w");
	}
	if(brightness_file==NULL){
		log_write(LOG_ERR,"Failed to open backlight device!  Check permissions and ensure you are running Linux kernel 6.11 or later.");
		return EXIT_FAILURE;
	}
	setvbuf(brightness_file,NULL,_IONBF,0);
	
	struct pollfd found_device_pollfds[SEARCH_DEVICE_COUNT];
	int const found_device_count=get_input_fds(found_device_pollfds);
	if(found_device_count<1){
		log_write(LOG_ERR,"No matching input devices found!  Check permissions.");
		exit(EXIT_FAILURE);
	}
	
	log_write(LOG_INFO,"Backlight timeout: %d second%s",configured_on_sec,configured_on_sec!=1?"s":"");
	log_write(LOG_INFO,"Brightness level: %d%%",configured_brightness);
	log_write(LOG_INFO,"Fade duration: %d microsecond%s",configured_fade_duration_nsec/NSEC_PER_USEC,configured_fade_duration_nsec/NSEC_PER_USEC!=1?"s":"");
	
	pthread_condattr_t condattr;
	pthread_condattr_init(&condattr);
	pthread_condattr_setclock(&condattr,CLOCK_MONOTONIC);
	
	pthread_cond_init(&fader_cond,&condattr);
	pthread_mutex_init(&fader_mutex,NULL);
	pthread_t fader_thread;
	pthread_create(&fader_thread,NULL,fader,&(struct fader_config){.brightness_file=brightness_file,.fade_duration_nsec=configured_fade_duration_nsec});
	
	// Set up the exit handler
	struct sigaction exit_action;
	exit_action.sa_handler=exit_handler;
	sigaction(SIGINT,&exit_action,NULL);
	sigaction(SIGTERM,&exit_action,NULL);
	
	set_brightness(brightness_file,0);
	
	struct timespec off_time={};
	while(true){
		if(exit_requested){
			break;
		}
		
		if(poll(found_device_pollfds,found_device_count,desired_brightness==configured_brightness?0:-1)==-1&&errno!=EINTR){
			log_write(LOG_ERR,"Poll failure‽");
			exit(EXIT_FAILURE);
		}
		
		for(int device_index=0;device_index<found_device_count;device_index++){
			if(found_device_pollfds[device_index].revents&POLLIN){
				struct input_event input_events[512];
				int read_bytes=read(found_device_pollfds[device_index].fd,input_events,sizeof(input_events));
				if(read_bytes==-1&&errno!=EINTR){
					log_write(LOG_ERR,"Read failure‽");
					exit(EXIT_FAILURE);
				}
				
				// The last event is always an EV_SYN, so start at the second-to-last and go backward…
				for(int event_index=read_bytes/sizeof(struct input_event)-2;event_index>=0;event_index--){
					// Looking for an event that is not EV_SYN or EV_LED…
					if(input_events[event_index].type!=EV_SYN&&input_events[event_index].type!=EV_LED){
						// If one is found, calculate candidate_off_time
						struct timespec candidate_off_time;
						candidate_off_time.tv_sec=input_events[event_index].input_event_sec+configured_on_sec;
						candidate_off_time.tv_nsec=input_events[event_index].input_event_usec*NSEC_PER_USEC;
						
						// If candidate_off_time is later than off_time, update off_time
						if(timespec_gt(candidate_off_time,off_time)){
							off_time=candidate_off_time;
						}
						break;
					}
				}
			}
		}
		
		struct timespec current_time;
		clock_gettime(CLOCK_MONOTONIC,&current_time);
		if(timespec_gt(off_time,current_time)){
			// If the off_time is in the future
			if(desired_brightness==0){
				// And the backlight is currently off, turn it on
				log_write(LOG_INFO,"Turning backlight on");
				start_fade(configured_brightness);
			}
			
			// Sleep until off_time
			clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&off_time,NULL);
		}else if(desired_brightness==configured_brightness){
			// Otherwise, turn it off
			log_write(LOG_INFO,"Turning backlight off");
			start_fade(0);
		}
	}
	
	pthread_cancel(fader_thread);
	pthread_join(fader_thread,NULL);
	set_brightness(brightness_file,0);
	
	return EXIT_SUCCESS;
}
