/* 
qrq - High speed morse trainer, similar to the DOS classic "Rufz"
Copyright (C) 2006-2013  Fabian Kurz

$Id: qrq.c 564 2013-01-06 13:25:46Z dj1yfk $

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/ 
#if WIN32
#define WIN_THREADS
#endif

#ifndef WIN_THREADS
#include <pthread.h>			/* CW output will be in a separate thread */
#endif
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>				/* basename */
#include <ctype.h>
#include <time.h> 
#include <limits.h> 			/* PATH_MAX */
#include <dirent.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>			/* mkdir */
#include <sys/types.h>
#include <errno.h>
#ifdef WIN32
#include <windows.h>
#endif

#define PI M_PI

#define SILENCE 0		/* Waveforms for the tone generator */
#define SINE 1
#define SAWTOOTH 2
#define SQUARE 3

#ifndef DESTDIR
#	define DESTDIR "/usr"
#endif

#ifndef VERSION
#  define VERSION "0.0.0"
#endif

#ifdef CA
#include "coreaudio.h"
typedef void *AUDIO_HANDLE;
#endif

#ifdef OSS
#include "oss.h"
#define write_audio(x, y, z) write(x, y, z)
#define close_audio(x) close(x)
typedef int AUDIO_HANDLE;
#endif

#ifdef PA
#include "pulseaudio.h"
typedef void *AUDIO_HANDLE;
#endif

/* callsign array will be dynamically allocated */
static char **calls = NULL;

const static char *codetable[] = {
".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..",".---",
"-.-",".-..","--","-.","---",".--.","--.-",".-.","...","-","..-","...-",
".--","-..-","-.--","--..","-----",".----","..---","...--","....-",".....",
"-....", "--...","---..","----."};

/* List of available callbase files. Probably no need to do dynamic memory allocation for that list.... */

static char cblist[100][PATH_MAX];

static char mycall[15]="DJ1YFK";		/* mycall. will be read from qrqrc */
static char dspdevice[PATH_MAX]="/dev/dsp";	/* will also be read from qrqrc */
static int score = 0;					/* qrq score */
static int sending_complete;			/* global lock for "enter" while sending */
static int callnr = 0;					/* nr of actual call in attempt */
static int initialspeed=200;			/* initial speed. to be read from file*/
static int mincharspeed=0;				/* min. char. speed, below: farnsworth*/
static int speed=200;					/* current speed in cpm */
static int maxspeed=0;
static int freq=800;					/* current cw sidetone freq */
static int errornr=0;					/* number of errors in attempt */
static int p=0;							/* position of cursor, relative to x */
static int status=1;					/* 1= attempt, 2=config */
static int mode=1;						/* 0 = overwrite, 1 = insert */
static int j=0;							/* counter etc. */
static int constanttone=0;              /* if 1 don't change the pitch */
static int ctonefreq=800;               /* if constanttone=1 use this freq */
static int f6=0;						/* f6 = 1: allow unlimited repeats */
static int fixspeed=0;					/* keep speed fixed, regardless of err*/
static int unlimitedattempt=0;			/* attempt with all calls  of the DB */
static int attemptvalid=1;				/* 1 = not using any "cheats" */
static unsigned long int nrofcalls=0;	

long samplerate=44100;
static long long_i;
static int waveform = SINE;				/* waveform: (0 = none) */
static char wavename[10]="Sine    ";	/* Name of the waveform */
static double edge=2.0;						/* rise/fall time in milliseconds */
static int ed;							/* risetime, normalized to samplerate */

static short buffer[88200];
static int full_buf[882000];  /* 20 second max buffer */
static int full_bufpos = 0;

AUDIO_HANDLE dsp_fd;

static int display_toplist();
static int calc_score (char * realcall, char * input, int speed, char * output);
static int update_score();
static int show_error (char * realcall, char * wrongcall); 
static int clear_display();
static int add_to_toplist(char * mycall, int score, int maxspeed);
static int read_config();
static int save_config();
static int tonegen(int freq, int length, int waveform);
static void *morse(void * arg); 
static int add_to_buf(void* data, int size);
static int readline(WINDOW *win, int y, int x, char *line, int i); 
static void thread_fail (int j);
static int check_toplist ();
static int find_files ();
static int statistics ();
static int read_callbase ();
static void find_callbases();
static void select_callbase ();
static void help ();
static void callbase_dialog();
static void parameter_dialog();
static int clear_parameter_display();
static void update_parameter_dialog();

#ifdef WIN_THREADS
HANDLE cwthread;
#else
pthread_t cwthread;				/* thread for CW output, to enable
								   keyboard reading at the same time */
pthread_attr_t cwattr;
#endif

char rcfilename[PATH_MAX]="";			/* filename and path to qrqrc */
char tlfilename[PATH_MAX]="";			/* filename and path to toplist */
char cbfilename[PATH_MAX]="";			/* filename and path to callbase */

char destdir[PATH_MAX]="";


/* create windows */
WINDOW *top_w;					/* actual score					*/
WINDOW *mid_w;					/* callsign history/mistakes	*/
WINDOW *conf_w;					/* parameter config display	*/
WINDOW *bot_w;					/* user input line				*/
WINDOW *inf_w;					/* info window for param displ	*/
WINDOW *right_w;				/* highscore list/settings		*/


int main (int argc, char *argv[]) {

  /* if built as osx bundle set DESTDIR to Resources dir of bundle */
#ifdef OSX_BUNDLE
  char tempdir[PATH_MAX]="";
  char* p_slash = strrchr(argv[0], '/');
  strncpy(tempdir, argv[0], p_slash - argv[0]);
  p_slash = strrchr(tempdir, '/');
  strncpy(destdir, tempdir, p_slash - tempdir);
  strcat(destdir, "/Resources");
#else
  strcpy(destdir, DESTDIR);
#endif

	char abort = 0;
	char tmp[80]="";
	char input[15]="";
	int i=0,j=0,k=0;						/* counter etc. */
	char previouscall[80]="";
	int previousfreq = 0;
	int f6pressed=0;

	if (argc > 1) {
		help();
	}
	
	(void) initscr();
	cbreak();
	noecho();
	curs_set(FALSE);
	keypad(stdscr, TRUE);
	scrollok(stdscr, FALSE);
	
	printw("qrq v%s - Copyright (C) 2006-2013 Fabian Kurz, DJ1YFK\n", VERSION);
	printw("This is free software, and you are welcome to redistribute it\n");
	printw("under certain conditions (see COPYING).\n");

	refresh();

	/* search for 'toplist', 'qrqrc' and callbase.qcb and put their locations
	 * into tlfilename, rcfilename, cbfilename */
	find_files();

	/* check if the toplist is in the suitable format. as of 0.0.7, each line
	 * is 31 characters long, with the added time stamp */
	check_toplist();

	/* buffer for audio */
	for (long_i=0;long_i<88200;long_i++) {
		buffer[long_i]=0;
	}
	
	/* random seed from time */
	srand( (unsigned) time(NULL) ); 

#ifndef WIN_THREADS
	/* Initialize cwthread. We have to wait for the cwthread to finish before
	 * the next cw output can be made, this will be done with pthread_join */
	pthread_attr_init(&cwattr);
	pthread_attr_setdetachstate(&cwattr, PTHREAD_CREATE_JOINABLE);
#endif
	
	/****** Reading configuration file ******/
	printw("\nReading configuration file qrqrc \n");
	read_config();

	attemptvalid = 1;
	if (f6 || fixspeed || unlimitedattempt) {
		attemptvalid = 0;	
	}

	/****** Reading callsign database ******/
	printw("\nReading callsign database... ");
	nrofcalls = read_callbase();

	printw("done. %d calls read.\n\n", nrofcalls);
	printw("Press any key to continue...");

	refresh();
	getch();

	erase();
	refresh();

	top_w = newwin(4, 60, 0, 0);
	mid_w = newwin(17, 60, 4, 0);
	conf_w = newwin(17, 60, 4, 0);
	bot_w = newwin(3, 60, 21, 0);
	inf_w = newwin(3, 60, 21, 0);
	right_w = newwin(24, 20, 0, 60);

	werase(top_w);
	werase(mid_w);
	werase(conf_w);
	werase(bot_w);
	werase(inf_w);
	werase(right_w);

	keypad(bot_w, TRUE);
	keypad(mid_w, TRUE);
	keypad(conf_w, TRUE);

#ifdef WIN_THREADS
	cwthread = (HANDLE) _beginthread( morse,0,"QRQ");
#else
	/* no need to join here, this is the first possible time CW is sent */
	pthread_create(&cwthread, NULL, & morse, (void *) "QRQ");
#endif

/* very outter loop */
while (1) {	

/* status 1 = running an attempt of 50 calls */	
while (status == 1) {
	box(top_w,0,0);
	box(conf_w,0,0);
	box(mid_w,0,0);
	box(bot_w,0,0);
	box(inf_w,0,0);
	box(right_w,0,0);
	wattron(top_w,A_BOLD);
	mvwaddstr(top_w,1,1, "QRQ v");
	mvwaddstr(top_w,1,6, VERSION);
	wattroff(top_w, A_BOLD);
	mvwaddstr(top_w,1,11, " by Fabian Kurz, DJ1YFK");
	mvwaddstr(top_w,2,1, "Homepage and Toplist: http://fkurz.net/ham/qrq.html"
					"     ");

	clear_display();
	wattron(mid_w,A_BOLD);
	mvwaddstr(mid_w,1,1, "Usage:");
	mvwaddstr(mid_w,10,2, "F6                          F10       ");
	wattroff(mid_w, A_BOLD);
	mvwaddstr(mid_w,2,2, "After entering your callsign, 50 random callsigns");
	mvwaddstr(mid_w,3,2, "from a database will be sent. After each callsign,");
	mvwaddstr(mid_w,4,2, "enter what you have heard. If you copied correctly,");
	mvwaddstr(mid_w,5,2, "full points are credited and the speed increases by");
	mvwaddstr(mid_w,6,2, "2 WpM -- otherwise the speed decreases and only a ");
	mvwaddstr(mid_w,7,2, "fraction of the points, depending on the number of");
	mvwaddstr(mid_w,8,2, "errors is credited.");
	mvwaddstr(mid_w,10,2, "F6 repeats a callsign once, F10 quits.");
	mvwaddstr(mid_w,12,2, "Settings can be changed with F5 (or in qrqrc).");
#ifndef WIN32
	mvwaddstr(mid_w,14,2, "Score statistics (requires gnuplot) with F7.");
#endif

	wattron(right_w,A_BOLD);
	mvwaddstr(right_w,1, 6, "Toplist");
	wattroff(right_w,A_BOLD);

	display_toplist();

	p=0;						/* cursor to start position */
	wattron(bot_w,A_BOLD);
	mvwaddstr(bot_w, 1, 1, "Please enter your callsign:                      ");
	wattroff(bot_w,A_BOLD);
	
	wrefresh(top_w);
	wrefresh(mid_w);
	wrefresh(bot_w);
	wrefresh(right_w); 
	
	/* reset */
	maxspeed = errornr = score = 0;
	speed = initialspeed;
	
	/* prompt for own callsign */
	i = readline(bot_w, 1, 30, mycall, 1);

	/* F5 -> Configure sound */
	if (i == 5) {
		parameter_dialog();
		break;
	} 
	/* F6 -> play test CW */
	else if (i == 6) {
		freq = constanttone ? ctonefreq : 800;
#ifdef WIN_THREADS
		 WaitForSingleObject(cwthread,INFINITE);
		 cwthread = (HANDLE) _beginthread( morse,0,"VVVTEST");
#else
		pthread_join(cwthread, NULL);
		j = pthread_create(&cwthread, NULL, &morse, (void *) "VVVTEST");	
		thread_fail(j);
#endif
		break;
	}
	else if (i == 7) {
#ifndef WIN32
		statistics();
#endif
		break;
	}

	if (strlen(mycall) == 0) {
		strcpy(mycall, "NOCALL");
	}
	else if (strlen(mycall) > 7) {		/* cut excessively long calls */
		mycall[7] = '\0';
	}
	
	clear_display();
	wrefresh(mid_w);
	
	/* update toplist (highlight may change) */
	display_toplist();

	mvwprintw(top_w,1,1,"                                      ");
	mvwprintw(top_w,2,1,"                                               ");
	mvwprintw(top_w,1,1,"Callsign:");
	wattron(top_w,A_BOLD);
	mvwprintw(top_w,1,11, "%s", mycall);
	wattroff(top_w,A_BOLD);
	update_score();
	wrefresh(top_w);


	/* Reread callbase */
	nrofcalls = read_callbase();

	/****** send 50 or unlimited calls, ask for input, score ******/
	
	for (callnr=1; callnr < (unlimitedattempt ? nrofcalls : 51); callnr++) {
		/* Make sure to wait for the cwthread of the previous callsign, if
		 * necessary. */
#ifdef WIN_THREADS
		WaitForSingleObject(cwthread,INFINITE);
#else
		pthread_join(cwthread, NULL);
#endif	
		/* select an unused callsign from the calls-array */
		do {
			i = (int) ((float) nrofcalls*rand()/(RAND_MAX+1.0));
		} while (calls[i] == NULL);

		/* only relevant for callbases with less than 50 calls */
		if (nrofcalls == callnr) { 		/* Only one call left!" */
				callnr =  51; 			/* Get out after next one */
		}



		/* output frequency handling a) random b) fixed */
		if ( constanttone == 0 ) {
				/* random freq, fraction of samplerate */
				freq = (int) (samplerate/(50+(40.0*rand()/(RAND_MAX+1.0))));
		}
		else { /* fixed frequency */
				freq = ctonefreq;
		}

		mvwprintw(bot_w,1,1,"                                      ");
		mvwprintw(bot_w, 1, 1, "%3d/%s", callnr, unlimitedattempt ? "-" : "50");	
		wrefresh(bot_w);	
		tmp[0]='\0';

		/* starting the morse output in a separate process to make keyboard
		 * input and echoing at the same time possible */
		
		sending_complete = 0;	
#ifdef WIN_THREADS
		cwthread = (HANDLE) _beginthread( morse,0,calls[i]);
#else
		j = pthread_create(&cwthread, NULL, morse, calls[i]);	
		thread_fail(j);		
#endif
		
		f6pressed=0;

		while (!abort && (j = readline(bot_w, 1, 8, input,1)) > 4) {/* F5..F10 pressed */

			switch (j) {
				case 6:		/* repeat call */
				if (f6pressed && (f6 == 0)) {
					continue;
				}
				f6pressed=1;
				/* wait for old cwthread to finish, then send call again */
			
#ifdef WIN_THREADS
			WaitForSingleObject(cwthread,INFINITE);
			cwthread = (HANDLE) _beginthread( morse,0,calls[i]);
#else
			pthread_join(cwthread, NULL);
			j = pthread_create(&cwthread, NULL, &morse, calls[i]);	
			thread_fail(j);
#endif	
					break; /* 6*/
				case 7:		/* repeat _previous_ call */
					if (callnr > 1) {
						k = freq;
						freq = previousfreq;
#ifdef WIN_THREADS
			WaitForSingleObject(cwthread,INFINITE);
			cwthread = (HANDLE) _beginthread( morse,0,previouscall);
			WaitForSingleObject(cwthread,INFINITE);
#else
			pthread_join(cwthread, NULL);
			j = pthread_create(&cwthread, NULL, &morse, previouscall);	
			thread_fail(j);
			pthread_join(cwthread, NULL);
#endif	
						/* NB: We must wait for the CW thread before
						 * we set the freq back -- this blocks keyboard
						 * input, but in this case it shouldn't matter */
						freq = k;
					}
					break;
				case 10:	/* abort attempt */
					abort = 1;
					continue;
					break;
			}
					
		}

		
		if (abort) {
			abort = 0;
			input[0]='\0';
			break;
		}
		
		tmp[0]='\0';	
		score += calc_score(calls[i], input, speed, tmp);
		update_score();
		if (strcmp(tmp, "*")) {			/* made an error */
				show_error(calls[i], tmp);
		}
		input[0]='\0';
		strncpy(previouscall, calls[i], 80);
		previousfreq = freq;
		calls[i] = NULL;
	}

	/* attempt is over, send AR */
	callnr = 0;
	
#ifdef WIN_THREADS
		 WaitForSingleObject(cwthread,INFINITE);
		 cwthread = (HANDLE) _beginthread( morse,0,"+");
#else
		pthread_join(cwthread, NULL);
		j = pthread_create(&cwthread, NULL, &morse, (void *) "+");	
		thread_fail(j);
#endif
	
	add_to_toplist(mycall, score, maxspeed);
	
	curs_set(0);
	wattron(bot_w,A_BOLD);
	mvwprintw(bot_w,1,1, "Attempt finished. Press any key to continue!");
	wattroff(bot_w,A_BOLD);
	wrefresh(bot_w);
	getch();
	mvwprintw(bot_w,1,1, "                                            ");
	curs_set(1);

	
} /* while (status == 1) */


} /* very outter loop */

	getch();
	endwin();
	delwin(top_w);
	delwin(bot_w);
	delwin(mid_w);
	delwin(right_w);
	getch();
	return 0;
}



/* (formerly status == 2). Change parameters */

void parameter_dialog () {

int j = 0;


update_parameter_dialog();

while ((j = getch()) != 0) {

	switch ((int) j) {
		case '+':							/* rise/falltime */
			if (edge <= 9.0) {
				edge += 0.1;
			}
			break;
		case '-':
			if (edge > 0.1) {
				edge -= 0.1;
			}
			break;
		case 'w':							/* change waveform */
			waveform = ((waveform + 1) % 3)+1;	/* toggle 1-2-3 */
			break;
		case 'k':							/* constanttone */
			if (ctonefreq >= 160) {
				ctonefreq -= 10;
			}
			else {
					constanttone = 0;
			}
			break;
		case 'l':
			if (constanttone == 0) {
				constanttone = 1;
			}
			else if (ctonefreq < 1600) {
				ctonefreq += 10;
			}
			break;
		case '0':
			if (constanttone == 1) {
				constanttone = 0;
			}
			else {
				constanttone = 1;
			}
			break;
		case 'f':
				f6 = (f6 ? 0 : 1);
			break;
		case 's':
				fixspeed = (fixspeed ? 0 : 1);
			break;
		case 'u':
				unlimitedattempt = (unlimitedattempt ? 0 : 1);
			break;
		case KEY_UP: 
			initialspeed += 10;
			break;
		case KEY_DOWN:
			if (initialspeed > 10) {
				initialspeed -= 10;
			}
			break;
		case KEY_RIGHT:
			mincharspeed += 10;
			break;
		case KEY_LEFT:
			if (mincharspeed > 10) {
				mincharspeed -= 10;
			}
			break;
		case 'c':
			readline(conf_w, 5, 25, mycall, 1);
			if (strlen(mycall) == 0) {
				strcpy(mycall, "NOCALL");
			}
			else if (strlen(mycall) > 7) {	/* cut excessively long calls */
				mycall[7] = '\0';
			}
			p=0;							/* cursor position */
			break;
#ifdef OSS
		case 'e':
			readline(conf_w, 12, 25, dspdevice, 0);
			if (strlen(dspdevice) == 0) {
				strcpy(dspdevice, "/dev/dsp");
			}
			p=0;							/* cursor position */
			break;
#endif
		case 'd':							/* go to database browser */
			if (!callnr) {					/* Only allow outside of attempt */
				curs_set(1);
				callbase_dialog();
			}
			break;
		case KEY_F(2):
			save_config();	
			mvwprintw(conf_w,15,39, "Config saved!");
			wrefresh(conf_w);
#ifdef WIN32
			Sleep(1000);
#else
			sleep(1);	
#endif
			break;
		case KEY_F(6):
			freq = constanttone ? ctonefreq : 800;
#ifdef WIN_THREADS
		 WaitForSingleObject(cwthread,INFINITE);
		 cwthread = (HANDLE) _beginthread( morse,0,"TESTING");
#else
		pthread_join(cwthread, NULL);
		j = pthread_create(&cwthread, NULL, &morse, (void *) "TESTING");	
		thread_fail(j);
#endif
			break;
		case KEY_F(10):
		case KEY_F(3):
			curs_set(1);
			clear_parameter_display();
			wrefresh(conf_w);
			/* restore old windows */
			touchwin(mid_w);
			touchwin(bot_w);
			wrefresh(mid_w);
			wrefresh(bot_w);
			return;
	}

	speed = initialspeed;

	attemptvalid = 1;
	if (f6 || fixspeed || unlimitedattempt) {
		attemptvalid = 0;	
	}

	update_parameter_dialog();

} /* while 1 (return only by F3/F10) */

} /* parameter_dialog */


/* update_parameter_dialog 
 * repaints the whole config/parameter screen (F5) */


void update_parameter_dialog () {

	clear_parameter_display();
	switch (waveform) {
		case SINE:
			strcpy(wavename, "Sine    ");
			break;
		case SAWTOOTH:
			strcpy(wavename, "Sawtooth");
			break;
		case SQUARE:
			strcpy(wavename, "Square  ");
			break;
	}

	mvwaddstr(inf_w,1,1, "                                                         ");
	curs_set(0);
	wattron(conf_w,A_BOLD);
	mvwaddstr(conf_w,1,1, "Configuration:          Value                Change");
	mvwprintw(conf_w,14,2, "      F6                    F10            ");
	mvwprintw(conf_w,15,2, "      F2");
	wattroff(conf_w, A_BOLD);
	mvwprintw(conf_w,2,2, "Initial Speed:         %3d CpM / %3d WpM" 
					"    up/down", initialspeed, initialspeed/5);
	mvwprintw(conf_w,3,2, "Min. character Speed:  %3d CpM / %3d WpM" 
					"    left/right", mincharspeed, mincharspeed/5);
	mvwprintw(conf_w,4,2, "CW rise/falltime (ms): %1.1f           " 
					"       +/-", edge);
	mvwprintw(conf_w,5,2, "Callsign:              %-14s" 
					"       c", mycall);
	mvwprintw(conf_w,6,2, "CW pitch (0 = random): %-4d"
					"                 k/l or 0", (constanttone)?ctonefreq : 0);
	mvwprintw(conf_w,7,2, "CW waveform:           %-8s"
					"             w", wavename);
	mvwprintw(conf_w,8,2, "Allow unlimited F6*:   %-3s"
					"                  f", (f6 ? "yes" : "no"));
	mvwprintw(conf_w,9,2, "Fixed CW speed*:       %-3s"
					"                  s", (fixspeed ? "yes" : "no"));
	mvwprintw(conf_w,10,2, "Unlimited attempt*:    %-3s"
					"                  u", (unlimitedattempt ? "yes" : "no"));
	if (!callnr) {
		mvwprintw(conf_w,11,2, "Callsign database:     %-15s"
					"      d (%d)", basename(cbfilename),nrofcalls);
	}
#ifdef OSS
	mvwprintw(conf_w,12,2, "DSP device:            %-15s"
					"      e", dspdevice);
#endif
	mvwprintw(conf_w,14,2, "Press");
	mvwprintw(conf_w,14,11, "to play sample CW,");
	mvwprintw(conf_w,14,34, "to go back.");
	mvwprintw(conf_w,15,2, "Press");
	mvwprintw(conf_w,15,11, "to save config permanently.");
	mvwprintw(inf_w,1,1, "          * Makes scores ineligible for toplist");
	wrefresh(conf_w);
	wrefresh(inf_w);
	

} /* update_parameter_dialog */




void callbase_dialog () {

	clear_parameter_display();

	wattron(conf_w,A_BOLD);
	mvwaddstr(conf_w,1,1, "Change Callsign Database");
	wattroff(conf_w,A_BOLD);
#if WIN32
	mvwprintw(conf_w,3,1, ".qcb files found:");
#else
	mvwprintw(conf_w,3,1, ".qcb files found (in %s/share/qrq/ and ~/.qrq/):",destdir);
#endif

	/* populate cblist */	
	find_callbases();
	/* selection dialog */
	select_callbase();
	wrefresh(conf_w);

	nrofcalls = read_callbase();


	return;	/* back to config menu */
}














/* reads a callsign etc. in *win at y/x and writes it to *line */

static int readline(WINDOW *win, int y, int x, char *line, int capitals) {
	int c;						/* character we read */
	int i=0;

	if (strlen(line) == 0) {p=0;}	/* cursor to start if no call in buffer */
	
	if (mode == 1) { 
		mvwaddstr(win,1,55,"INS");
	}
	else {
		mvwaddstr(win,1,55,"OVR");
	}

	mvwaddstr(win,y,x,line);
	wmove(win,y,x+p);
	wrefresh(win);
	curs_set(TRUE);
	
	while (1) {
		c = wgetch(win);
		if (c == '\n' && sending_complete)
			break;

		if (((c > 64 && c < 91) || (c > 96 && c < 123) || (c > 47 && c < 58)
					 || c == '/') && strlen(line) < 14) {
	
			line[strlen(line)+1]='\0';
			if (capitals) {
				c = toupper(c);
			}
			if (mode == 1) {						/* insert */
				for(i=strlen(line);i > p; i--) {	/* move all chars by one */
					line[i] = line[i-1];
				}
			} 
			line[p]=c;						/* insert into gap */
			p++;
		}
		else if ((c == KEY_BACKSPACE || c == 127 || c == 9 || c == 8)
						&& p != 0) {					/* BACKSPACE */
			for (i=p-1;i < strlen(line); i++) {
				line[i] =  line[i+1];
			}
			p--;
		}
		else if (c == KEY_DC && strlen(line) != 0) {		/* DELETE */ 
			p++;
			for (i=p-1;i < strlen(line); i++) {
				line[i] =  line[i+1];
			}
			p--;
		}
		else if (c == KEY_LEFT && p != 0) {
			p--;	
		}
		else if (c == KEY_RIGHT && p < strlen(line)) {
			p++;
		}
		else if (c == KEY_HOME) {
			p = 0;
		}
		else if (c == KEY_END) {
			p = strlen(line);
		}
		else if (c == KEY_IC) {						/* INS/OVR */
			if (mode == 1) { 
				mode = 0; 
				mvwaddstr(win,1,55,"OVR");
			}
			else {
				mode = 1;
				mvwaddstr(win,1,55,"INS");
			}
		}
		else if (c == KEY_PPAGE && callnr && !attemptvalid) {
			speed += 5;
			update_score();
			wrefresh(top_w);
		}
		else if (c == KEY_NPAGE && callnr && !attemptvalid) {
			if (speed > 20) speed -= 5;
			update_score();
			wrefresh(top_w);
		}
		else if (c == KEY_F(5)) {
			parameter_dialog();
		}
		else if (c == KEY_F(6)) {
			return 6;
		}
		else if (c == KEY_F(7)) {
			return 7;
		}
		else if (c == KEY_F(10)) {				/* quit */
			if (callnr) {						/* quit attempt only */
				return 10;
			} 
			/* else: quit program */
			endwin();
			printf("Thanks for using 'qrq'!\nYou can submit your"
					" highscore to http://fkurz.net/ham/qrqtop.php\n");
			/* make sure that no more output is running, then send 73 & quit */
			speed = 200; freq = 800;
#ifdef WIN_THREADS
		 WaitForSingleObject(cwthread,INFINITE);
		 cwthread = (HANDLE) _beginthread( morse,0,"73");
		 WaitForSingleObject(cwthread,INFINITE);
#else
		pthread_join(cwthread, NULL);
		j = pthread_create(&cwthread, NULL, &morse, (void *) "73");	
		thread_fail(j);
			/* make sure the cw thread doesn't die with the main thread */
			/* Exit the whole main thread */
			pthread_join(cwthread, NULL);
#endif
			exit(0);
		}
		
		mvwaddstr(win,y,x,"                ");
		mvwaddstr(win,y,x,line);
		wmove(win,y,x+p);
		wrefresh(win);
	}
	curs_set(FALSE);
	return 0;
}

/* Read toplist and diplay first 10 entries */
static int display_toplist () {
	FILE * fh;
	int i=0;
	char tmp[35]="";
	if ((fh = fopen(tlfilename, "a+")) == NULL) {
		endwin();
		fprintf(stderr, "Couldn't read or create file '%s'!", tlfilename);
		exit(EXIT_FAILURE);
	}
	rewind(fh);				/* a+ -> end of file, we want the beginning */
	(void) fgets(tmp, 34, fh);		/* first line not used */
	while ((feof(fh) == 0) && i < 20) {
		i++;
		if (fgets(tmp, 34, fh) != NULL) {
			tmp[17]='\0';
			if (strstr(tmp, mycall)) {		/* highlight own call */
				wattron(right_w, A_BOLD);
			}
			mvwaddstr(right_w,i+2, 2, tmp);
			wattroff(right_w, A_BOLD);
		}
	}
	fclose(fh);
	wrefresh(right_w);
	return 0;
}

/* calculate score depending on number of errors and speed.
 * writes the correct call and entered call with highlighted errors to *output
 * and returns the score for this call
 *
 * in training modes (unlimited attempts, f6, fixed speed), no points.
 * */
static int calc_score (char * realcall, char * input, int spd, char * output) {
	int i,x,m=0;

	x = strlen(realcall);

	if (strcmp(input, realcall) == 0) {		 /* exact match! */
		output[0]='*';						/* * == OK, no mistake */
		output[1]='\0';	
		if (speed > maxspeed) {maxspeed = speed;}
		if (!fixspeed) speed += 10;
		if (attemptvalid) {
			return 2*x*spd;						/* score */
		}
		else {
			return 0;
		}
	}
	else {									/* assemble error string */
		errornr += 1;
		if (strlen(input) >= x) {x =  strlen(input);}
		for (i=0;i < x;i++) {
			if (realcall[i] != input[i]) {
				m++;								/* mistake! */
				output[i] = tolower(input[i]);		/* print as lower case */
			}
			else {
				output[i] = input[i];
			}
		}
		output[i]='\0';
		if ((speed > 29) && !fixspeed) {speed -= 10;}

		/* score when 1-3 mistakes was made */
		if ((m < 4) && attemptvalid) {
			return (int) (2*x*spd)/(5*m);
		}
		else {return 0;};
	}
}

/* print score, current speed and max speed to window */
static int update_score() {
	mvwaddstr(top_w,1,20, "Score:                         ");
	mvwaddstr(top_w,2,20, "Speed:     CpM/    WpM, Max:    /  ");
	if (attemptvalid) {
		mvwprintw(top_w, 1, 27, "%6d", score);	
	}
	else {
		mvwprintw(top_w, 1, 27, "[training mode]", score);	
	}
	mvwprintw(top_w, 2, 27, "%3d", speed);	
	mvwprintw(top_w, 2, 35, "%3d", speed/5);	
	mvwprintw(top_w, 2, 49, "%3d", maxspeed);	
	mvwprintw(top_w, 2, 54, "%3d", maxspeed/5);	
	wrefresh(top_w);
	return 0;
}

/* display the correct callsign and what the user entered, with mistakes
 * highlighted. */
static int show_error (char * realcall, char * wrongcall) {
	int x=2;
	int y = errornr;
	int i;

	/* Screen is full of errors. Remove them and start at the beginning */
	if (errornr == 31) {	
		for (i=1;i<16;i++) {
			mvwaddstr(mid_w,i,2,"                                        "
							 "          ");
		}
		errornr = y = 1;
	}

	/* Move to second column after 15 errors */	
	if (errornr > 15) {
		x=30; y = (errornr % 16)+1;
	}

	mvwprintw(mid_w,y,x, "%-13s %-13s", realcall, wrongcall);
	wrefresh(mid_w);		
	return 0;
}

/* clear error display */
static int clear_display() {
	int i;
	for (i=1;i<16;i++) {
		mvwprintw(mid_w,i,1,"                                 "
										"                        ");
	}
	return 0;
}

/* clear parameter display */
static int clear_parameter_display() {
	int i;
	for (i=1;i<16;i++) {
		mvwprintw(conf_w,i,1,"                                 "
										"                        ");
	}
	return 0;
}


/* write entry into toplist at the right place 
 * going down from the top of the list until the score in the current line is
 * lower than the score made. then */

static int add_to_toplist(char * mycall, int score, int maxspeed) {
	FILE *fh;	
	char *part1, *part2;
	char tmp[35]="";
	char insertline[35]="DJ1YFK     36666 333 1111111111";		/* example */
						/* call       pts   max timestamp */
	int i=0, k=0, j=0;
	int pos = 0;		/* position where first score < our score appears */
	int timestamp = 0;
	int len = 32;		/* length of a line. 32 for unix, 33 for Win */

	/* For the training modes */
	if (score == 0) {
		return 0;
	}

	timestamp = (int) time(NULL);
	sprintf(insertline, "%-10s%6d %3d %10d", mycall, score, maxspeed, timestamp);
	
	if ((fh = fopen(tlfilename, "rb+")) == NULL) {
		printf("Unable to open toplist file %s!\n", tlfilename);
		exit(EXIT_FAILURE);
	}

	/* find out if we use CRLF or just LF */
	fgets(tmp, 35, fh);
	if (tmp[31] == '\r') {	/* CRLF */
		len = 33;
		strcat(insertline, "\r\n");
	}
	else {
		len = 32;
		strcat(insertline, "\n");
	}

	fseek(fh, 0, SEEK_END);
	j = ftell(fh);

	part1 = malloc((size_t) j);
	part2 = malloc((size_t) j + len);	/* one additional entry */

	rewind(fh);

	/* read whole toplist */
	fread(part1, sizeof(char), (size_t) j, fh);

	/* find first score below "score"; scores at positions 10 + (i*len) */

	do {
		for (i = 0 ; i < 6 ; i++) {	
			tmp[i] = part1[i + (10 + pos*len)];
		}
		k = atoi(tmp);
		pos++;
	} while (score < k);
 
	/* Found it! Insert own score here! */
	memcpy(part2, part1, len * (pos-1));
	memcpy(part2 + len * (pos - 1), insertline, len);
	memcpy(part2 + len * pos , part1 + len * (pos -1), j - len * (pos - 1));

	rewind(fh);
	fwrite(part2, sizeof(char), (size_t) j + len, fh);
	fclose(fh);

	free(part1);
	free(part2);

	return 0;

}


/* Read config file 
 *
 * TODO contains too much copypasta. write proper function to parse a key=value
 *
 * */

static int read_config () {
	FILE *fh;
	char tmp[80]="";
	int i=0;
	int k=0;
	int line=0;

	if ((fh = fopen(rcfilename, "r")) == NULL) {
		endwin();
		fprintf(stderr, "Unable to open config file %s!\n", rcfilename);
		exit(EXIT_FAILURE);
	}

	while ((feof(fh) == 0) && (fgets(tmp, 80, fh) != NULL)) {
		i=0;
		line++;
		tmp[strlen(tmp)-1]='\0';

		/* find callsign, speed etc. 
		 * only allow if the lines are beginning at zero, so stuff can be
		 * commented out easily; return value if strstr must point to tmp*/
		if(tmp == strstr(tmp,"callsign=")) {
			while (isalnum(tmp[i] = toupper(tmp[9+i]))) {
				i++;
			}
			tmp[i]='\0';
			if (strlen(tmp) < 8) {				/* empty call allowed */
				strcpy(mycall,tmp);
				printw("  line  %2d: callsign: >%s<\n", line, mycall);
			}
			else {
				printw("  line  %2d: callsign: >%s< too long. "
								"Using default >%s<.\n", line, tmp, mycall);
			}
		}
		else if (tmp == strstr(tmp,"initialspeed=")) {
			while (isdigit(tmp[i] = tmp[13+i])) {
				i++;
			}
			tmp[i]='\0';
			i = atoi(tmp);
			if (i > 9) {
				initialspeed = speed = i;
				printw("  line  %2d: initial speed: %d\n", line, initialspeed);
			}
			else {
				printw("  line  %2d: initial speed: %d invalid (range: 10..oo)."
								" Using default %d.\n",line,  i, initialspeed);
			}
		}
		else if (tmp == strstr(tmp,"mincharspeed=")) {
			while (isdigit(tmp[i] = tmp[13+i])) {
				i++;
			}
			tmp[i]='\0';
			if ((i = atoi(tmp)) > 0) {
				mincharspeed = i;
				printw("  line  %2d: min.char.speed: %d\n", line, mincharspeed);
			} /* else ignore */
		}
		else if (tmp == strstr(tmp,"dspdevice=")) {
			while (isgraph(tmp[i] = tmp[10+i])) {
				i++;
			}
			tmp[i]='\0';
			if (strlen(tmp) > 1) {
				strcpy(dspdevice,tmp);
				printw("  line  %2d: dspdevice: >%s<\n", line, dspdevice);
			}
			else {
				printw("  line  %2d: dspdevice: >%s< invalid. "
								"Using default >%s<.\n", line, tmp, dspdevice);
			}
		}
		else if (tmp == strstr(tmp, "risetime=")) {
			while (isdigit(tmp[i] = tmp[9+i]) || ((tmp[i] = tmp[9+i])) == '.') {
				i++;	
			}
			tmp[i]='\0';
			edge = atof(tmp);
			printw("  line  %2d: risetime: %f\n", line, edge);
		}
		else if (tmp == strstr(tmp, "waveform=")) {
			if (isdigit(tmp[i] = tmp[9+i])) {	/* read 1 char only */
				tmp[++i]='\0';
				waveform = atoi(tmp);
			}
			if ((waveform <= 3) && (waveform > 0)) {
				printw("  line  %2d: waveform: %d\n", line, waveform);
			}
			else {
				printw("  line  %2d: waveform: %d invalid. Using default.\n",
						 line, waveform);
				waveform = SINE;
			}
		}
		else if (tmp == strstr(tmp, "constanttone=")) {
			while (isdigit(tmp[i] = tmp[13+i])) {
				i++;    
			}
			tmp[i]='\0';
			k = 0; 
			k = atoi(tmp); 							/* constanttone */
			if ( (k*k) > 1) {
				printw("  line  %2d: constanttone: %s invalid. "
							"Using default %d.\n", line, tmp, constanttone);
			}
			else {
				constanttone = k ;
				printw("  line  %2d: constanttone: %d\n", line, constanttone);
			}
        }
        else if (tmp == strstr(tmp, "ctonefreq=")) {
			while (isdigit(tmp[i] = tmp[10+i])) {
            	i++;    
			}
			tmp[i]='\0';
			k = 0; 
			k = atoi(tmp);							/* ctonefreq */
			if ( (k > 1600) || (k < 100) ) {
				printw("  line  %2d: ctonefreq: %s invalid. "
					"Using default %d.\n", line, tmp, ctonefreq);
			}
			else {
				ctonefreq = k ;
				printw("  line  %2d: ctonefreq: %d\n", line, ctonefreq);
			}
		}
		else if (tmp == strstr(tmp, "f6=")) {
			f6=0;
			if (tmp[3] == '1') {
				f6 = 1;
			}
			printw("  line  %2d: unlimited f6: %s\n", line, (f6 ? "yes":"no"));
        }
		else if (tmp == strstr(tmp, "fixspeed=")) {
			fixspeed=0;
			if (tmp[9] == '1') {
				fixspeed = 1;
			}
			printw("  line  %2d: fixed speed:  %s\n", line, (fixspeed ? "yes":"no"));
        }
		else if (tmp == strstr(tmp, "unlimitedattempt=")) {
			unlimitedattempt=0;
			if (tmp[17] == '1') {
				unlimitedattempt= 1;
			}
			printw("  line  %2d: unlim. att.:  %s\n", line, (unlimitedattempt ? "yes":"no"));
        }
		else if (tmp == strstr(tmp,"callbase=")) {
			while (isgraph(tmp[i] = tmp[9+i])) {
				i++;
			}
			tmp[i]='\0';
			if (strlen(tmp) > 1) {
				strcpy(cbfilename,tmp);
				printw("  line  %2d: callbase:  >%s<\n", line, cbfilename);
			}
			else {
				printw("  line  %2d: callbase:  >%s< invalid. "
								"Using default >%s<.\n", line, tmp, cbfilename);
			}
		}
		else if (tmp == strstr(tmp,"samplerate=")) {
			while (isdigit(tmp[i] = tmp[11+i])) {
				i++;
			}
			tmp[i]='\0';
			samplerate = atoi(tmp);
			printw("  line  %2d: sample rate: %d\n", line, samplerate);
		}
	}

	printw("Finished reading qrqrc.\n");
	return 0;
}


static void *morse(void *arg) { 
	char * text = arg;
	int i,j;
	int c, fulldotlen, dotlen, dashlen, charspeed, farnsworth, fwdotlen;
	const char *code;

#if WIN32 /* WinMM simple support by Lukasz Komsta, SP8QED */
	HWAVEOUT		h;
	WAVEFORMATEX	wf;
	WAVEHDR			wh;
	HANDLE			d;

	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = 1;
	wf.wBitsPerSample = 16;
	wf.nSamplesPerSec = samplerate * 2;
	wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
	wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
	wf.cbSize = 0;
	d = CreateEvent(0, FALSE, FALSE, 0);
	if(waveOutOpen(&h, 0, &wf, (DWORD) d, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR);

#else
	/* opening the DSP device */
	dsp_fd = open_dsp(dspdevice);
#endif
	/* set bufpos to 0 */

	full_bufpos = 0; 

	/* Some silence; otherwise the call starts right after pressing enter */
	tonegen(0, samplerate/4, SILENCE);

	/* Farnsworth? */
	if (speed < mincharspeed) {
			charspeed = mincharspeed;
			farnsworth = 1;
			fwdotlen = (int) (samplerate * 6/speed);
	}
	else {
		charspeed = speed;
		farnsworth = 0;
	}

	/* speed is in LpM now, so we have to calculate the dot-length in
	 * milliseconds using the well-known formula  dotlength= 60/(wpm*50) 
	 * and then to samples */

	dotlen = (int) (samplerate * 6/charspeed);
	fulldotlen = dotlen;
	dashlen = 3*dotlen;

	/* edge = length of rise/fall time in ms. ed = in samples */

	ed = (int) (samplerate * (edge/1000.0));

	/* the signal needs "ed" samples to reach the full amplitude and
	 * at the end another "ed" samples to reach zero. The dots and
	 * dashes therefore are becoming longer by "ed" and the pauses
	 * after them are shortened accordingly by "ed" samples */

	for (i = 0; i < strlen(text); i++) {
		c = text[i];
		if (isalpha(c)) {
			code = codetable[c-65];
		}
		else if (isdigit(c)) {
			code = codetable[c-22];
		}
		else if (c == '/') { 
			code = "-..-.";
		}
		else if (c == '+') {
			code = ".-.-.";
		}
		else {						/* not supposed to happen! */
			code = "..--..";
		}
		
		/* code is now available as string with - and . */

		for (j = 0; j < strlen(code) ; j++) {
			c = code[j];
			if (c == '.') {
				tonegen(freq, dotlen + ed, waveform);
				tonegen(0, fulldotlen - ed, SILENCE);
			}
			else {
				tonegen(freq, dashlen + ed, waveform);
				tonegen(0, fulldotlen - ed, SILENCE);
			}
		}
		if (farnsworth) {
			tonegen(0, 3*fwdotlen - fulldotlen, SILENCE);
		}
		else {
			tonegen(0, 2*fulldotlen, SILENCE);
		}
	}


#if !defined(PA) && !defined(CA)
	add_to_buf(buffer, 88200);
#endif

#if WIN32
	wh.lpData = (char*) &full_buf[0];
	wh.dwBufferLength = full_bufpos - 2;
	wh.dwFlags = 0;
	wh.dwLoops = 0;
	waveOutPrepareHeader(h, &wh, sizeof(wh));
	ResetEvent(d);
	waveOutWrite(h, &wh, sizeof(wh));
	if(WaitForSingleObject(d, INFINITE) != WAIT_OBJECT_0);
	waveOutUnprepareHeader(h, &wh, sizeof(wh));
	waveOutClose(h);
	CloseHandle(d);
#else
	write_audio(dsp_fd, &full_buf[0], full_bufpos);
	close_audio(dsp_fd);
#endif
	sending_complete = 1;
	return NULL;
}

static int add_to_buf(void* data, int size)
{
	memcpy(&full_buf[full_bufpos / sizeof(int)], data, size);
	full_bufpos += size;
	return 0;
}	

/* tonegen generates a sinus tone of frequency 'freq' and length 'len' (samples)
 * based on 'samplerate', 'edge' (rise/falltime) */

static int tonegen (int freq, int len, int waveform) {
	int x=0;
	int out;
	double val=0;

	for (x=0; x < len-1; x++) {
		switch (waveform) {
			case SINE:
				val = sin(2*PI*freq*x/samplerate);
				break;
			case SAWTOOTH:
				val=((1.0*freq*x/samplerate)-floor(1.0*freq*x/samplerate))-0.5;
				break;
			case SQUARE:
				val = ceil(sin(2*PI*freq*x/samplerate))-0.5;
				break;
			case SILENCE:
				val = 0;
		}


		if (x < ed) { val *= pow(sin(PI*x/(2.0*ed)),2); }	/* rising edge */

		if (x > (len-ed)) {								/* falling edge */
				val *= pow(sin(2*PI*(x-(len-ed)+ed)/(4*ed)),2); 
		}
		
		out = (int) (val * 32500.0);
#ifndef PA
		out = out + (out<<16);	/* stereo only for OSS & CoreAudio*/
#endif
		add_to_buf(&out, sizeof(out));
	}
	return 0;
}

/* Save config file
 *
 * Tries to keep the old format (including comments, etc.) and adds
 * config options that were not used yet in the file to the end 
 * */

static int save_config () {
	FILE *fh;
	char tmp[80]="";
	char confopts[12][80] = {
		"\ncallsign=", 
		"\ncallbase=",
		"\ndspdevice=", 
		"\ninitialspeed=", 
		"\nmincharspeed=",
		"\nwaveform=", 
		"\nconstanttone=",
		"\nctonefreq=", 
		"\nfixspeed=", 
		"\nunlimitedattempt=", 
		"\nf6=", 
		"\nrisetime=" 
	};
	char *conf1;
	char *conf2;
	char *find, *findend;
	int i, len, conf1len, conf2len;
	int j;

	conf2 = malloc(1);

	if ((fh = fopen(rcfilename, "rb")) == NULL) {
		endwin();
		fprintf(stderr, "Unable to open config file '%s'!\n", rcfilename);
		exit(EXIT_FAILURE);
	}

	fseek(fh, 0, SEEK_END);
	j = (int) ftell(fh);
	conf1 = malloc((size_t) j+1);

	rewind(fh);
	i = fread(conf1, sizeof(char), (size_t) j, fh);
	conf1[j] = '\0';
	conf1len = j;

	fclose(fh);

	/* The whole config file is now in conf1 
	 *
	 * For each config option, search&replace it with the current value.
	 * Only accept key=value pairs if the key starts on pos 0 of the line
	 * */

	//endwin();
	for (i = 0; i < 12; i++) {
		/* assemble new string for this conf option*/
		switch (i) {
			case 0:
				sprintf(tmp, "%s%s ", confopts[i], mycall);
				break;
			case 1:
				sprintf(tmp, "%s%s ", confopts[i], cbfilename);
				break;
			case 2:
				sprintf(tmp, "%s%s ", confopts[i], dspdevice);
				break;
			case 3:
				sprintf(tmp, "%s%d ", confopts[i], initialspeed);
				break;
			case 4:
				sprintf(tmp, "%s%d ", confopts[i], mincharspeed);
				break;
			case 5:
				sprintf(tmp, "%s%d ", confopts[i], waveform);
				break;
			case 6:
				sprintf(tmp, "%s%d ", confopts[i], constanttone);
				break;
			case 7:
				sprintf(tmp, "%s%d ", confopts[i], ctonefreq);
				break;
			case 8:
				sprintf(tmp, "%s%d ", confopts[i], fixspeed);
				break;
			case 9:
				sprintf(tmp, "%s%d ", confopts[i], unlimitedattempt);
				break;
			case 10:
				sprintf(tmp, "%s%d ", confopts[i], f6);
				break;
			case 11:
				sprintf(tmp, "%s%f ", confopts[i], edge);
				break;
		}	

		/* Conf option already in rc-file? */
		if ((find = strstr(conf1, confopts[i])) != NULL) {
			/* determine length. */
			findend = find;
			findend++;	/* starts with \n, always skip it */
			while (!isspace(*findend++));
			len = findend - find;
			
			/* old size of conf1: conf1len (see above) 
			 * new size: conf1len - len + strlen(tmp)*/
			conf2len = conf1len - len + strlen(tmp);
			conf2 = realloc(conf2, (size_t) conf2len);
			memcpy(conf2, conf1, (find - conf1));
			memcpy(conf2 + (find - conf1), tmp, strlen(tmp));
			memcpy(conf2 + (find - conf1) + strlen(tmp), findend, 
					(size_t) conf2len - (find - conf1) - strlen(tmp));
		}
		/* otherwise, add to the end */
		else {
			/* CR LF or LF only? */
			if (strstr(conf1, "\r")) {
				strcat(tmp, "\r");
			}
			strcat(tmp, "\n");
			conf2len = conf1len + strlen(tmp) - 1;
			conf2 = realloc(conf2, conf2len);
			memcpy(conf2, conf1, conf1len-1);  // excl. \0
			memcpy(conf2 + conf1len - 1, tmp, strlen(tmp)); // (incl. \0)
		}
		conf1 = realloc(conf1, (size_t) conf2len);
		conf1len = conf2len;
		if (conf1 == NULL) {
			exit(0);
		}
		memcpy(conf1, conf2, conf1len);
	}

	if ((fh = fopen(rcfilename, "wb")) == NULL) {
		endwin();
		fprintf(stderr, "Unable to open config file '%s'!\n", rcfilename);
		exit(EXIT_FAILURE);
	}

	fwrite(conf1, conf1len, sizeof(char), fh); 
	fclose(fh);

	free(conf1);
	free(conf2);

	return 0;
}
		
static void thread_fail (int j) {
	if (j) {
		endwin();
		perror("Error: Unable to create cwthread!\n");
		exit(EXIT_FAILURE);
	}
}

/* Add timestamps to toplist file if not there yet */
static int check_toplist () {
	char line[80]="";
	char tmp[80]="";
	FILE *fh;
	FILE *fh2;

	if ((fh = fopen(tlfilename, "r+")) == NULL) {
		endwin();
		perror("Unable to open toplist file 'toplist'!\n");
		exit(EXIT_FAILURE);
	}

	fgets(tmp, 35, fh);
	
	rewind(fh);
	
	if (strlen(tmp) == 21) {
			printw("Toplist file in old format. Converting...");
			strcpy(tmp, "cp -f ");
			strcat(tmp, tlfilename);
			strcat(tmp, " /tmp/qrq-toplist");
			if (system(tmp)) {
					printw("Failed to copy to /tmp/qrq-toplist\n");
					getch();
					endwin();
					exit(EXIT_FAILURE);
			}

			fh2 = fopen("/tmp/qrq-toplist", "r+"); 		/* should work ... */

			while ((feof(fh2) == 0) && (fgets(line, 35, fh2) != NULL)) {
					line[20]=' ';
					strcpy(tmp, line);
					strcat(tmp, "1181234567\n");
					fputs(tmp, fh);
			}
			
			printw(" done!\n");
	
			fclose(fh2);

	}

	fclose(fh);

	return 0;
}



/* See where our files are. We need 'callbase.qcb', 'qrqrc' and 'toplist'.
 * The can be: 
 * 1) In the current directory -> use them
 * 2) In ~/.qrq/  -> use toplist and qrqrc from there and callbase from
 *    DESTDIR/share/qrq/
 * 3) in DESTDIR/share/qrq/ -> create ~/.qrq/ and copy qrqrc and toplist
 *    there.
 * 4) Nowhere --> Exit.*/
static int find_files () {
	
	FILE *fh;
	const char *homedir = NULL;
	char tmp_rcfilename[1024] = "";
	char tmp_tlfilename[1024] = "";
	char tmp_cbfilename[1024] = "";

	printw("\nChecking for necessary files (qrqrc, toplist, callbase)...\n");
	
	if (((fh = fopen("qrqrc", "r")) == NULL) ||
		((fh = fopen("toplist", "r")) == NULL) ||
		((fh = fopen("callbase.qcb", "r")) == NULL)) {
		
		if ((homedir = getenv("HOME")) != NULL) {
		printw("... not found in current directory. Checking "
						"%s/.qrq/...\n", homedir);
		refresh();
		strcat(rcfilename, homedir);
		}
		else {
		printw("... not found in current directory. Checking "
						"./.qrq/...\n", homedir);
		refresh();
		strcat(rcfilename, ".");
		}
				
		strcat(rcfilename, "/.qrq/qrqrc");
	
		/* check if there is ~/.qrq/qrqrc. If it's there, it's safe to assume
		 * that toplist also exists at the same place and callbase exists in
		 * DESTDIR/share/qrq/. */

		if ((fh = fopen(rcfilename, "r")) == NULL ) {
			printw("... not found in %s/.qrq/. Checking %s/share/qrq..."
							"\n", homedir, destdir);
			/* check for the files in DESTDIR/share/qrq/. if exists, copy 
			 * qrqrc and toplist to ~/.qrq/  */

			strcpy(tmp_rcfilename, destdir);
			strcat(tmp_rcfilename, "/share/qrq/qrqrc");
			strcpy(tmp_tlfilename, destdir);
			strcat(tmp_tlfilename, "/share/qrq/toplist");
			strcpy(tmp_cbfilename, destdir);
			strcat(tmp_cbfilename, "/share/qrq/callbase.qcb");

			if (((fh = fopen(tmp_rcfilename, "r")) == NULL) ||
				((fh = fopen(tmp_tlfilename, "r")) == NULL) ||
				 ((fh = fopen(tmp_cbfilename, "r")) == NULL)) {
				printw("Sorry: Couldn't find 'qrqrc', 'toplist' and"
			   			" 'callbase.qcb' anywhere. Exit.\n");
				getch();
				endwin();
				exit(EXIT_FAILURE);
			}
			else {			/* finally found it in DESTDIR/share/qrq/ ! */
				/* abusing rcfilename here for something else temporarily */
				printw("Found files in %s/share/qrq/."
						"\nCreating directory %s/.qrq/ and copy qrqrc and"
						" toplist there.\n", destdir, homedir);
				strcpy(rcfilename, homedir);
				strcat(rcfilename, "/.qrq/");
#ifdef WIN32
				j = mkdir(rcfilename);
#else
				j = mkdir(rcfilename,  0777);
#endif
				if (j && (errno != EEXIST)) {
					printw("Failed to create %s! Exit.\n", rcfilename);
					getch();
					endwin();
					exit(EXIT_FAILURE);
				}
				/* OK, now we created the directory, we can read in
				 * DESTDIR/local/, so I assume copying files won't cause any
				 * problem, with system()... */

				strcpy(rcfilename, "install -m 644 ");
				strcat(rcfilename, tmp_tlfilename);
				strcat(rcfilename, " ");
				strcat(rcfilename, homedir);
				strcat(rcfilename, "/.qrq/ 2> /dev/null");
				if (system(rcfilename)) {
					printw("Failed to copy toplist file: %s\n", rcfilename);
					getch();
					endwin();
					exit(EXIT_FAILURE);
				}
				strcpy(rcfilename, "install -m 644 ");
				strcat(rcfilename, tmp_rcfilename);
				strcat(rcfilename, " ");
				strcat(rcfilename, homedir);
				strcat(rcfilename, "/.qrq/ 2> /dev/null");
				if (system(rcfilename)) {
					printw("Failed to copy qrqrc file: %s\n", rcfilename);
					getch();
					endwin();
					exit(EXIT_FAILURE);
				}
				printw("Files copied. You might want to edit "
						"qrqrc according to your needs.\n", homedir);
				strcpy(rcfilename, homedir);
				strcat(rcfilename, "/.qrq/qrqrc");
				strcpy(tlfilename, homedir);
				strcat(tlfilename, "/.qrq/toplist");
				strcpy(cbfilename, tmp_cbfilename);
			} /* found in DESTDIR/share/qrq/ */
		}
		else {
			printw("... found files in %s/.qrq/.\n", homedir);
			strcat(tlfilename, homedir);
			strcat(tlfilename, "/.qrq/toplist");
			strcpy(cbfilename, destdir);
			strcat(cbfilename, "/share/qrq/callbase.qcb");
		}
	}
	else {
		printw("... found in current directory.\n");
		strcpy(rcfilename, "qrqrc");
		strcpy(tlfilename, "toplist");
		strcpy(cbfilename, "callbase.qcb");
	}
	refresh();
	fclose(fh);
	return 0;
}


static int statistics () {
		char line[80]="";

		int time = 0;
		int score = 0;
		int count= 0;

		FILE *fh;
		FILE *fh2;
		
		if ((fh = fopen(tlfilename, "r")) == NULL) {
				fprintf(stderr, "Unable to open toplist.");
				exit(0);
		}
		
		if ((fh2 = fopen("/tmp/qrq-plot", "w+")) == NULL) {
				fprintf(stderr, "Unable to open /tmp/qrq-plot.");
				exit(0);
		}

		fprintf(fh2, "set yrange [0:]\nset xlabel \"Date/Time\"\n"
					"set title \"QRQ scores for %s. Press 'q' to "
					"close this window.\"\n"
					"set ylabel \"Score\"\nset xdata time\nset "
					" timefmt \"%%s\"\n "
					"plot \"-\" using 1:2 title \"\"\n", mycall);

		while ((feof(fh) == 0) && (fgets(line, 80, fh) != NULL)) {
				if ((strstr(line, mycall) != NULL)) {
					count++;
					sscanf(line, "%*s %d %*d %d", &score, &time);
					fprintf(fh2, "%d %d\n", time, score);
				}
		}

		if (!count) {
			fprintf(fh2, "0 0\n");
		}
		
		fprintf(fh2, "end\npause 10000");

		fclose(fh);
		fclose(fh2);

		system("gnuplot /tmp/qrq-plot 2> /dev/null &");
	return 0;
}


int read_callbase () {
	FILE *fh;
	int c,i;
	int maxlen=0;
	char tmp[80] = "";
	int nr=0;

	if ((fh = fopen(cbfilename, "r")) == NULL) {
		endwin();
		fprintf(stderr, "Error: Couldn't read callsign database ('%s')!\n",
						cbfilename);
		exit(EXIT_FAILURE);
	}

	/* count the lines/calls and lengths */
	i=0;
	while ((c = getc(fh)) != EOF) {
		i++;
		if (c == '\n') {
			nr++;
			maxlen = (i > maxlen) ? i : maxlen;
			i = 0;
		}
	}
	maxlen++;

	if (!nr) {
		endwin();
		printf("\nError: Callsign database empty, no calls read. Exiting.\n");
		exit(EXIT_FAILURE);
	}

	/* allocate memory for calls array, free if needed */

	free(calls);

	if ((calls = (char **) malloc( (size_t) sizeof(char *)*nr )) == NULL) {
		fprintf(stderr, "Error: Couldn't allocate %d bytes!\n", 
						(int) sizeof(char)*nr);
		exit(EXIT_FAILURE);
	}
	
	/* Allocate each element of the array with size maxlen */
	for (c=0; c < nr; c++) {
		if ((calls[c] = (char *) malloc (maxlen * sizeof(char))) == NULL) {
			fprintf(stderr, "Error: Couldn't allocate %d bytes!\n", maxlen);
			exit(EXIT_FAILURE);
		}
	}

	rewind(fh);
	
	nr=0;
	while (fgets(tmp,maxlen,fh) != NULL) {
		for (i = 0; i < strlen(tmp); i++) {
				tmp[i] = toupper(tmp[i]);
		}
		tmp[i-1]='\0';				/* remove newline */
		if (tmp[i-2] == '\r') {		/* also for DOS files */
			tmp[i-2] = '\0';
		}
		strcpy(calls[nr],tmp);
		nr++;
		if (nr == c) 			/* may happen if call file corrupted */
				break;
	}
	fclose(fh);


	return nr;

}

void find_callbases () {
	DIR *dir;
	struct dirent *dp;
	char tmp[PATH_MAX];
	char path[3][PATH_MAX];
	int i=0,j=0,k=0;

#ifndef WIN32
		strcpy(path[0], getenv("PWD"));
		strcat(path[0], "/");
		strcpy(path[1], getenv("HOME"));
		strcat(path[1], "/.qrq/");
		strcpy(path[2], destdir);
		strcat(path[2], "/share/qrq/");
#else
		strcpy(path[0], "./");
		strcpy(path[1], getenv("APPDATA"));
		strcat(path[1], "/qrq/");
		strcpy(path[2], "c:\\");
#endif

	for (i=0; i < 100; i++) {
		strcpy(cblist[i], "");
	}

	/* foreach paths...  */
	for (k = 0; k < 3; k++) {

		if (!(dir = opendir(path[k]))) {
			continue;
		}
	
		while ((dp = readdir(dir))) {
			strcpy(tmp, dp->d_name);
			i = strlen(tmp);
			/* find *.qcb files ...  */
			if (i>4 && tmp[i-1] == 'b' && tmp[i-2] == 'c' && tmp[i-3] == 'q') {
				strcpy(cblist[j], path[k]);
				strcat(cblist[j], tmp);
				j++;
			}
		}
	} /* for paths */
}



void select_callbase () {
	int i = 0, j = 0, k = 0;
	int c = 0;		/* cursor position   */
	int p = 0;		/* page a 10 entries */
	char* cblist_ptr;


	curs_set(FALSE);

	/* count files */
	while (strcmp(cblist[i], "")) i++;

	if (!i) {
		mvwprintw(conf_w,10,4, "No qcb-files found!");
		wrefresh(conf_w);
#ifdef WIN32
		Sleep(1000);
#else
		sleep(1);
#endif
		return;
	}

	/* loop for key unput */
	while (1) {

	/* cls */
	for (j = 5; j < 16; j++) {
			mvwprintw(conf_w,j,2, "                                         ");
	}

	/* display 10 files, highlight cursor position */
	for (j = p*10; j < (p+1)*10; j++) {
		if (j <= i) {
				cblist_ptr = cblist[j];
				mvwprintw(conf_w,5+(j - p*10 ),2, "  %s       ", cblist_ptr);
		}
		if (c == j) {						/* cursor */
			mvwprintw(conf_w,5+(j - p*10),2, ">");
		}
	}
	
	wrefresh(conf_w);

	k = getch();

	switch ((int) k) {
		case KEY_UP:
			c = (c > 0) ? (c-1) : c;
			if (!((c+1) % 10)) {	/* scroll down */
				p = (p > 0) ? (p-1) : p;
			}
			break;
		case KEY_DOWN:
			c = (c < i-1) ? (c+1) : c;
			if (c && !(c % 10)) {	/* scroll down */
				p++;
			}
			break;
		case '\n':
			strcpy(cbfilename, cblist[c]);
			nrofcalls = read_callbase();
			return;	
			break;
	}

	wrefresh(conf_w);

	} /* while 1 */

	curs_set(TRUE);

}



void help () {
		printf("qrq v%s  (c) 2006-2013 Fabian Kurz, DJ1YFK. "
					"http://fkurz.net/ham/qrq.html\n", VERSION);
		printf("High speed morse telegraphy trainer, similar to"
					" RUFZ.\n\n");
		printf("This is free software, and you are welcome to" 
						" redistribute it\n");
		printf("under certain conditions (see COPYING).\n\n");
		printf("Start 'qrq' without any command line arguments for normal"
					" operation.\n");
		exit(0);
}


/* vim: noai:ts=4:sw=4 
*/
