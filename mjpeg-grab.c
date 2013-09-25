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
static char* jpegFilename = NULL;
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

	for (; count > 0; count--) {
		struct pollfd pfd = {fd, POLLIN, 0};
		int timeout = -1;

		int r = poll(&pfd, 1, timeout);

		if (r == -1)
			errno_exit("poll");

		if (r == 1)
			frameRead();
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
	unsigned int min;

	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
		if (EINVAL == errno) {
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

	if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect; /* reset to default */

		if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
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

	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
		errno_exit("VIDIOC_S_FMT");

	if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
		fprintf(stderr,"Libv4l didn't accept MJPEG format. Can't proceed.\n");
		exit(EXIT_FAILURE);
	}

	/* Note VIDIOC_S_FMT may change width and height. */
	if (width != fmt.fmt.pix.width) {
		width = fmt.fmt.pix.width;
		fprintf(stderr,"Image width set to %ui by device %s.\n", width, deviceName);
	}

	if (height != fmt.fmt.pix.height) {
		height = fmt.fmt.pix.height;
		fprintf(stderr,"Image height set to %ui by device %s.\n", height, deviceName);
	}
	
	CLEAR(frameint);
	
	/* Attempt to set the frame interval. */
	frameint.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	frameint.parm.capture.timeperframe.numerator = 1;
	frameint.parm.capture.timeperframe.denominator = fps;
	if (-1 == xioctl(fd, VIDIOC_S_PARM, &frameint))
		fprintf(stderr,"Unable to set frame interval.\n");

	/* Buggy driver paranoia. */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;

	readInit(fmt.fmt.pix.sizeimage);
}

static void deviceClose(void)
{
	if (-1 == v4l2_close(fd))
		errno_exit("close");

	fd = -1;
}

static void deviceOpen(void)
{
	struct stat st;

	// stat file
	if (-1 == stat(deviceName, &st)) {
		fprintf(stderr, "Cannot identify '%s': %d, %s\n", deviceName, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	// check if its device
	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is no device\n", deviceName);
		exit(EXIT_FAILURE);
	}

	// open device
	fd = v4l2_open(deviceName, O_RDWR /* required */ | O_NONBLOCK, 0);

	// check if opening was successfull
	if (-1 == fd) {
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
		"-o | --output        Set JPEG output filename\n"
		"-W | --width         Set image width\n"
		"-H | --height        Set image height\n"
		"-I | --interval      Set frame interval (fps)\n"
		"-v | --version       Print version\n"
		"-c | --count         Number of jpeg's to capture\n"
		"",
		name);
	}

static const char short_options [] = "d:ho:W:H:I:vc:";

static const struct option
long_options [] = {
	{ "device",   required_argument, NULL, 'd' },
	{ "help",     no_argument,       NULL, 'h' },
	{ "output",   required_argument, NULL, 'o' },
	{ "width",    required_argument, NULL, 'W' },
	{ "height",   required_argument, NULL, 'H' },
	{ "interval", required_argument, NULL, 'I' },
	{ "version",	no_argument,		   NULL, 'v' },
	{ "count",    required_argument, NULL, 'c' },
	{ 0, 0, 0, 0 }
};

int main(int argc, char **argv)
{

	for (;;) {
		int index, c = 0;

		c = getopt_long(argc, argv, short_options, long_options, &index);

		if (-1 == c)
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

			case 'W':
				// set width
				width = atoi(optarg);
				break;

			case 'H':
				// set height
				height = atoi(optarg);
				break;
				
			case 'I':
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

	// check for need parameters
	if (!jpegFilename) {
		fprintf(stderr, "You have to specify JPEG output filename!\n\n");
		usage(stdout, argv[0]);
		exit(EXIT_FAILURE);
	}

	// open and initialize device
	deviceOpen();
	deviceInit();

	// process frames
	mainLoop();

	// close device
	deviceUninit();
	deviceClose();

	exit(EXIT_SUCCESS);

	return 0;
}
