#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include "liburing.h"

#define QD	64
#define BS	(32*1024)

#define GET_LOWER_32BITS(v)  ((v) & 0xFFFFFFFF)

static int infd, outfd;

char *infd_path = NULL, *outfd_path = NULL;

int speed_limitation = 0;

char *iuc_program_name = NULL;

bool check_io_uring = false;

char const iuc_short_opts[] = "I:O:S:ah";

const struct option iuc_long_opts[] = {
    {"input",     1,  NULL,  'I'},
    {"output",    1,  NULL,  'O'},
    {"speed",     1,  NULL,  'S'},
    {"available", 0,  NULL,  'a'},
    {"help",      0,  NULL,  'h'},
    {0,  0,  0,  0}
};

void try_help() {
    fprintf(stdout, "Try  '%s --help' for more information.\n", iuc_program_name);
    exit(1);
}

void help() {
    static char const *const help_msg[] = {
        "Copy file with io_uring",
        "",
        " -I,   --input      set the path of input file",
        " -O,   --output     set the path of output file",
        " -S,   --speed      set the speed limitaion (MB/s)",
        " -a,   --available  check the environment for io_uring",
        " -h,   --help       give this help message",
        0
    };

    char const *const *p = help_msg;

    while (*p) {
        fprintf(stdout, "%s\n", *p++);
    }
}

char* base_name(char *fname) {
    char *p;
    
    if ((p = strrchr(fname, '/')) != NULL) {
        fname = p + 1;
    }

    return fname;
}

struct io_data {
	int read;
	off_t first_offset, offset;
	size_t first_len;
	struct iovec iov;
};

static int setup_context(unsigned entries, struct io_uring *ring)
{
	int ret;

	ret = io_uring_queue_init(entries, ring, 0);
	if (ret < 0) {
		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
		return -1;
	}

	return 0;
}

static int get_file_size(int fd, off_t *size)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return -1;
	if (S_ISREG(st.st_mode)) {
		*size = st.st_size;
		return 0;
	} else if (S_ISBLK(st.st_mode)) {
		unsigned long long bytes;

		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
			return -1;

		*size = bytes;
		return 0;
	}

	return -1;
}

static void queue_prepped(struct io_uring *ring, struct io_data *data)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	assert(sqe);

	if (data->read)
		io_uring_prep_readv(sqe, infd, &data->iov, 1, data->offset);
	else
		io_uring_prep_writev(sqe, outfd, &data->iov, 1, data->offset);

	io_uring_sqe_set_data(sqe, data);
}

static int queue_read(struct io_uring *ring, off_t size, off_t offset)
{
	struct io_uring_sqe *sqe;
	struct io_data *data;

	data = static_cast<io_data*>(malloc(size + sizeof(*data)));
	if (!data)
		return 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		free(data);
		return 1;
	}

	data->read = 1;
	data->offset = data->first_offset = offset;

	data->iov.iov_base = data + 1;
	data->iov.iov_len = size;
	data->first_len = size;

	io_uring_prep_readv(sqe, infd, &data->iov, 1, offset);
	io_uring_sqe_set_data(sqe, data);
	return 0;
}

static void queue_write(struct io_uring *ring, struct io_data *data)
{
	data->read = 0;
	data->offset = data->first_offset;

	data->iov.iov_base = data + 1;
	data->iov.iov_len = data->first_len;

	queue_prepped(ring, data);
	io_uring_submit(ring);
}

static int copy_file(struct io_uring *ring, off_t insize)
{
	unsigned long reads, writes;
	struct io_uring_cqe *cqe;
	off_t write_left, offset;
	int ret;
    off_t speed_size = 0, speed_size_limitation = speed_limitation * 1024 * 1024 / 10;
    useconds_t speed_val = 0;
    struct timeval speed_time;
    struct timeval speed_time_tmp;

	write_left = insize;
	writes = reads = offset = 0;

    if (speed_limitation > 0) {
        gettimeofday(&speed_time, NULL);
    }

	while (insize || write_left) {
		unsigned long had_reads;
		int got_comp;
        
        if(speed_limitation > 0) {
            gettimeofday(&speed_time_tmp, NULL);
            speed_val = (speed_time_tmp.tv_sec * 1000000 + speed_time_tmp.tv_usec) - (speed_time.tv_sec * 1000000 + speed_time.tv_usec);
            if(speed_val > 100000) {
                speed_time.tv_sec = speed_time_tmp.tv_sec;
                speed_time.tv_usec = speed_time_tmp.tv_usec;
                speed_size = 0;
            } else if (speed_size >= speed_size_limitation) {
                useconds_t sleep_time = 100000 - speed_val;
                usleep(sleep_time);
                gettimeofday(&speed_time, NULL);
                speed_size = 0;
            }
        }
		/*
		 * Queue up as many reads as we can
		 */
		had_reads = reads;
		while (insize) {
			off_t this_size = insize;

			if (reads + writes >= QD)
				break;
			if (this_size > BS)
				this_size = BS;
			else if (!this_size)
				break;

			if (queue_read(ring, this_size, offset))
				break;

			insize -= this_size;
			offset += this_size;
            speed_size += this_size;
            reads++;

            if(speed_limitation > 0 && speed_size >= speed_size_limitation) {
                break;
            }
		}

		if (had_reads != reads) {
			ret = io_uring_submit(ring);
			if (ret < 0) {
				fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
				break;
			}
		}

		/*
		 * Queue is full at this point. Find at least one completion.
		 */
		got_comp = 0;
		while (write_left) {
			struct io_data *data;

			if (!got_comp) {
				ret = io_uring_wait_cqe(ring, &cqe);
				got_comp = 1;
			} else {
				ret = io_uring_peek_cqe(ring, &cqe);
				if (ret == -EAGAIN) {
					cqe = NULL;
					ret = 0;
				}
			}
			if (ret < 0) {
				fprintf(stderr, "io_uring_peek_cqe: %s\n",
							strerror(-ret));
				return 1;
			}
			if (!cqe)
				break;

			data = static_cast<io_data*>(io_uring_cqe_get_data(cqe));
			if (cqe->res < 0) {
				if (cqe->res == -EAGAIN) {
					queue_prepped(ring, data);
					io_uring_cqe_seen(ring, cqe);
					continue;
				}
				fprintf(stderr, "cqe failed: %s\n",
						strerror(-cqe->res));
				return 1;
			} else if ((size_t)cqe->res != data->iov.iov_len) {
				/* Short read/write, adjust and requeue */
				data->iov.iov_base += cqe->res;
				data->iov.iov_len -= cqe->res;
				data->offset += cqe->res;
				queue_prepped(ring, data);
				io_uring_cqe_seen(ring, cqe);
				continue;
			}

			/*
			 * All done. if write, nothing else to do. if read,
			 * queue up corresponding write.
			 */
			if (data->read) {
				queue_write(ring, data);
				write_left -= data->first_len;
				reads--;
				writes++;
			} else {
				free(data);
				writes--;
			}
			io_uring_cqe_seen(ring, cqe);
		}
	}

	/* wait out pending writes */
	while (writes) {
		struct io_data *data;

		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			return 1;
		}
		if (cqe->res < 0) {
			fprintf(stderr, "write res=%d\n", cqe->res);
			return 1;
		}
		data = static_cast<io_data*>(io_uring_cqe_get_data(cqe));
		free(data);
		writes--;
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	off_t insize;
	int ret;

    iuc_program_name = base_name(argv[0]);

    while(true) {
        int optc;
        int long_idx = -1;
        char *stop = NULL;
        
        optc = getopt_long(argc, argv, iuc_short_opts, iuc_long_opts, &long_idx);

        if (optc < 0) {
            break;
        }

        switch (optc) {
            case 'I':
                infd_path = optarg;
                break;
            case 'O':
                outfd_path = optarg;
                break;
            case 'S':
                speed_limitation = GET_LOWER_32BITS(strtoul(optarg, &stop, 0));
                if (*stop != '\0' || ERANGE == errno || speed_limitation <= 0) {
                    printf("Error speed limitation: %s\n", optarg);
                    return 1;
                }
                break;
            case 'a':
                check_io_uring = true;
                break;
            case 'h':
                help();
                return 0;
                break;
            default:
                try_help();
        }
    }

    if (check_io_uring) {
        if (setup_context(QD, &ring)) {
            printf("Do not support io_uring\n");
            exit(1);
        } else {
            printf("Support io_uring\n");
			io_uring_queue_exit(&ring);
            exit(0);
        }
    }

	if (!infd_path || !outfd_path) {
        printf("Enter the input file and output file\n");
		printf("%s -I infile -O outfile\n", iuc_program_name);
		return 1;
	}

	infd = open(infd_path, O_RDONLY);
	if (infd < 0) {
		perror("open infile");
		return 1;
	}
	outfd = open(outfd_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (outfd < 0) {
		perror("open outfile");
		return 1;
	}

	if (setup_context(QD, &ring)) {
        perror("io_uring set");
        return 1;
    }

	if (get_file_size(infd, &insize)) {
        perror("file size");
        return 1;
    }

	ret = copy_file(&ring, insize);

	close(infd);
	close(outfd);
	io_uring_queue_exit(&ring);
	return ret;
}