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

#define DEV_INPUT_EVENT "/dev/input"
#define EVENT_DEV_NAME "event"

#define BACKLIGHT_DEVICE "/sys/class/leds/chromeos::kbd_backlight/brightness"
#define DEFAULT_ON_SEC 10
#define DEFAULT_BRIGHTNESS 30
#define DEFAULT_FADE_DURATION_USEC 100000

// This must be set exactly to the number of elements in the array or else a segfault will occur
#define SEARCH_DEVICE_COUNT 2
static const char *search_devices[SEARCH_DEVICE_COUNT]={
	"AT Translated Set 2 keyboard",
	"PIXA3854:00 093A:0274 Touchpad",
};

static const struct option long_options[]={
	{"brightness",required_argument,0,'b'},
	{"fadeduration",required_argument,0,'f'},
	{"timeout",required_argument,0,'t'},
	{"help",no_argument,0,'h'},
	{},
};

static struct pollfd found_device_pollfds[SEARCH_DEVICE_COUNT];
static int found_device_count=0;

static struct timespec input_batch_delay={};

static pthread_t fader_thread;
static pthread_t timer_thread;

static pthread_cond_t timer_cond;
static pthread_mutex_t timer_mutex;

static pthread_cond_t fader_cond;
static pthread_mutex_t fader_mutex;

static int configured_on_sec=DEFAULT_ON_SEC;
static int configured_brightness=DEFAULT_BRIGHTNESS;
static int configured_fade_duration_nsec=DEFAULT_FADE_DURATION_USEC*NSEC_PER_USEC;

static int desired_brightness=0;
static struct timespec off_time={};

static int current_brightness=0;
static int fade_interval_nsec=0;

static FILE *brightness_file=NULL;

static bool exit_requested=false;

static bool is_daemon;

static void log_write(int priority,const char *format,...){
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

static void calculate_fade_interval(){
	fade_interval_nsec=configured_fade_duration_nsec/abs(current_brightness-desired_brightness);
}

static void exit_handler(int signal_number){
	exit_requested=true;
}

static int is_event_device(const struct dirent *dir){
	return strncmp(EVENT_DEV_NAME,dir->d_name,5)==0;
}

static long min(const long x,const long y){
	if(x<y){
		return x;
	}else{
		return y;
	}
}

static bool string_in_array(const char *string,const char *array[],int array_length){
	for(int i=0;i<array_length;i++){
		if(!strcmp(string,array[i])){
			return true;
		}
	}
	return false;
}

static int get_input_fds(struct pollfd *pollfds){
	struct dirent **namelist={};
	int device_count=scandir(DEV_INPUT_EVENT,&namelist,is_event_device,alphasort);
	int clock_type=CLOCK_MONOTONIC;
	if(device_count<=0){
		log_write(LOG_ERR,"%s directory contains no devices!",DEV_INPUT_EVENT);
		return EXIT_FAILURE;
	}
	
	for(int i=0;i<device_count&&found_device_count<=SEARCH_DEVICE_COUNT;i++){
		char device_filename[4096];
		int fd=-1;
		char device_name[256]="???";
		
		snprintf(device_filename,sizeof(device_filename),"%s/%s",DEV_INPUT_EVENT,namelist[i]->d_name);
		fd=open(device_filename,O_RDONLY);
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
		
		free(namelist[i]);
	}
	
	if(found_device_count==0){
		log_write(LOG_ERR,"No matching input devices found!  Check permissions.");
		return EXIT_FAILURE;
	}
	
	return EXIT_SUCCESS;
}

static void set_brightness(const int brightness){
	fprintf(brightness_file,"%d",brightness);
	fflush(brightness_file);
}

static int string_to_int(int *result,const int min,const int max,const char *string){
	errno=0;
	const long long_value=strtol(string,NULL,10);
	if(errno!=0){
		return errno;
	}
	if(long_value>=min&&long_value<=max){
		*result=(int)long_value;
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}

static void timespec_add_nsec(struct timespec *timespec,const long nsec){
	timespec->tv_sec+=nsec/NSEC_PER_SEC;
	timespec->tv_nsec+=nsec%NSEC_PER_SEC;
	if(timespec->tv_nsec>=NSEC_PER_SEC){
		timespec->tv_sec+=1;
		timespec->tv_nsec-=NSEC_PER_SEC;
	}
}

static int timespec_cmp(const struct timespec ts1,const struct timespec ts2){
	if(ts1.tv_sec==ts2.tv_sec&&ts1.tv_nsec==ts2.tv_nsec){
		return 0;
	}else if((ts1.tv_sec>ts2.tv_sec)||(ts1.tv_sec==ts2.tv_sec&&ts1.tv_nsec>ts2.tv_nsec)){
		return 1;
	}else{
		return -1;
	}
}

static int usage(){
	log_write(LOG_NOTICE,"Usage: keylightc [--brightness <brightness>] [--fadeduration <fadeduration>] [--timeout <timeout>]");
	log_write(LOG_NOTICE,"keylightc - automatic keyboard backlight daemon for Framework laptops");
	log_write(LOG_NOTICE,"Options:");
	log_write(LOG_NOTICE,"  --brightness\t\tbrightness level when active (1-100) [default=%d]",DEFAULT_BRIGHTNESS);
	log_write(LOG_NOTICE,"  --fadeduration\tfade time in microseconds (1-1000000) [default=%d]",DEFAULT_FADE_DURATION_USEC);
	log_write(LOG_NOTICE,"  --timeout\t\tactivity timeout in seconds (1-%d) [default=%d]",INT_MAX,DEFAULT_ON_SEC);
	log_write(LOG_NOTICE,"  --help\t\tdisplay usage information");
	return EXIT_FAILURE;
}

void *fader(void *arg){
	pthread_mutex_lock(&fader_mutex);
	
	struct timespec next_fade_step_time={};
	while(true){
		struct timespec current_time;
		clock_gettime(CLOCK_MONOTONIC,&current_time);
		
		if(current_brightness!=desired_brightness){
			// If the current brightness is not the desired brightness…
			if(timespec_cmp(current_time,next_fade_step_time)>=0){
				// …and the next fade step is due, fade in/out
				if(current_brightness>desired_brightness){
					current_brightness--;
				}else{
					current_brightness++;
				}
				set_brightness(current_brightness);
				
				// Calculate next_fade_step_time based on current_time and fade_interval_nsec
				memcpy(&next_fade_step_time,&current_time,sizeof(struct timespec));
				timespec_add_nsec(&next_fade_step_time,fade_interval_nsec);
			}
			
			// Wait until next_fade_step_time
			pthread_cond_timedwait(&fader_cond,&fader_mutex,&next_fade_step_time);
		}else{
			// Otherwise, wait to be signaled
			pthread_cond_wait(&fader_cond,&fader_mutex);
		}
	}
}

void *timer(void *arg){
	pthread_mutex_lock(&timer_mutex);
	
	while(true){
		struct timespec current_time;
		clock_gettime(CLOCK_MONOTONIC,&current_time);
		
		if(desired_brightness==0){
			// If the backlight is already off, wait to be signaled once it has been turned back on
			pthread_cond_wait(&timer_cond,&timer_mutex);
		}else if(timespec_cmp(current_time,off_time)>=0){
			// If the backlight is on and current_time is greater than or equal to off_time, turn the backlight off
			log_write(LOG_INFO,"Turning backlight off");
			pthread_mutex_lock(&fader_mutex);
			desired_brightness=0;
			calculate_fade_interval();
			pthread_cond_signal(&fader_cond);
			pthread_mutex_unlock(&fader_mutex);
		}else{
			// Otherwise, wait until off_time
			pthread_cond_timedwait(&timer_cond,&timer_mutex,&off_time);
		}
	}
}

int main(const int argc,char **argv){
	is_daemon=!isatty(1);
	if(is_daemon){
		openlog("keylightc",LOG_CONS|LOG_PID|LOG_NDELAY,LOG_DAEMON);
	}
	
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
	
	brightness_file=fopen(BACKLIGHT_DEVICE,"w");
	if(brightness_file==NULL){
		log_write(LOG_ERR,"Failed to open backlight device!  Check permissions and ensure you are running Linux kernel 6.11 or later.");
		return EXIT_FAILURE;
	}
	
	if(get_input_fds(found_device_pollfds)){
		exit(EXIT_FAILURE);
	}
	
	// Set input_batch_delay to half configured_on_sec with an upper limit of 5 seconds
	timespec_add_nsec(&input_batch_delay,min((long)configured_on_sec*NSEC_PER_SEC/2,(long)NSEC_PER_SEC*5));
	
	log_write(LOG_INFO,"Backlight timeout: %d second%s",configured_on_sec,configured_on_sec!=1?"s":"");
	log_write(LOG_INFO,"Brightness level: %d%%",configured_brightness);
	log_write(LOG_INFO,"Fade duration: %d microsecond%s",configured_fade_duration_nsec/NSEC_PER_USEC,configured_fade_duration_nsec/NSEC_PER_USEC!=1?"s":"");
	
	pthread_condattr_t timer_condattr;
	pthread_condattr_init(&timer_condattr);
	pthread_condattr_setclock(&timer_condattr,CLOCK_MONOTONIC);
	pthread_cond_init(&timer_cond,&timer_condattr);
	pthread_mutex_init(&timer_mutex,NULL);
	
	pthread_condattr_t fader_condattr;
	pthread_condattr_init(&fader_condattr);
	pthread_condattr_setclock(&fader_condattr,CLOCK_MONOTONIC);
	pthread_cond_init(&fader_cond,&timer_condattr);
	pthread_mutex_init(&fader_mutex,NULL);
	
	pthread_create(&fader_thread,NULL,fader,NULL);
	pthread_create(&timer_thread,NULL,timer,NULL);
	
	// Set up the exit handler
	struct sigaction exit_action;
	exit_action.sa_handler=exit_handler;
	sigaction(SIGINT,&exit_action,NULL);
	sigaction(SIGTERM,&exit_action,NULL);
	
	set_brightness(0);
	
	struct timespec latest_event_time={};
	while(true){
		if(exit_requested){
			break;
		}
		
		if(poll(found_device_pollfds,found_device_count,-1)==-1){
			if(errno!=EINTR){
				log_write(LOG_ERR,"Poll failure‽");
				exit(EXIT_FAILURE);
			}
		}
		
		bool new_event=false;
		for(int i=0;i<found_device_count;i++){
			if(found_device_pollfds[i].revents&POLLIN){
				struct input_event input_event[512];
				int read_bytes=read(found_device_pollfds[i].fd,input_event,sizeof(input_event));
				if(read_bytes==-1){
					if(errno!=EINTR){
						log_write(LOG_ERR,"Read failure‽");
						exit(EXIT_FAILURE);
					}
				}
				
				// Get the index of the last event, which will be the latest event for this device
				int last_event_index=read_bytes/sizeof(struct input_event)-1;
				
				// Convert the event time into a timespec and update latest_event_time if it is more recent
				struct timespec event_time;
				event_time.tv_sec=input_event[last_event_index].input_event_sec;
				event_time.tv_nsec=input_event[last_event_index].input_event_usec*NSEC_PER_USEC;
				if(timespec_cmp(event_time,latest_event_time)>=0){
					memcpy(&latest_event_time,&event_time,sizeof(struct timespec));
					new_event=true;
				}
			}
		}
		
		if(new_event){
			pthread_mutex_lock(&timer_mutex);
			// Set off_time to latest_event_time plus configured_on_sec
			off_time.tv_sec=latest_event_time.tv_sec+configured_on_sec;
			off_time.tv_nsec=latest_event_time.tv_nsec;
			
			// If the backlight is currently off…
			if(desired_brightness==0){
				// Turn it on
				pthread_mutex_lock(&fader_mutex);
				log_write(LOG_INFO,"Turning backlight on");
				desired_brightness=configured_brightness;
				calculate_fade_interval();
				pthread_cond_signal(&fader_cond);
				pthread_mutex_unlock(&fader_mutex);
				
				// And signal the timer thread to start timing down to turn it off
				pthread_cond_signal(&timer_cond);
			}
			pthread_mutex_unlock(&timer_mutex);
			
			// Sleep here to prevent spinning and using too much CPU
			nanosleep(&input_batch_delay,NULL);
		}
	}
	
	pthread_cancel(fader_thread);
	pthread_cancel(timer_thread);
	pthread_join(fader_thread,NULL);
	pthread_join(timer_thread,NULL);
	set_brightness(0);
	
	if(is_daemon){
		closelog();
	}
	
	return EXIT_SUCCESS;
}
