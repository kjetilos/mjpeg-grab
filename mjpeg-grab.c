/**
 * Based on v4l2grab by Tobias Müller
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <libv4l2.h>

#define CLEAR(x) memset (&(x), 0, sizeof (x))
#define VERSION "3.0"

struct buffer {
  void * start;
  size_t length;
};

static int fd = -1;
struct buffer * buffers = NULL;

// global settings
static unsigned int width = 1280;
static unsigned int height = 720;
static unsigned int fps = 30;
static char* jpegFilename = "output.jpg";
static char* deviceName = "/dev/video0";
static unsigned int frame_count = 1;

/**
 * Print error message and terminate programm with EXIT_FAILURE return code.
 *
 * \param s error message to print
 */
static void errno_exit(const char* s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

/**
 *	Do ioctl and retry if error was EINTR ("A signal was caught during the ioctl() operation."). Parameters are the same as on ioctl.
 *
 *	\param fd file descriptor
 *	\param request request
 *	\param argp argument
 *	\returns result from ioctl
*/
static int xioctl(int fd, int request, void* argp)
{
	int r;

	do r = v4l2_ioctl(fd, request, argp);
	while (-1 == r && EINTR == errno);

	return r;
}

static void rawWrite(const unsigned char* img, size_t length)
{
	FILE *outfile = fopen( jpegFilename, "ab" );
	if (!outfile)
	{
		errno_exit("raw write");
	}

	fwrite(img, 1, length, outfile);
	fclose(outfile);
}

/**
 * process image read
 */
static void imageProcess(const void* p, size_t length)
{
	rawWrite(p, length);
}

/**
 * read single frame
 */
static int frameRead(void)
{
	ssize_t n = v4l2_read(fd, buffers[0].start, buffers[0].length);

	if (n == -1) {
		switch (errno) {
			case EAGAIN:
				return 0;

			case EIO:
				// Could ignore EIO, see spec.
				// fall through

			default:
				errno_exit("read");
		}
	}

	imageProcess(buffers[0].start, n);

	return 1;
}

/**
 * Read frames and process them
 */
static void mainLoop(void)
{	
	unsigned int count = frame_count;

	while (count > 0) {
		struct pollfd pfd = {fd, POLLIN, 0};
		int timeout = -1;

		int r = poll(&pfd, 1, timeout);

		if (r == -1)
			errno_exit("poll");

		if (frameRead())
			count--;
	}
}

static void deviceUninit(void)
{
	free(buffers[0].start);
	free(buffers);
}

static void readInit(unsigned int buffer_size)
{
	buffers = calloc(1, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	buffers[0].length = buffer_size;
	buffers[0].start = malloc(buffer_size);

	if (!buffers[0].start) {
		fprintf (stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}
}

static void deviceInit(void)
{
	struct v4l2_capability cap;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	struct v4l2_format fmt;
	struct v4l2_streamparm frameint;

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
		if (errno == EINVAL) {
			fprintf(stderr, "%s is no V4L2 device\n",deviceName);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_QUERYCAP");
		}
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		fprintf(stderr, "%s is no video capture device\n",deviceName);
		exit(EXIT_FAILURE);
	}

	if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
		fprintf(stderr, "%s does not support read i/o\n",deviceName);
		exit(EXIT_FAILURE);
	}

	/* Select video input, video standard and tune here. */
	CLEAR(cropcap);

	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (xioctl(fd, VIDIOC_CROPCAP, &cropcap) == 0) {
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect; /* reset to default */

		if (xioctl(fd, VIDIOC_S_CROP, &crop) == -1) {
			switch (errno) {
				case EINVAL:
					/* Cropping not supported. */
					break;
				default:
					/* Errors ignored. */
					break;
			}
		}
	} else {
		/* Errors ignored. */
	}

	CLEAR(fmt);

	// v4l2_format
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;

	if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1)
		errno_exit("VIDIOC_S_FMT");

	if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
		fprintf(stderr,"Libv4l didn't accept MJPEG format. Can't proceed.\n");
		exit(EXIT_FAILURE);
	}
	
	CLEAR(frameint);
	
	/* Attempt to set the frame interval. */
	frameint.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	frameint.parm.capture.timeperframe.numerator = 1;
	frameint.parm.capture.timeperframe.denominator = fps;
	if (xioctl(fd, VIDIOC_S_PARM, &frameint) == -1)
		fprintf(stderr,"Unable to set frame interval.\n");

	readInit(fmt.fmt.pix.sizeimage);
}

static void deviceClose(void)
{
	if (v4l2_close(fd) == -1)
		errno_exit("close");

	fd = -1;
}

static void deviceOpen(void)
{
	// open device
	fd = v4l2_open(deviceName, O_RDWR | O_NONBLOCK, 0);

	// check if opening was successfull
	if (fd == -1) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n", deviceName, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

static void usage(FILE* fp, const char* name)
{
	fprintf(fp,
		"Usage: %s [options]\n\n"
		"Options:\n"
		"-d | --device name   Video device name [/dev/video0]\n"
		"-h | --help          Print this message\n"
		"-o | --output        Set JPEG output filename [output.jpg]\n"
		"-r | --resolution    Set resolution i.e 1280x720\n"
		"-i | --interval      Set frame interval (fps)\n"
		"-v | --version       Print version\n"
		"-c | --count         Number of jpeg's to capture [1]\n"
		"",
		name);
}

static const char short_options [] = "d:ho:r:i:vc:";

static const struct option
long_options [] = {
	{ "device",     required_argument, NULL, 'd' },
	{ "help",       no_argument,       NULL, 'h' },
	{ "output",     required_argument, NULL, 'o' },
	{ "resolution", required_argument, NULL, 'r' },
	{ "interval",   required_argument, NULL, 'I' },
	{ "version",	  no_argument,		   NULL, 'v' },
	{ "count",      required_argument, NULL, 'c' },
	{ 0, 0, 0, 0 }
};

int main(int argc, char **argv)
{

	for (;;) {
		int index, c = 0;

		c = getopt_long(argc, argv, short_options, long_options, &index);

		if (c == -1)
			break;

		switch (c) {
			case 0: /* getopt_long() flag */
				break;

			case 'd':
				deviceName = optarg;
				break;

			case 'h':
				// print help
				usage(stdout, argv[0]);
				exit(EXIT_SUCCESS);

			case 'o':
				// set jpeg filename
				jpegFilename = optarg;
				break;

			case 'r':
				if (sscanf(optarg, "%ux%u", &width, &height) != 2) {
					fprintf(stderr, "Illegal resolution argument\n");
					usage(stdout, argv[0]);
					exit(EXIT_FAILURE);
				}
				break;

			case 'i':
				// set fps
				fps = atoi(optarg);
				break;

			case 'v':
				printf("Version: %s\n", VERSION);
				exit(EXIT_SUCCESS);
				break;

			case 'c':
				frame_count = atoi(optarg);
				break;

			default:
				usage(stderr, argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	// open and initialize device
	deviceOpen();
	deviceInit();

	// process frames
	mainLoop();

	// close device
	deviceUninit();
	deviceClose();

	return 0;
}
